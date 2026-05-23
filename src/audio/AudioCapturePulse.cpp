#include "AudioCapturePulse.h"
#include "../utils/Logger.h"
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

AudioCapturePulse::AudioCapturePulse()
    : m_mainloop(nullptr), m_mainloopApi(nullptr), m_context(nullptr), m_stream(nullptr),
      m_sampleRate(44100), m_channels(2), m_bytesPerSample(2),
      m_isOpen(false), m_isCapturing(false)
{
    m_pipeSourceModuleIndex = PA_INVALID_INDEX;
    m_bus = std::make_unique<AudioBus>(m_sampleRate, m_channels);
    // ~2 s of slack at 44.1 kHz stereo. Encoder/recorder pulls every
    // video frame, so this is comfortably above expected fill.
    m_localTap = m_bus->createTap(static_cast<size_t>(m_sampleRate) * m_channels * 2);
}

AudioCapturePulse::~AudioCapturePulse()
{
    close();
    cleanupPulseAudio();
}

bool AudioCapturePulse::initializePulseAudio()
{
    if (m_mainloop)
    {
        return true; // Already initialized
    }

    m_mainloop = pa_mainloop_new();
    if (!m_mainloop)
    {
        LOG_ERROR("Failed to criar PulseAudio mainloop");
        return false;
    }

    m_mainloopApi = pa_mainloop_get_api(m_mainloop);
    m_context = pa_context_new(m_mainloopApi, "RetroCapture");

    if (!m_context)
    {
        LOG_ERROR("Failed to criar PulseAudio context");
        cleanupPulseAudio();
        return false;
    }

    pa_context_set_state_callback(m_context, contextStateCallback, this);

    pa_context_flags_t flags = PA_CONTEXT_NOFLAGS;
    if (pa_context_connect(m_context, nullptr, flags, nullptr) < 0)
    {
        LOG_ERROR("Failed to conectar ao PulseAudio: " + std::string(pa_strerror(pa_context_errno(m_context))));
        cleanupPulseAudio();
        return false;
    }

    return true;
}

void AudioCapturePulse::cleanupPulseAudio()
{
    stopCapture();
    if (m_monitor)
    {
        m_monitor->stop();
        m_monitor.reset();
    }
    stopPublishSource();
    disconnectRecordStream();

    // Best-effort GC of any pre-0.8 module-null-sink / module-loopback
    // we may have just inherited from an old session on this host.
    gcLegacyRetroCaptureModules();

    if (m_mainloop && m_context)
    {
        int ret = 0;
        for (int i = 0; i < 100; i++)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            usleep(10000);
        }
    }

    if (m_context)
    {
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
        m_context = nullptr;
    }

    if (m_mainloop)
    {
        pa_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
    }

    m_mainloopApi = nullptr;
}

void AudioCapturePulse::contextStateCallback(pa_context *c, void *userdata)
{
    (void)c;
    AudioCapturePulse *self = static_cast<AudioCapturePulse *>(userdata);
    self->contextStateChanged();
}

void AudioCapturePulse::contextStateChanged()
{
    if (!m_context)
    {
        return;
    }

    pa_context_state_t state = pa_context_get_state(m_context);

    switch (state)
    {
    case PA_CONTEXT_READY:
        LOG_INFO("PulseAudio context pronto");
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        LOG_ERROR("PulseAudio context falhou ou terminou");
        m_isOpen = false;
        break;
    default:
        break;
    }
}

void AudioCapturePulse::streamStateCallback(pa_stream *s, void *userdata)
{
    (void)s;
    AudioCapturePulse *self = static_cast<AudioCapturePulse *>(userdata);
    self->streamStateChanged();
}

void AudioCapturePulse::streamStateChanged()
{
    if (!m_stream)
    {
        return;
    }

    pa_stream_state_t state = pa_stream_get_state(m_stream);

    switch (state)
    {
    case PA_STREAM_READY:
        LOG_INFO("PulseAudio stream pronto");
        break;
    case PA_STREAM_FAILED:
    case PA_STREAM_TERMINATED:
        LOG_ERROR("PulseAudio stream falhou ou terminou");
        m_isCapturing = false;
        break;
    default:
        break;
    }
}

void AudioCapturePulse::streamReadCallback(pa_stream *s, size_t length, void *userdata)
{
    (void)s;
    AudioCapturePulse *self = static_cast<AudioCapturePulse *>(userdata);
    self->streamRead(length);
}

void AudioCapturePulse::streamRead(size_t length)
{
    (void)length;
    if (!m_stream)
    {
        return;
    }

    const void *data;
    size_t bytes;

    if (pa_stream_peek(m_stream, &data, &bytes) < 0)
    {
        LOG_ERROR("Failed to ler dados do stream PulseAudio");
        return;
    }

    if (data && bytes > 0)
    {
        size_t samples = bytes / m_bytesPerSample;
        const int16_t *sampleData = static_cast<const int16_t *>(data);

        if (m_bus)
        {
            m_bus->push(sampleData, samples);
        }

        if (m_audioCallback)
        {
            m_audioCallback(sampleData, samples);
        }
    }

    pa_stream_drop(m_stream);
}

void AudioCapturePulse::streamSuccessCallback(pa_stream *s, int success, void *userdata)
{
    (void)s;
    (void)success;
    (void)userdata;
}

bool AudioCapturePulse::open(const std::string &deviceName)
{
    if (m_isOpen)
    {
        LOG_WARN("AudioCapture already open");
        return true;
    }

    if (!initializePulseAudio())
    {
        return false;
    }

    m_deviceName = deviceName;

    int ret = 0;
    int maxIterations = 100;
    int iteration = 0;

    while (iteration < maxIterations)
    {
        pa_mainloop_iterate(m_mainloop, 0, &ret);

        pa_context_state_t state = pa_context_get_state(m_context);
        if (state == PA_CONTEXT_READY)
        {
            break;
        }
        else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
        {
            LOG_ERROR("Failed to connect to PulseAudio");
            return false;
        }

        usleep(10000);
        iteration++;
    }

    if (pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        LOG_ERROR("Timeout connecting to PulseAudio");
        return false;
    }

    // One-time GC of any module-null-sink / module-loopback left behind
    // by older RetroCapture binaries on this host. Idempotent against a
    // clean graph.
    gcLegacyRetroCaptureModules();

    if (!connectRecordStream(deviceName))
    {
        return false;
    }

    if (!startPublishSource())
    {
        // Publish failure is non-fatal — capture still works internally
        // for the encoder/recorder. Log loudly so the user understands
        // why other apps don't see `RetroCapture` in pactl.
        LOG_WARN("AudioCapture: failed to publish virtual source 'RetroCapture'; "
                 "other apps will not see it in the PulseAudio graph");
    }

    // Open the host-side monitor playback (the "output" half of the
    // RetroCapture I/O pair). ~2 s tap slack matches the other taps.
    // The two hardware clocks (capture device + default sink) will
    // drift in ppm over long sessions, accumulating in this tap; the
    // bus drop-oldest at the cap is the current safety net. Smooth
    // sample-rate-matching is a follow-up.
    {
        const size_t monitorCapacity =
            static_cast<size_t>(m_sampleRate) * m_channels * 2;
        auto monitorTap = m_bus->createTap(monitorCapacity);
        m_monitor = std::make_unique<MonitorPlayback>();
        if (!m_monitor->start(std::move(monitorTap), m_sampleRate, m_channels))
        {
            LOG_WARN("AudioCapture: monitor playback failed to start; "
                     "user will not hear the input through the default sink");
            m_monitor.reset();
        }
    }

    m_isOpen = true;
    LOG_INFO("AudioCapture opened: " + std::to_string(m_sampleRate) + "Hz, " +
             std::to_string(m_channels) + " channels, source=" +
             (deviceName.empty() ? "<default>" : deviceName));
    return true;
}

bool AudioCapturePulse::connectRecordStream(const std::string &device)
{
    if (!m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        LOG_ERROR("PulseAudio context not ready for connectRecordStream");
        return false;
    }

    disconnectRecordStream();

    pa_sample_spec sampleSpec;
    sampleSpec.format = PA_SAMPLE_S16LE;
    sampleSpec.rate = m_sampleRate;
    sampleSpec.channels = m_channels;

    pa_buffer_attr bufferAttr;
    bufferAttr.maxlength = static_cast<uint32_t>(-1);
    bufferAttr.tlength = static_cast<uint32_t>(-1);
    bufferAttr.prebuf = static_cast<uint32_t>(-1);
    bufferAttr.minreq = static_cast<uint32_t>(-1);
    bufferAttr.fragsize = static_cast<uint32_t>(m_sampleRate * m_bytesPerSample * m_channels / 10);

    const char *streamName = "RetroCapture Audio Capture";
    m_stream = pa_stream_new(m_context, streamName, &sampleSpec, nullptr);
    if (!m_stream)
    {
        LOG_ERROR("Failed to create PulseAudio stream");
        return false;
    }

    pa_stream_set_state_callback(m_stream, streamStateCallback, this);
    pa_stream_set_read_callback(m_stream, streamReadCallback, this);

    pa_stream_flags_t flags = static_cast<pa_stream_flags>(
        PA_STREAM_START_CORKED | PA_STREAM_ADJUST_LATENCY);

    const char *deviceArg = device.empty() ? nullptr : device.c_str();

    if (pa_stream_connect_record(m_stream, deviceArg, &bufferAttr, flags) < 0)
    {
        LOG_ERROR("Failed to connect record stream: " +
                  std::string(pa_strerror(pa_context_errno(m_context))));
        pa_stream_unref(m_stream);
        m_stream = nullptr;
        return false;
    }

    int ret = 0;
    int maxIterations = 100;
    int iteration = 0;
    while (iteration < maxIterations)
    {
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        pa_stream_state_t state = pa_stream_get_state(m_stream);
        if (state == PA_STREAM_READY)
        {
            break;
        }
        if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
        {
            LOG_ERROR("Record stream failed before reaching READY");
            pa_stream_unref(m_stream);
            m_stream = nullptr;
            return false;
        }
        usleep(10000);
        iteration++;
    }

    if (pa_stream_get_state(m_stream) != PA_STREAM_READY)
    {
        LOG_ERROR("Timeout opening record stream");
        pa_stream_unref(m_stream);
        m_stream = nullptr;
        return false;
    }

    m_currentInputSourceName = device;
    return true;
}

void AudioCapturePulse::disconnectRecordStream()
{
    if (!m_stream)
    {
        return;
    }
    pa_stream_disconnect(m_stream);
    pa_stream_unref(m_stream);
    m_stream = nullptr;
    m_isCapturing = false;
}

void AudioCapturePulse::close()
{
    if (!m_isOpen)
    {
        return;
    }

    stopCapture();
    if (m_monitor)
    {
        m_monitor->stop();
        m_monitor.reset();
    }
    stopPublishSource();
    disconnectRecordStream();
    m_currentInputSourceName.clear();
    m_isOpen = false;
    LOG_INFO("AudioCapture closed");
}

bool AudioCapturePulse::isOpen() const
{
    return m_isOpen;
}

bool AudioCapturePulse::startCapture()
{
    if (!m_isOpen)
    {
        LOG_ERROR("AudioCapture not open");
        return false;
    }

    if (m_isCapturing)
    {
        return true;
    }

    if (!m_stream)
    {
        LOG_ERROR("Stream unavailable");
        return false;
    }

    pa_operation *op = pa_stream_cork(m_stream, 0, streamSuccessCallback, this);
    if (op)
    {
        pa_operation_unref(op);
    }

    m_isCapturing = true;
    LOG_INFO("AudioCapture iniciado");

    return true;
}

void AudioCapturePulse::stopCapture()
{
    if (!m_isCapturing)
    {
        return;
    }

    if (m_stream)
    {
        pa_operation *op = pa_stream_cork(m_stream, 1, streamSuccessCallback, this);
        if (op)
        {
            pa_operation_unref(op);
        }
    }

    m_isCapturing = false;
    LOG_INFO("AudioCapture parado");
}

size_t AudioCapturePulse::getSamples(std::vector<float> &samples)
{
    // Drive the PA mainloop even if not open so reconnect/teardown
    // callbacks still fire.
    if (m_mainloop)
    {
        int ret = 0;
        for (int i = 0; i < 5; i++)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
        }
    }

    if (!m_localTap)
    {
        samples.clear();
        return 0;
    }

    const size_t available = m_localTap->available();
    if (available == 0)
    {
        samples.clear();
        return 0;
    }

    std::vector<int16_t> raw(available);
    const size_t pulled = m_localTap->pull(raw.data(), available);
    samples.resize(pulled);
    for (size_t i = 0; i < pulled; ++i)
    {
        samples[i] = static_cast<float>(raw[i]) / 32768.0f;
    }
    return pulled;
}

size_t AudioCapturePulse::getSamples(int16_t *buffer, size_t maxSamples)
{
    if (m_mainloop)
    {
        int ret = 0;
        for (int i = 0; i < 5; i++)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
        }
    }

    if (!m_isOpen || !buffer || maxSamples == 0 || !m_localTap)
    {
        return 0;
    }

    return m_localTap->pull(buffer, maxSamples);
}

uint32_t AudioCapturePulse::getSampleRate() const
{
    return m_sampleRate;
}

uint32_t AudioCapturePulse::getChannels() const
{
    return m_channels;
}

// Static variables for source listing (must be before callbacks that use them)
static std::vector<AudioDeviceInfo> g_availableSources;
static std::mutex g_sourcesMutex;

void AudioCapturePulse::sourceInfoCallback(pa_context *c, const pa_source_info *i, int eol, void *userdata)
{
    (void)c;
    (void)userdata;

    if (eol < 0)
    {
        return;
    }

    if (eol > 0)
    {
        // End of list
        return;
    }

    if (i)
    {
        // Only include non-monitor sources (monitors are virtual sources for sinks)
        // We want actual audio sources that can be connected to our sink
        if (i->monitor_of_sink == PA_INVALID_INDEX)
        {
            AudioDeviceInfo info;
            info.id = i->name ? i->name : "";
            info.name = i->description ? i->description : info.id;
            info.description = i->description ? i->description : "";
            info.available = (i->state == PA_SOURCE_RUNNING || i->state == PA_SOURCE_IDLE);

            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            g_availableSources.push_back(info);
        }
    }
}

void AudioCapturePulse::waitForContextReady()
{
    if (!m_context || !m_mainloop)
    {
        return;
    }

    int ret = 0;
    int maxIterations = 100;
    int iteration = 0;

    while (iteration < maxIterations)
    {
        pa_mainloop_iterate(m_mainloop, 0, &ret);

        pa_context_state_t state = pa_context_get_state(m_context);
        if (state == PA_CONTEXT_READY)
        {
            break;
        }
        else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
        {
            return;
        }

        usleep(10000); // 10ms
        iteration++;
    }
}

std::vector<AudioDeviceInfo> AudioCapturePulse::listInputSources()
{
    std::vector<AudioDeviceInfo> devices;

    if (!initializePulseAudio())
    {
        return devices;
    }

    waitForContextReady();

    if (pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        LOG_ERROR("PulseAudio context not ready for listing devices");
        return devices;
    }

    // Clear previous list
    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        g_availableSources.clear();
    }

    // Request source list
    pa_operation *op = pa_context_get_source_info_list(m_context, sourceInfoCallback, this);
    if (!op)
    {
        LOG_ERROR("Failed to request source list");
        return devices;
    }

    // Wait for operation to complete
    int ret = 0;
    int maxIterations = 100;
    int iteration = 0;

    while (iteration < maxIterations)
    {
        {
            std::lock_guard<std::mutex> mainloopLock(m_mainloopMutex);
            pa_mainloop_iterate(m_mainloop, 0, &ret);
        }

        // Check if operation is done (we can't directly check, so we wait a bit)
        if (iteration > 10) // Give it some time
        {
            pa_operation_state_t opState = pa_operation_get_state(op);
            if (opState == PA_OPERATION_DONE || opState == PA_OPERATION_CANCELLED)
            {
                break;
            }
        }

        usleep(10000); // 10ms
        iteration++;
    }

    pa_operation_unref(op);

    // Copy results
    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        devices = g_availableSources;
    }

    return devices;
}


std::vector<AudioDeviceInfo> AudioCapturePulse::listDevices()
{
    // For backward compatibility, return input sources
    return listInputSources();
}

void AudioCapturePulse::setDeviceStateCallback(std::function<void(const std::string &, bool)> callback)
{
    m_deviceStateCallback = callback;
}

std::vector<std::string> AudioCapturePulse::getAvailableDevices()
{
    std::vector<std::string> devices;

    // Use listInputSources() to get available audio sources
    std::vector<AudioDeviceInfo> sources = listInputSources();

    // Convert AudioDeviceInfo to std::string (use name for display)
    for (const auto &source : sources)
    {
        devices.push_back(source.name.empty() ? source.id : source.name);
    }

    return devices;
}

void AudioCapturePulse::setAudioCallback(std::function<void(const int16_t *data, size_t samples)> callback)
{
    m_audioCallback = callback;
}

// Globals for module-load / unload async ops. operationCallback writes
// g_moduleIndex when load returns; unloadModuleCallback writes
// g_sinkOperationSuccess.
static std::atomic<bool>     g_sinkOperationSuccess{false};
static std::atomic<uint32_t> g_moduleIndex{PA_INVALID_INDEX};

void AudioCapturePulse::operationCallback(pa_context *c, uint32_t index, void *userdata)
{
    (void)c;
    (void)userdata;
    g_moduleIndex = index;
    g_sinkOperationSuccess = (index != PA_INVALID_INDEX);
}

static void unloadModuleCallback(pa_context *c, int success, void *userdata)
{
    (void)c;
    (void)userdata;
    // In PulseAudio, success is 0 for success, non-zero for failure
    g_sinkOperationSuccess = (success == 0);
}

bool AudioCapturePulse::connectInputSource(const std::string &sourceName)
{
    if (sourceName.empty())
    {
        LOG_ERROR("Source name is empty");
        return false;
    }

    if (!m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        LOG_ERROR("PulseAudio context not ready");
        return false;
    }

    const bool wasCapturing = m_isCapturing;

    if (!connectRecordStream(sourceName))
    {
        return false;
    }

    if (wasCapturing)
    {
        startCapture();
    }

    LOG_INFO("AudioCapture input switched to: " + sourceName);
    return true;
}

void AudioCapturePulse::disconnectInputSource()
{
    if (!m_stream && m_currentInputSourceName.empty())
    {
        return;
    }

    disconnectRecordStream();
    m_currentInputSourceName.clear();
    LOG_INFO("AudioCapture input disconnected");
}


// Compact module scan: picks out any pre-0.8 module-null-sink that
// claimed sink_name=RetroCapture, and any module-loopback that fed it.
// Once both classes are unloaded, the pre-0.8 RetroCapture sink and its
// stray sink-inputs vanish on their own. Anything else is left alone.
// 0.8+ does not load any null-sink or loopback — capture is direct,
// publish goes via module-pipe-source, monitor playback is in-process
// (MonitorPlayback / pa_simple) — so on a clean 0.8+ graph this is a
// no-op.
static void legacyModuleInfoCallback(pa_context *c, const pa_module_info *i, int eol, void *userdata)
{
    (void)c;
    if (eol || !i || !i->name)
    {
        return;
    }
    auto *out = static_cast<std::vector<uint32_t> *>(userdata);
    const std::string name = i->name;
    const std::string args = i->argument ? i->argument : "";

    if (name == "module-null-sink" && args.find("sink_name=RetroCapture") != std::string::npos)
    {
        out->push_back(i->index);
        return;
    }
    if (name == "module-loopback" &&
        (args.find("sink=RetroCapture") != std::string::npos ||
         args.find("RetroCaptureInputLoopback") != std::string::npos))
    {
        out->push_back(i->index);
    }
}

void AudioCapturePulse::gcLegacyRetroCaptureModules()
{
    if (!m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        return;
    }

    std::vector<uint32_t> stale;
    pa_operation *op = pa_context_get_module_info_list(m_context, legacyModuleInfoCallback, &stale);
    if (op)
    {
        int ret = 0;
        for (int i = 0; i < 100; i++)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            pa_operation_state_t st = pa_operation_get_state(op);
            if (st == PA_OPERATION_DONE || st == PA_OPERATION_CANCELLED)
            {
                break;
            }
            usleep(10000);
        }
        pa_operation_unref(op);
    }

    for (uint32_t idx : stale)
    {
        g_sinkOperationSuccess = false;
        pa_operation *unloadOp = pa_context_unload_module(m_context, idx, unloadModuleCallback, this);
        if (!unloadOp)
        {
            continue;
        }
        int ret = 0;
        for (int i = 0; i < 50; i++)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            if (g_sinkOperationSuccess) break;
            usleep(10000);
        }
        pa_operation_unref(unloadOp);
    }

    if (!stale.empty())
    {
        LOG_INFO("gcLegacyRetroCaptureModules: unloaded " +
                 std::to_string(stale.size()) + " pre-0.8 module(s)");
    }
}

bool AudioCapturePulse::startPublishSource()
{
    if (m_publisher)
    {
        return true;
    }
    if (!m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY || !m_bus)
    {
        return false;
    }

    // Per-process temp dir keeps the FIFO away from anything the user
    // might create at /tmp, and lets us `rmdir` cleanly on shutdown.
    char dirTemplate[] = "/tmp/retrocapture-XXXXXX";
    if (!mkdtemp(dirTemplate))
    {
        LOG_ERROR("startPublishSource: mkdtemp failed: " +
                  std::string(std::strerror(errno)));
        return false;
    }
    m_fifoDir  = dirTemplate;
    m_fifoPath = m_fifoDir + "/RetroCapture.fifo";

    if (mkfifo(m_fifoPath.c_str(), 0600) != 0)
    {
        LOG_ERROR("startPublishSource: mkfifo(" + m_fifoPath + ") failed: " +
                  std::string(std::strerror(errno)));
        rmdir(m_fifoDir.c_str());
        m_fifoDir.clear();
        m_fifoPath.clear();
        return false;
    }

    // Format mirrors the bus exactly so a future DSP stage doesn't need
    // sample-rate conversion between input and publish.
    std::string args = "source_name=RetroCapture "
                       "file=" + m_fifoPath + " "
                       "format=s16le "
                       "rate=" + std::to_string(m_sampleRate) + " "
                       "channels=" + std::to_string(m_channels) + " "
                       "source_properties=device.description=\"RetroCapture\"";

    g_sinkOperationSuccess = false;
    g_moduleIndex          = PA_INVALID_INDEX;

    pa_operation *op = pa_context_load_module(m_context, "module-pipe-source",
                                              args.c_str(),
                                              operationCallback, this);
    if (!op)
    {
        LOG_ERROR("startPublishSource: pa_context_load_module failed: " +
                  std::string(pa_strerror(pa_context_errno(m_context))));
        unlink(m_fifoPath.c_str());
        rmdir(m_fifoDir.c_str());
        m_fifoPath.clear();
        m_fifoDir.clear();
        return false;
    }

    int ret = 0;
    for (int i = 0; i < 200; i++)
    {
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        if (g_sinkOperationSuccess && g_moduleIndex != PA_INVALID_INDEX)
        {
            break;
        }
        pa_operation_state_t st = pa_operation_get_state(op);
        if (st == PA_OPERATION_DONE || st == PA_OPERATION_CANCELLED)
        {
            break;
        }
        usleep(5000);
    }
    pa_operation_unref(op);

    if (!g_sinkOperationSuccess || g_moduleIndex == PA_INVALID_INDEX)
    {
        LOG_ERROR("startPublishSource: module-pipe-source load did not "
                  "complete successfully");
        unlink(m_fifoPath.c_str());
        rmdir(m_fifoDir.c_str());
        m_fifoPath.clear();
        m_fifoDir.clear();
        return false;
    }
    m_pipeSourceModuleIndex = g_moduleIndex;

    // ~2 s slack matches the local tap; drop-oldest if a downstream
    // pipe-source consumer stalls so we don't snowball memory.
    auto publishTap = m_bus->createTap(static_cast<size_t>(m_sampleRate) *
                                       m_channels * 2);

    m_publisher = std::make_unique<PipeSourcePublisher>();
    if (!m_publisher->start(m_fifoPath, std::move(publishTap)))
    {
        LOG_ERROR("startPublishSource: PipeSourcePublisher failed to open FIFO");
        m_publisher.reset();
        // Unload the module we just loaded so we don't leak a half-set-up source.
        g_sinkOperationSuccess = false;
        pa_operation *unload = pa_context_unload_module(m_context,
                                                        m_pipeSourceModuleIndex,
                                                        unloadModuleCallback,
                                                        this);
        if (unload)
        {
            for (int i = 0; i < 100; i++)
            {
                pa_mainloop_iterate(m_mainloop, 0, &ret);
                if (g_sinkOperationSuccess) break;
                usleep(5000);
            }
            pa_operation_unref(unload);
        }
        m_pipeSourceModuleIndex = PA_INVALID_INDEX;
        unlink(m_fifoPath.c_str());
        rmdir(m_fifoDir.c_str());
        m_fifoPath.clear();
        m_fifoDir.clear();
        return false;
    }

    LOG_INFO("Published virtual source 'RetroCapture' (fifo=" + m_fifoPath + ")");
    return true;
}

void AudioCapturePulse::resyncMonitor()
{
    if (m_monitor)
    {
        m_monitor->requestResync();
    }
}

void AudioCapturePulse::stopPublishSource()
{
    if (m_publisher)
    {
        m_publisher->stop();
        m_publisher.reset();
    }

    if (m_pipeSourceModuleIndex != PA_INVALID_INDEX && m_context &&
        pa_context_get_state(m_context) == PA_CONTEXT_READY)
    {
        g_sinkOperationSuccess = false;
        pa_operation *op = pa_context_unload_module(m_context,
                                                    m_pipeSourceModuleIndex,
                                                    unloadModuleCallback,
                                                    this);
        if (op)
        {
            int ret = 0;
            for (int i = 0; i < 100; i++)
            {
                pa_mainloop_iterate(m_mainloop, 0, &ret);
                if (g_sinkOperationSuccess) break;
                usleep(5000);
            }
            pa_operation_unref(op);
        }
    }
    m_pipeSourceModuleIndex = PA_INVALID_INDEX;

    if (!m_fifoPath.empty())
    {
        unlink(m_fifoPath.c_str());
        m_fifoPath.clear();
    }
    if (!m_fifoDir.empty())
    {
        rmdir(m_fifoDir.c_str());
        m_fifoDir.clear();
    }
}
