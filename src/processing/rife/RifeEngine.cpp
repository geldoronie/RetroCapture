// RIFE v4 on ncnn + Vulkan (#248).
//
// Adapted from rife-ncnn-vulkan (https://github.com/nihui/rife-ncnn-vulkan,
// MIT, Copyright (c) 2020 nihui): RIFE::process_v4() (non-TTA path), the
// "rife.Warp" custom layer (warp.cpp) and the compute shaders below. Changes:
// packed RGBA8 in/out (rife_preproc / rife_postproc), inputs uploaded and
// pre-processed once per frame and reused across phases and pairs, one
// command buffer per phase.
// ncnn's platform.h uses SRW locks / condition variables (Vista+); old
// MinGW headers (MXE) only declare them for _WIN32_WINNT >= 0x0600.
#if defined(_WIN32) && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "RifeEngine.h"

#include "gpu.h"
#include "layer.h"
#include "mat.h"
#include "net.h"
#include "pipeline.h"

#ifdef __APPLE__
#include <dlfcn.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <string>
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace
{
// ---------------------------------------------------------------- shaders
// GLSL 450 compute, compiled at load time by ncnn's glslang with ncnn's
// storage macros (sfp / afp / buffer_ld1 ...) defined for the device.

// Packed RGBA8 (or planar float RGB without 8-bit storage) -> planar
// normalised RGB, zero-padded to the network's 32-px granularity.
const char *kPreprocComp = R"(
#version 450
#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
layout (binding = 0) readonly buffer bottom_blob { uint8_t bottom_blob_data[]; };
#else
layout (binding = 0) readonly buffer bottom_blob { float bottom_blob_data[]; };
#endif
layout (binding = 1) writeonly buffer top_blob { sfp top_blob_data[]; };
layout (push_constant) uniform parameter
{
    int w;
    int h;
    int cstep;
    int outw;
    int outh;
    int outcstep;
} p;
void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.outw || gy >= p.outh || gz >= 3)
        return;
    if (gx >= p.w || gy >= p.h)
    {
        top_blob_data[gz * p.outcstep + gy * p.outw + gx] = sfp(0.f);
        return;
    }
#if NCNN_int8_storage
    float v = float(uint(bottom_blob_data[(gy * p.w + gx) * 4 + gz]));
#else
    float v = bottom_blob_data[gz * p.cstep + gy * p.w + gx];
#endif
    top_blob_data[gz * p.outcstep + gy * p.outw + gx] = sfp(v * (1.f / 255.f));
}
)";

// Planar network output -> packed RGBA8 (alpha 255), or planar float RGB.
const char *kPostprocComp = R"(
#version 450
#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_int8_storage
#extension GL_EXT_shader_8bit_storage: require
#endif
layout (binding = 0) readonly buffer bottom_blob { sfp bottom_blob_data[]; };
#if NCNN_int8_storage
layout (binding = 1) writeonly buffer top_blob { uint8_t top_blob_data[]; };
#else
layout (binding = 1) writeonly buffer top_blob { float top_blob_data[]; };
#endif
layout (push_constant) uniform parameter
{
    int w;
    int h;
    int cstep;
    int outw;
    int outh;
    int outcstep;
} p;
void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.outw || gy >= p.outh || gz >= 3)
        return;
    float v = float(bottom_blob_data[gz * p.cstep + gy * p.w + gx]);
#if NCNN_int8_storage
    uint v32 = clamp(uint(floor(v * 255.f + 0.5f)), 0, 255);
    int o = (gy * p.outw + gx) * 4;
    top_blob_data[o + gz] = uint8_t(v32);
    if (gz == 2)
        top_blob_data[o + 3] = uint8_t(255);
#else
    top_blob_data[gz * p.outcstep + gy * p.outw + gx] = v;
#endif
}
)";

// Timestep plane (RIFE v4 takes t as a third input image).
const char *kTimestepComp = R"(
#version 450
#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
layout (binding = 0) writeonly buffer top_blob { sfp top_blob_data[]; };
layout (push_constant) uniform parameter
{
    int w;
    int h;
    int cstep;
    float timestamp;
} p;
void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.w || gy >= p.h || gz >= 1)
        return;
    top_blob_data[gz * p.cstep + gy * p.w + gx] = sfp(p.timestamp);
}
)";

// rife.Warp: backward bilinear warp of an image by a flow field.
const char *kWarpComp = R"(
#version 450
#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_fp16_arithmetic
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
#endif
layout (binding = 0) readonly buffer image_blob { sfp image_blob_data[]; };
layout (binding = 1) readonly buffer flow_blob { sfp flow_blob_data[]; };
layout (binding = 2) writeonly buffer top_blob { sfp top_blob_data[]; };
layout (push_constant) uniform parameter
{
    int w;
    int h;
    int c;
    int cstep;
} p;
void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.w || gy >= p.h || gz >= p.c)
        return;
    afp flow_x = buffer_ld1(flow_blob_data, gy * p.w + gx);
    afp flow_y = buffer_ld1(flow_blob_data, p.cstep + gy * p.w + gx);
    afp sample_x = afp(gx) + flow_x;
    afp sample_y = afp(gy) + flow_y;
    int x0 = int(floor(sample_x));
    int y0 = int(floor(sample_y));
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    x0 = clamp(x0, 0, p.w - 1);
    y0 = clamp(y0, 0, p.h - 1);
    x1 = clamp(x1, 0, p.w - 1);
    y1 = clamp(y1, 0, p.h - 1);
    afp alpha = sample_x - afp(x0);
    afp beta = sample_y - afp(y0);
    afp v0 = buffer_ld1(image_blob_data, gz * p.cstep + y0 * p.w + x0);
    afp v1 = buffer_ld1(image_blob_data, gz * p.cstep + y0 * p.w + x1);
    afp v2 = buffer_ld1(image_blob_data, gz * p.cstep + y1 * p.w + x0);
    afp v3 = buffer_ld1(image_blob_data, gz * p.cstep + y1 * p.w + x1);
    afp v4 = v0 * (afp(1.f) - alpha) + v1 * alpha;
    afp v5 = v2 * (afp(1.f) - alpha) + v3 * alpha;
    afp v = v4 * (afp(1.f) - beta) + v5 * beta;
    buffer_st1(top_blob_data, gz * p.cstep + gy * p.w + gx, v);
}
)";

const char *kWarpPack4Comp = R"(
#version 450
#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_fp16_arithmetic
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
#endif
layout (binding = 0) readonly buffer image_blob { sfpvec4 image_blob_data[]; };
layout (binding = 1) readonly buffer flow_blob { sfp flow_blob_data[]; };
layout (binding = 2) writeonly buffer top_blob { sfpvec4 top_blob_data[]; };
layout (push_constant) uniform parameter
{
    int w;
    int h;
    int c;
    int cstep;
} p;
void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.w || gy >= p.h || gz >= p.c)
        return;
    afp flow_x = buffer_ld1(flow_blob_data, gy * p.w + gx);
    afp flow_y = buffer_ld1(flow_blob_data, p.cstep + gy * p.w + gx);
    afp sample_x = afp(gx) + flow_x;
    afp sample_y = afp(gy) + flow_y;
    int x0 = int(floor(sample_x));
    int y0 = int(floor(sample_y));
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    x0 = clamp(x0, 0, p.w - 1);
    y0 = clamp(y0, 0, p.h - 1);
    x1 = clamp(x1, 0, p.w - 1);
    y1 = clamp(y1, 0, p.h - 1);
    afp alpha = sample_x - afp(x0);
    afp beta = sample_y - afp(y0);
    afpvec4 v0 = buffer_ld4(image_blob_data, gz * p.cstep + y0 * p.w + x0);
    afpvec4 v1 = buffer_ld4(image_blob_data, gz * p.cstep + y0 * p.w + x1);
    afpvec4 v2 = buffer_ld4(image_blob_data, gz * p.cstep + y1 * p.w + x0);
    afpvec4 v3 = buffer_ld4(image_blob_data, gz * p.cstep + y1 * p.w + x1);
    afpvec4 v4 = v0 * (afp(1.f) - alpha) + v1 * alpha;
    afpvec4 v5 = v2 * (afp(1.f) - alpha) + v3 * alpha;
    afpvec4 v = v4 * (afp(1.f) - beta) + v5 * beta;
    buffer_st4(top_blob_data, gz * p.cstep + gy * p.w + gx, v);
}
)";

ncnn::Pipeline *makePipeline(const ncnn::VulkanDevice *vkdev, const char *src, const ncnn::Option &opt, int lx,
                             int ly, int lz)
{
    std::vector<uint32_t> spirv;
    if (ncnn::compile_spirv_module(src, opt, spirv) != 0 || spirv.empty())
        return nullptr;
    ncnn::Pipeline *p = new ncnn::Pipeline(vkdev);
    if (lx > 0)
        p->set_optimal_local_size_xyz(lx, ly, lz);
    else
        p->set_optimal_local_size_xyz();
    std::vector<ncnn::vk_specialization_type> spec;
    if (p->create(spirv.data(), spirv.size() * 4, spec) != 0)
    {
        delete p;
        return nullptr;
    }
    return p;
}

// --------------------------------------------------------- rife.Warp layer
class Warp : public ncnn::Layer
{
public:
    Warp() { support_vulkan = true; }

    int create_pipeline(const ncnn::Option &opt) override
    {
        if (!vkdev) return 0;
        m_pack1 = makePipeline(vkdev, kWarpComp, opt, 0, 0, 0);
        m_pack4 = makePipeline(vkdev, kWarpPack4Comp, opt, 0, 0, 0);
        return (m_pack1 && m_pack4) ? 0 : -1;
    }
    int destroy_pipeline(const ncnn::Option &) override
    {
        delete m_pack1;
        delete m_pack4;
        m_pack1 = m_pack4 = nullptr;
        return 0;
    }

    // CPU path (used by ncnn only if the GPU path is unavailable).
    int forward(const std::vector<ncnn::Mat> &bottom, std::vector<ncnn::Mat> &top,
                const ncnn::Option &) const override
    {
        const ncnn::Mat &image = bottom[0];
        const ncnn::Mat &flow = bottom[1];
        const int w = image.w, h = image.h, channels = image.c;
        ncnn::Mat &out = top[0];
        out.create(w, h, channels);
        if (out.empty()) return -100;
        for (int q = 0; q < channels; q++)
        {
            float *o = out.channel(q);
            const ncnn::Mat img = image.channel(q);
            const float *fx = flow.channel(0);
            const float *fy = flow.channel(1);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    const float sx = x + *fx++;
                    const float sy = y + *fy++;
                    int x0 = static_cast<int>(std::floor(sx));
                    int y0 = static_cast<int>(std::floor(sy));
                    int x1 = x0 + 1, y1 = y0 + 1;
                    x0 = std::min(std::max(x0, 0), w - 1);
                    y0 = std::min(std::max(y0, 0), h - 1);
                    x1 = std::min(std::max(x1, 0), w - 1);
                    y1 = std::min(std::max(y1, 0), h - 1);
                    const float a = sx - x0, b = sy - y0;
                    const float v4 = img.row(y0)[x0] * (1 - a) + img.row(y0)[x1] * a;
                    const float v5 = img.row(y1)[x0] * (1 - a) + img.row(y1)[x1] * a;
                    *o++ = v4 * (1 - b) + v5 * b;
                }
        }
        return 0;
    }

    int forward(const std::vector<ncnn::VkMat> &bottom, std::vector<ncnn::VkMat> &top, ncnn::VkCompute &cmd,
                const ncnn::Option &opt) const override
    {
        const ncnn::VkMat &image = bottom[0];
        const ncnn::VkMat &flow = bottom[1];
        ncnn::VkMat &out = top[0];
        out.create(image.w, image.h, image.c, image.elemsize, image.elempack, opt.blob_vkallocator);
        if (out.empty()) return -100;
        std::vector<ncnn::VkMat> bindings(3);
        bindings[0] = image;
        bindings[1] = flow;
        bindings[2] = out;
        std::vector<ncnn::vk_constant_type> constants(4);
        constants[0].i = out.w;
        constants[1].i = out.h;
        constants[2].i = out.c;
        constants[3].i = static_cast<int>(out.cstep);
        if (image.elempack == 4)
            cmd.record_pipeline(m_pack4, bindings, constants, out);
        else if (image.elempack == 1)
            cmd.record_pipeline(m_pack1, bindings, constants, out);
        else
            return -1;
        return 0;
    }

private:
    ncnn::Pipeline *m_pack1 = nullptr;
    ncnn::Pipeline *m_pack4 = nullptr;
};

DEFINE_LAYER_CREATOR(Warp)

std::mutex g_instanceMutex;
bool g_instanceTried = false;
RifeEngine::DeviceInfo g_device;

bool fileExists(const std::string &p)
{
    std::ifstream f(p, std::ios::binary);
    return static_cast<bool>(f);
}
} // namespace

// ------------------------------------------------------------------ engine
struct RifeEngine::Impl
{
    ncnn::VulkanDevice *vkdev = nullptr;
    ncnn::Net net;
    ncnn::Option opt;
    ncnn::VkAllocator *blobAlloc = nullptr;
    ncnn::VkAllocator *stagingAlloc = nullptr;
    ncnn::Pipeline *preproc = nullptr;
    ncnn::Pipeline *postproc = nullptr;
    ncnn::Pipeline *timestep = nullptr;
    bool int8 = false; // packed RGBA8 in/out on the GPU
    bool ok = false;

    // The two most recent frames, pre-processed (planar, padded).
    struct Held
    {
        uint64_t id = 0;
        ncnn::VkMat padded;
    };
    Held held[2];
    int next = 0;
    int w = 0, h = 0, wp = 0, hp = 0;

    const ncnn::VkMat *find(uint64_t id) const
    {
        for (const Held &x : held)
            if (x.id == id && !x.padded.empty()) return &x.padded;
        return nullptr;
    }

    ~Impl()
    {
        held[0].padded.release();
        held[1].padded.release();
        delete preproc;
        delete postproc;
        delete timestep;
        net.clear();
        if (vkdev)
        {
            if (blobAlloc) vkdev->reclaim_blob_allocator(blobAlloc);
            if (stagingAlloc) vkdev->reclaim_staging_allocator(stagingAlloc);
        }
    }
};

RifeEngine::RifeEngine() : d(new Impl) {}
RifeEngine::~RifeEngine() = default;

RifeEngine::DeviceInfo RifeEngine::probe()
{
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    if (g_instanceTried) return g_device;
    g_instanceTried = true;
    // ncnn loads the Vulkan loader itself (NCNN_SIMPLEVK): no link-time
    // dependency, a missing libvulkan / vulkan-1.dll / MoltenVK just fails here.
    const char *driver = nullptr;
#ifdef __APPLE__
    // dlopen("libvulkan.dylib") does not search Homebrew's prefix on Apple
    // Silicon; MoltenVK also works loaded directly as the driver. #255 — the
    // .app bundles MoltenVK in Contents/Frameworks: tried first, so RIFE
    // works without a Vulkan install.
    static std::string bundled;
    {
        char exe[4096];
        uint32_t size = sizeof(exe);
        if (_NSGetExecutablePath(exe, &size) == 0)
        {
            std::string dir(exe);
            const size_t slash = dir.rfind('/');
            if (slash != std::string::npos)
                bundled = dir.substr(0, slash) + "/../Frameworks/libMoltenVK.dylib";
        }
        const char *cands[] = {bundled.empty() ? nullptr : bundled.c_str(),
                               "libvulkan.dylib",
                               "libvulkan.1.dylib",
                               "/opt/homebrew/lib/libvulkan.1.dylib",
                               "/usr/local/lib/libvulkan.1.dylib",
                               "/opt/homebrew/lib/libMoltenVK.dylib",
                               "/usr/local/lib/libMoltenVK.dylib"};
        for (const char *c : cands)
        {
            if (!c) continue;
            if (void *h = dlopen(c, RTLD_LOCAL | RTLD_NOW))
            {
                dlclose(h);
                driver = c;
                break;
            }
        }
    }
#endif
    if (ncnn::create_gpu_instance(driver) != 0)
    {
        g_device.reason = "no Vulkan loader or driver (Vulkan instance creation failed)";
        return g_device;
    }
    const int n = ncnn::get_gpu_count();
    if (n <= 0)
    {
        g_device.reason = "no Vulkan device found";
        return g_device;
    }
    const int idx = ncnn::get_default_gpu_index();
    const ncnn::GpuInfo &info = ncnn::get_gpu_info(idx);
    g_device.ok = true;
    g_device.index = idx;
    g_device.name = info.device_name();
    return g_device;
}

void RifeEngine::shutdownVulkan()
{
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    if (g_instanceTried && g_device.ok)
        ncnn::destroy_gpu_instance();
    g_instanceTried = false;
    g_device = DeviceInfo();
}

bool RifeEngine::loaded() const { return d->ok; }

bool RifeEngine::load(const std::string &modelDir, int gpuIndex, std::string &err)
{
    const std::string param = modelDir + "/flownet.param";
    const std::string bin = modelDir + "/flownet.bin";
    if (!fileExists(param) || !fileExists(bin))
    {
        err = "RIFE model not found (" + param + ")";
        return false;
    }
    d->vkdev = ncnn::get_gpu_device(gpuIndex);
    if (!d->vkdev)
    {
        err = "Vulkan device unavailable";
        return false;
    }
    const ncnn::GpuInfo &info = d->vkdev->info;

    ncnn::Option opt;
    opt.num_threads = 1;
    opt.use_vulkan_compute = true;
    opt.use_fp16_packed = info.support_fp16_packed();
    opt.use_fp16_storage = info.support_fp16_storage();
    opt.use_fp16_arithmetic = false; // as upstream: fp16 math hurts RIFE's flow
    opt.use_int8_storage = info.support_int8_storage();
    opt.use_bf16_storage = false;
    opt.use_bf16_packed = false;
    d->int8 = opt.use_int8_storage;

    d->net.opt = opt;
    d->net.set_vulkan_device(d->vkdev);
    d->net.register_custom_layer("rife.Warp", Warp_layer_creator);
    if (d->net.load_param(param.c_str()) != 0 || d->net.load_model(bin.c_str()) != 0)
    {
        err = "could not load the RIFE model from " + modelDir;
        return false;
    }
    d->opt = d->net.opt;
    // Keep our shaders' storage types in line with what the net ended up using.
    d->opt.use_int8_storage = d->int8;

    d->preproc = makePipeline(d->vkdev, kPreprocComp, d->opt, 8, 8, 3);
    d->postproc = makePipeline(d->vkdev, kPostprocComp, d->opt, 8, 8, 3);
    d->timestep = makePipeline(d->vkdev, kTimestepComp, d->opt, 8, 8, 1);
    if (!d->preproc || !d->postproc || !d->timestep)
    {
        err = "RIFE compute shaders failed to compile";
        return false;
    }
    d->blobAlloc = d->vkdev->acquire_blob_allocator();
    d->stagingAlloc = d->vkdev->acquire_staging_allocator();
    d->opt.blob_vkallocator = d->blobAlloc;
    d->opt.workspace_vkallocator = d->blobAlloc;
    d->opt.staging_vkallocator = d->stagingAlloc;
    d->ok = true;
    return true;
}

void RifeEngine::forgetFrames()
{
    for (Impl::Held &x : d->held)
    {
        x.id = 0;
        x.padded.release();
    }
}

bool RifeEngine::uploadFrame(uint64_t id, const uint8_t *rgba, int w, int h, std::string &err)
{
    if (!d->ok)
    {
        err = "RIFE not loaded";
        return false;
    }
    if (w != d->w || h != d->h)
    {
        forgetFrames();
        d->w = w;
        d->h = h;
        d->wp = (w + 31) / 32 * 32;
        d->hp = (h + 31) / 32 * 32;
    }
    const ncnn::Option &opt = d->opt;
    const size_t elemsize = opt.use_fp16_storage ? 2u : 4u;

    ncnn::VkCompute cmd(d->vkdev);
    ncnn::VkMat in;
    ncnn::Mat host;
    if (d->int8)
    {
        // Bytes as they are: a w*4 x h matrix of 1-byte elements (no cast).
        host = ncnn::Mat(w * 4, h, const_cast<uint8_t *>(rgba), static_cast<size_t>(1), 1);
    }
    else
    {
        host = ncnn::Mat::from_pixels(rgba, ncnn::Mat::PIXEL_RGBA2RGB, w, h);
    }
    cmd.record_clone(host, in, opt);

    Impl::Held &slot = d->held[d->next];
    d->next = 1 - d->next;
    slot.id = 0;
    slot.padded.create(d->wp, d->hp, 3, elemsize, 1, d->blobAlloc);
    if (slot.padded.empty())
    {
        err = "out of GPU memory";
        return false;
    }
    std::vector<ncnn::VkMat> bindings(2);
    bindings[0] = in;
    bindings[1] = slot.padded;
    std::vector<ncnn::vk_constant_type> constants(6);
    constants[0].i = w;
    constants[1].i = h;
    constants[2].i = d->int8 ? 0 : static_cast<int>(in.cstep);
    constants[3].i = slot.padded.w;
    constants[4].i = slot.padded.h;
    constants[5].i = static_cast<int>(slot.padded.cstep);
    cmd.record_pipeline(d->preproc, bindings, constants, slot.padded);
    if (cmd.submit_and_wait() != 0)
    {
        err = "Vulkan submit failed (upload)";
        return false;
    }
    slot.id = id;
    return true;
}

bool RifeEngine::interpolate(uint64_t idA, uint64_t idB, float t, uint8_t *out, std::string &err)
{
    const ncnn::VkMat *a = d->find(idA);
    const ncnn::VkMat *b = d->find(idB);
    if (!d->ok || !a || !b)
    {
        err = "frames not uploaded";
        return false;
    }
    const ncnn::Option &opt = d->opt;
    const size_t elemsize = opt.use_fp16_storage ? 2u : 4u;
    const int w = d->w, h = d->h;

    ncnn::VkCompute cmd(d->vkdev);
    ncnn::VkMat ts;
    ts.create(d->wp, d->hp, 1, elemsize, 1, d->blobAlloc);
    {
        std::vector<ncnn::VkMat> bindings(1);
        bindings[0] = ts;
        std::vector<ncnn::vk_constant_type> constants(4);
        constants[0].i = ts.w;
        constants[1].i = ts.h;
        constants[2].i = static_cast<int>(ts.cstep);
        constants[3].f = t;
        cmd.record_pipeline(d->timestep, bindings, constants, ts);
    }

    ncnn::VkMat outPadded;
    {
        ncnn::Extractor ex = d->net.create_extractor();
        ex.set_blob_vkallocator(d->blobAlloc);
        ex.set_workspace_vkallocator(d->blobAlloc);
        ex.set_staging_vkallocator(d->stagingAlloc);
        ex.input("in0", *a);
        ex.input("in1", *b);
        ex.input("in2", ts);
        if (ex.extract("out0", outPadded, cmd) != 0 || outPadded.empty())
        {
            err = "RIFE inference failed";
            return false;
        }
    }

    ncnn::VkMat outGpu;
    if (d->int8)
        outGpu.create(w * 4, h, static_cast<size_t>(1), 1, d->blobAlloc);
    else
        outGpu.create(w, h, 3, static_cast<size_t>(4), 1, d->blobAlloc);
    {
        std::vector<ncnn::VkMat> bindings(2);
        bindings[0] = outPadded;
        bindings[1] = outGpu;
        std::vector<ncnn::vk_constant_type> constants(6);
        constants[0].i = outPadded.w;
        constants[1].i = outPadded.h;
        constants[2].i = static_cast<int>(outPadded.cstep);
        constants[3].i = w;
        constants[4].i = h;
        constants[5].i = d->int8 ? 0 : static_cast<int>(outGpu.cstep);
        ncnn::VkMat dispatcher;
        dispatcher.w = w;
        dispatcher.h = h;
        dispatcher.c = 3;
        cmd.record_pipeline(d->postproc, bindings, constants, dispatcher);
    }

    ncnn::Mat host;
    if (d->int8)
        host = ncnn::Mat(w * 4, h, out, static_cast<size_t>(1), 1);
    cmd.record_clone(outGpu, host, opt);
    if (cmd.submit_and_wait() != 0)
    {
        err = "Vulkan submit failed (inference)";
        return false;
    }
    if (!d->int8)
        host.to_pixels(out, ncnn::Mat::PIXEL_RGB2RGBA);
    else if (host.data != out)
        std::memcpy(out, host.data, static_cast<size_t>(w) * h * 4);
    return true;
}
