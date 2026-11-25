#pragma once

#include <cstdint>
#include <memory>
#include <string>

/**
 * RIFE 4.x frame interpolation on ncnn + Vulkan (#248). No OpenGL here: the
 * caller hands in RGBA8 frames in host memory and gets RGBA8 frames back.
 * Only built with ENABLE_RIFE; the ncnn types stay inside the .cpp.
 *
 * Adapted from rife-ncnn-vulkan (MIT, Copyright (c) 2020 nihui): the RIFE v4
 * graph runner, its "rife.Warp" custom layer and its compute shaders. The
 * pre/post-processing shaders were changed to read/write packed RGBA8, so a
 * frame read back from OpenGL is uploaded as is (no CPU repacking).
 *
 * Usage (one thread at a time; the worker thread in RifeBackend):
 *   probe() once per process, load() once, then for every real frame
 *   uploadFrame(id, rgba), and interpolate(idA, idB, t, out) for each phase.
 *   The two most recent uploads stay on the GPU, so with a stream of frames
 *   every frame is uploaded and pre-processed exactly once.
 */
class RifeEngine
{
public:
    struct DeviceInfo
    {
        bool ok = false;
        int index = -1;
        std::string name;   // e.g. "AMD Radeon RX 9070 XT (RADV GFX1201)"
        std::string reason; // why ok is false
    };

    /** Creates the process-wide Vulkan instance (once) and picks the default GPU. */
    static DeviceInfo probe();
    /** Destroys the Vulkan instance; every RifeEngine must be gone. */
    static void shutdownVulkan();

    RifeEngine();
    ~RifeEngine();
    RifeEngine(const RifeEngine &) = delete;
    RifeEngine &operator=(const RifeEngine &) = delete;

    /** modelDir holds flownet.param / flownet.bin (a RIFE v4.x model). */
    bool load(const std::string &modelDir, int gpuIndex, std::string &err);
    bool loaded() const;

    /** Upload a real frame (RGBA8, w*h*4 bytes, tightly packed). */
    bool uploadFrame(uint64_t id, const uint8_t *rgba, int w, int h, std::string &err);
    /**
     * Frame at phase t (0 = A, 1 = B) between two uploaded frames, written
     * to out (RGBA8, w*h*4, alpha = 255). Blocks until the GPU is done.
     */
    bool interpolate(uint64_t idA, uint64_t idB, float t, uint8_t *out, std::string &err);
    /** Drop the frames held on the GPU (keeps the model). */
    void forgetFrames();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
