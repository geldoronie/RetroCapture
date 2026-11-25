#include "RifeBackend.h"
#include "../../utils/Logger.h"
#include "../../utils/Paths.h"
#ifdef RETROCAPTURE_RIFE
#include "RifeEngine.h"
#else
class RifeEngine // built without ENABLE_RIFE: never instantiated
{
};
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER_BINDING
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif

namespace
{
// The model shipped with the app (see assets/models/README / CMake).
constexpr const char *kModelName = "rife-v4.6";

int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void ema(double &acc, double x)
{
    acc = (acc <= 0.0) ? x : acc + (x - acc) * 0.1;
}

bool samePhase(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

#ifdef RETROCAPTURE_RIFE
// The model directory: RETROCAPTURE_RIFE_MODEL_DIR, else next to the other
// read-only assets (dev tree / portable: <root>/assets/models; Linux system
// install flattens assets/ into share/retrocapture/: <root>/models), else
// next to the executable.
std::string findModelDir()
{
    // Plain strings + fopen: the Windows cross build (GCC 5) has no <filesystem>.
    auto exists = [](const std::string &f) {
        FILE *fp = std::fopen(f.c_str(), "rb");
        if (fp) std::fclose(fp);
        return fp != nullptr;
    };
    auto ok = [&](const std::string &p) { return exists(p + "/flownet.param") && exists(p + "/flownet.bin"); };
    auto parent = [](std::string p) {
        while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
        const size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? std::string(".") : p.substr(0, s);
    };
    if (const char *env = std::getenv("RETROCAPTURE_RIFE_MODEL_DIR"))
    {
        if (ok(env)) return env;
    }
    const std::string m = std::string("/models/") + kModelName;
    const std::string root = Paths::getReadOnlyAssetsDir();
    const std::string exe = Paths::getExecutableDir();
    const std::string cands[] = {
        root + "/assets" + m,
        root + m,
        exe + "/assets" + m,
        parent(exe) + "/share/retrocapture" + m,
        parent(exe) + "/share/retrocapture/assets" + m,
    };
    for (const std::string &p : cands)
    {
        if (ok(p)) return p;
    }
    return {};
}
#endif
} // namespace

RifeBackend::RifeBackend() = default;

RifeBackend::~RifeBackend()
{
    shutdown();
}

bool RifeBackend::compiledIn()
{
#ifdef RETROCAPTURE_RIFE
    return true;
#else
    return false;
#endif
}

void RifeBackend::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started) return;
    m_started = true;
#ifdef RETROCAPTURE_RIFE
    m_state = State::Loading;
    m_stop = false;
    m_worker = std::thread(&RifeBackend::workerMain, this);
#else
    m_state = State::Unavailable;
    m_reason = "this build has no RIFE support (built without ncnn/Vulkan)";
#endif
}

RifeBackend::State RifeBackend::state() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

std::string RifeBackend::reason() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_reason;
}

std::string RifeBackend::device() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_device;
}

RifeBackend::Stats RifeBackend::stats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

bool RifeBackend::waitReady(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_doneCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                      [&] { return m_state == State::Ready || m_state == State::Unavailable; });
    return m_state == State::Ready;
}

void RifeBackend::submitFrame(GLuint fbo, uint32_t w, uint32_t h)
{
    if (!fbo || w == 0 || h == 0) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != State::Ready) return;
    }

    auto f = std::make_shared<Frame>();
    // Recycle a buffer the worker is done with (8 MB at 1080p).
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pool.empty())
        {
            f->rgba.swap(m_pool.back());
            m_pool.pop_back();
        }
    }
    f->w = w;
    f->h = h;
    f->rgba.resize(static_cast<size_t>(w) * h * 4);

    // Host round trip, GL side: read the frame back (rows bottom-up; the
    // result is written back the same way, so the orientation round-trips).
    const auto t0 = std::chrono::steady_clock::now();
    GLint prevFbo = 0, prevPack = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
    if (prevPack) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE,
                 f->rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    if (prevPack) glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPack));
    const double readMs = msSince(t0);
    f->submittedUs = nowUs();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ema(m_stats.readbackMs, readMs);
        f->id = m_nextId++;
        if (w != m_w || h != m_h)
        {
            m_w = w;
            m_h = h;
            m_pairB = 0;
            m_phasesCur.clear();
            m_phasesPrev.clear();
        }
        m_pairA = m_pairB;
        m_pairB = f->id;
        m_pairSubmittedUs = f->submittedUs;
        m_pairFirstDone = false;
        for (Result &r : m_results)
            m_pool.push_back(std::move(r.rgba));
        m_results.clear();
        m_requests.clear();
        notePairPhasesLocked();
        m_uploads.push_back(std::move(f));
        // A worker that falls behind only needs the last two frames.
        while (m_uploads.size() > 2) m_uploads.pop_front();
    }
    m_cv.notify_one();
}

void RifeBackend::notePairPhasesLocked()
{
    // Fixed-multiple modes ask for the same phases every pair: once two
    // consecutive pairs agree, request them up front so the worker starts
    // as soon as the frame is uploaded. Display mode (phases change every
    // pair) stays on demand.
    const bool stable = !m_phasesCur.empty() && m_phasesCur == m_phasesPrev;
    m_phasesPrev = m_phasesCur;
    m_phasesCur.clear();
    if (stable && m_pairA != 0)
    {
        m_requests = m_phasesPrev;
        m_phasesCur = m_phasesPrev;
    }
}

void RifeBackend::request(float t)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != State::Ready || m_pairA == 0) return;
        bool known = false;
        for (float r : m_requests)
            if (samePhase(r, t)) known = true;
        if (!known) m_requests.push_back(t);
        known = false;
        for (float r : m_phasesCur)
            if (samePhase(r, t)) known = true;
        if (!known) m_phasesCur.push_back(t);
    }
    m_cv.notify_one();
}

bool RifeBackend::hasResultLocked(float t) const
{
    for (const Result &r : m_results)
        if (r.pairB == m_pairB && samePhase(r.t, t)) return true;
    return false;
}

bool RifeBackend::fetch(float t, int waitMs, GLuint tex)
{
    request(t);
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_state != State::Ready || m_pairA == 0) return false;
    const uint64_t pair = m_pairB;
    auto ready = [&] { return m_stop || m_pairB != pair || hasResultLocked(t); };
    if (waitMs < 0)
        m_doneCv.wait(lock, ready);
    else
        m_doneCv.wait_for(lock, std::chrono::milliseconds(waitMs), ready);
    const Result *res = nullptr;
    for (const Result &r : m_results)
        if (r.pairB == pair && samePhase(r.t, t)) res = &r;
    if (!res || m_pairB != pair)
    {
        ++m_stats.late;
        return false;
    }
    if (res->failed || res->w != m_w || res->h != m_h) return false;

    // Host round trip, way back: one texture upload (under the lock, so the
    // worker cannot recycle the buffer meanwhile).
    const auto t0 = std::chrono::steady_clock::now();
    GLint prevTex = 0, prevUnpack = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevUnpack);
    if (prevUnpack) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(res->w), static_cast<GLsizei>(res->h), GL_RGBA,
                    GL_UNSIGNED_BYTE, res->rgba.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    if (prevUnpack) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(prevUnpack));
    ema(m_stats.uploadMs, msSince(t0));
    ++m_stats.delivered;
    return true;
}

void RifeBackend::reset()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pairA = m_pairB = 0;
        m_uploads.clear();
        for (Result &r : m_results)
            m_pool.push_back(std::move(r.rgba));
        m_results.clear();
        m_requests.clear();
        m_phasesCur.clear();
        m_phasesPrev.clear();
        m_resetPending = true;
    }
    m_cv.notify_one();
}

void RifeBackend::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;
        m_stop = true;
    }
    m_cv.notify_all();
    m_doneCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_started = false;
    m_state = State::Idle;
    m_uploads.clear();
    m_results.clear();
    m_requests.clear();
    m_pool.clear();
    m_pairA = m_pairB = 0;
}

void RifeBackend::workerMain()
{
#ifdef RETROCAPTURE_RIFE
    auto fail = [&](const std::string &why) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = State::Unavailable;
        m_reason = why;
        LOG_WARN("RIFE frame generation unavailable: " + why);
        m_doneCv.notify_all();
    };

    const auto tLoad = std::chrono::steady_clock::now();
    const RifeEngine::DeviceInfo dev = RifeEngine::probe();
    if (!dev.ok)
    {
        fail(dev.reason);
        return;
    }
    const std::string modelDir = findModelDir();
    if (modelDir.empty())
    {
        fail(std::string("model files not found (") + kModelName + "/flownet.param)");
        return;
    }
    {
        auto engine = std::make_unique<RifeEngine>();
        std::string err;
        if (!engine->load(modelDir, dev.index, err))
        {
            fail(err);
            return;
        }
        m_engine = std::move(engine);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = State::Ready;
        m_device = dev.name;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f ms", msSince(tLoad));
        LOG_INFO("RIFE frame generation ready on \"" + dev.name + "\" (" + modelDir + ", loaded in " + buf + ")");
        m_doneCv.notify_all();
    }

    uint64_t uploaded[2] = {0, 0};
    int nextUploaded = 0;
    auto isUploaded = [&](uint64_t id) { return id != 0 && (uploaded[0] == id || uploaded[1] == id); };
    std::string err;

    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        // Next phase of the current pair still to compute (both frames on the GPU).
        auto pendingPhase = [&](float &t) {
            if (m_pairA == 0 || !isUploaded(m_pairA) || !isUploaded(m_pairB)) return false;
            for (float r : m_requests)
                if (!hasResultLocked(r))
                {
                    t = r;
                    return true;
                }
            return false;
        };
        float t = 0.0f;
        m_cv.wait(lock, [&] { return m_stop || m_resetPending || !m_uploads.empty() || pendingPhase(t); });
        if (m_stop) break;

        if (m_resetPending)
        {
            m_resetPending = false;
            uploaded[0] = uploaded[1] = 0;
            lock.unlock();
            m_engine->forgetFrames();
            lock.lock();
            continue;
        }

        if (!m_uploads.empty())
        {
            std::shared_ptr<Frame> f = m_uploads.front();
            m_uploads.pop_front();
            lock.unlock();
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = m_engine->uploadFrame(f->id, f->rgba.data(), static_cast<int>(f->w),
                                                  static_cast<int>(f->h), err);
            const double ms = msSince(t0);
            lock.lock();
            ema(m_stats.frameUploadMs, ms);
            if (ok)
            {
                uploaded[nextUploaded] = f->id;
                nextUploaded = 1 - nextUploaded;
            }
            else
            {
                LOG_WARN("RIFE: frame upload failed: " + err);
            }
            m_pool.push_back(std::move(f->rgba));
            continue;
        }

        if (!pendingPhase(t)) continue;
        const uint64_t a = m_pairA, b = m_pairB;
        const uint32_t w = m_w, h = m_h;
        std::vector<uint8_t> buf;
        if (!m_pool.empty())
        {
            buf.swap(m_pool.back());
            m_pool.pop_back();
        }
        lock.unlock();
        buf.resize(static_cast<size_t>(w) * h * 4);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = m_engine->interpolate(a, b, t, buf.data(), err);
        const double ms = msSince(t0);
        lock.lock();
        if (!ok) LOG_WARN("RIFE: inference failed: " + err);
        ema(m_stats.inferMs, ms);
        if (m_pairB == b)
        {
            if (!m_pairFirstDone)
            {
                m_pairFirstDone = true;
                ema(m_stats.latencyMs, static_cast<double>(nowUs() - m_pairSubmittedUs) / 1000.0);
            }
            Result r;
            r.pairB = b;
            r.t = t;
            r.w = w;
            r.h = h;
            r.failed = !ok;
            r.rgba = std::move(buf);
            m_results.push_back(std::move(r));
            m_doneCv.notify_all();
        }
        else
        {
            m_pool.push_back(std::move(buf));
        }
    }
    lock.unlock();
    m_engine.reset();
    RifeEngine::shutdownVulkan();
#endif
}
