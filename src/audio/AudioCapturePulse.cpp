#include "AudioCapturePulse.h"
#include "../utils/Logger.h"
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <unistd.h>

AudioCapturePulse::AudioCapturePulse()
    : m_mainloop(nullptr)
    , m_mainloopApi(nullptr)
    , m_context(nullptr)
    , m_stream(nullptr)
    , m_virtualSinkIndex(PA_INVALID_INDEX)
    , m_moduleIndex(PA_INVALID_INDEX)
    , m_inputLoopbackModuleIndex(PA_INVALID_INDEX)
    , m_outputLoopbackModuleIndex(PA_INVALID_INDEX)
    , m_sampleRate(44100)
    , m_channels(2)
    , m_bytesPerSample(2) // 16-bit
    , m_isOpen(false)
    , m_isCapturing(false)
{
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
        LOG_ERROR("Falha ao criar PulseAudio mainloop");
        return false;
    }

    m_mainloopApi = pa_mainloop_get_api(m_mainloop);
    m_context = pa_context_new(m_mainloopApi, "RetroCapture");

    if (!m_context)
    {
        LOG_ERROR("Falha ao criar PulseAudio context");
        cleanupPulseAudio();
        return false;
    }

    pa_context_set_state_callback(m_context, contextStateCallback, this);

    pa_context_flags_t flags = PA_CONTEXT_NOFLAGS;
    if (pa_context_connect(m_context, nullptr, flags, nullptr) < 0)
    {
        LOG_ERROR("Falha ao conectar ao PulseAudio: " + std::string(pa_strerror(pa_context_errno(m_context))));
        cleanupPulseAudio();
        return false;
    }

    return true;
}

void AudioCapturePulse::cleanupPulseAudio()
{
    stopCapture();

    // IMPORTANT: Clean up all loopbacks BEFORE removing virtual sink
    // This ensures we don't leave orphaned modules
    disconnectInputSource();
    removeMonitoringOutput();

    removeVirtualSink();

    // Process events to ensure async operations complete
    if (m_mainloop && m_context)
    {
        int ret = 0;
        for (int i = 0; i < 100; i++)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            usleep(10000); // 10ms
        }
    }

    if (m_stream)
    {
        pa_stream_disconnect(m_stream);
        pa_stream_unref(m_stream);
        m_stream = nullptr;
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
    m_virtualSinkIndex = PA_INVALID_INDEX;
    m_moduleIndex = PA_INVALID_INDEX;
    m_inputLoopbackModuleIndex = PA_INVALID_INDEX;
    m_outputLoopbackModuleIndex = PA_INVALID_INDEX;
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
        LOG_ERROR("Falha ao ler dados do stream PulseAudio");
        return;
    }

    if (data && bytes > 0)
    {
        size_t samples = bytes / m_bytesPerSample;
        const int16_t *sampleData = static_cast<const int16_t *>(data);

        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            size_t oldSize = m_audioBuffer.size();
            m_audioBuffer.resize(oldSize + samples);
            std::memcpy(m_audioBuffer.data() + oldSize, sampleData, bytes);
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
        LOG_WARN("AudioCapture já está aberto");
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
            LOG_ERROR("Falha ao conectar ao PulseAudio");
            return false;
        }

        usleep(10000); // 10ms
        iteration++;
    }

    if (pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        LOG_ERROR("Timeout ao conectar ao PulseAudio");
        return false;
    }

    if (deviceName.empty())
    {
        if (!createVirtualSink())
        {
            LOG_ERROR("Falha ao criar sink virtual");
            return false;
        }
    }

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
        LOG_ERROR("Falha ao criar PulseAudio stream");
        removeVirtualSink();
        return false;
    }

    pa_stream_set_state_callback(m_stream, streamStateCallback, this);
    pa_stream_set_read_callback(m_stream, streamReadCallback, this);

    pa_stream_flags_t flags = static_cast<pa_stream_flags>(
        PA_STREAM_START_CORKED | PA_STREAM_ADJUST_LATENCY);

    std::string monitorDevice;
    if (m_virtualSinkIndex != PA_INVALID_INDEX)
    {
        monitorDevice = "RetroCapture.monitor";
    }
    else if (!deviceName.empty())
    {
        monitorDevice = deviceName;
    }

    const char *device = monitorDevice.empty() ? nullptr : monitorDevice.c_str();

    if (pa_stream_connect_record(m_stream, device, &bufferAttr, flags) < 0)
    {
        LOG_ERROR("Falha ao conectar stream de captura: " +
                  std::string(pa_strerror(pa_context_errno(m_context))));
        pa_stream_unref(m_stream);
        m_stream = nullptr;
        removeVirtualSink();
        return false;
    }

    iteration = 0;
    while (iteration < maxIterations)
    {
        pa_mainloop_iterate(m_mainloop, 0, &ret);

        pa_stream_state_t state = pa_stream_get_state(m_stream);
        if (state == PA_STREAM_READY)
        {
            break;
        }
        else if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
        {
            LOG_ERROR("Falha ao criar stream de captura");
            pa_stream_unref(m_stream);
            m_stream = nullptr;
            removeVirtualSink();
            return false;
        }

        usleep(10000); // 10ms
        iteration++;
    }

    if (pa_stream_get_state(m_stream) != PA_STREAM_READY)
    {
        LOG_ERROR("Timeout ao criar stream de captura");
        pa_stream_unref(m_stream);
        m_stream = nullptr;
        removeVirtualSink();
        return false;
    }

    m_isOpen = true;
    if (m_virtualSinkIndex != PA_INVALID_INDEX)
    {
        LOG_INFO("AudioCapture aberto com sink virtual 'RetroCapture'");
    }
    else
    {
        LOG_INFO("AudioCapture aberto: " + std::to_string(m_sampleRate) + "Hz, " +
                 std::to_string(m_channels) + " canais");
    }

    return true;
}

void AudioCapturePulse::close()
{
    if (!m_isOpen)
    {
        return;
    }

    stopCapture();

    // IMPORTANT: Disconnect all loopbacks BEFORE removing virtual sink
    // This ensures clean shutdown
    disconnectInputSource();
    removeMonitoringOutput();

    if (m_stream)
    {
        pa_stream_disconnect(m_stream);
        pa_stream_unref(m_stream);
        m_stream = nullptr;
    }

    removeVirtualSink();

    m_isOpen = false;
    LOG_INFO("AudioCapture fechado");
}

bool AudioCapturePulse::isOpen() const
{
    return m_isOpen;
}

bool AudioCapturePulse::startCapture()
{
    if (!m_isOpen)
    {
        LOG_ERROR("AudioCapture não está aberto");
        return false;
    }

    if (m_isCapturing)
    {
        return true;
    }

    if (!m_stream)
    {
        LOG_ERROR("Stream não está disponível");
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
    // IMPORTANTE: Processar mainloop mesmo se não estiver aberto
    if (m_mainloop)
    {
        int ret = 0;
        for (int i = 0; i < 5; i++)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
        }
    }

    // Converter int16_t para float
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    if (m_audioBuffer.empty())
    {
        samples.clear();
        return 0;
    }

    size_t numSamples = m_audioBuffer.size();
    samples.resize(numSamples);

    // Converter de int16_t (-32768 a 32767) para float (-1.0 a 1.0)
    for (size_t i = 0; i < numSamples; ++i)
    {
        samples[i] = static_cast<float>(m_audioBuffer[i]) / 32768.0f;
    }

    m_audioBuffer.clear();
    return numSamples;
}

size_t AudioCapturePulse::getSamples(int16_t *buffer, size_t maxSamples)
{
    // IMPORTANTE: Processar mainloop mesmo se não estiver aberto
    if (m_mainloop)
    {
        int ret = 0;
        for (int i = 0; i < 5; i++)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
        }
    }

    if (!m_isOpen || !buffer || maxSamples == 0)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    if (m_audioBuffer.empty())
    {
        return 0;
    }

    size_t samplesToCopy = std::min(maxSamples, m_audioBuffer.size());
    if (samplesToCopy > 0)
    {
        std::memcpy(buffer, m_audioBuffer.data(), samplesToCopy * sizeof(int16_t));
        m_audioBuffer.erase(m_audioBuffer.begin(),
                           m_audioBuffer.begin() + samplesToCopy);
    }

    return samplesToCopy;
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
    
    // Clean up any orphaned loopbacks before listing
    cleanupOrphanedLoopbacks();
    
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
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        
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
    
    LOG_INFO("Found " + std::to_string(devices.size()) + " audio input sources");
    return devices;
}

// Static variables for sink listing
static std::vector<AudioDeviceInfo> g_availableSinks;
static std::mutex g_sinksMutex;

static void sinkInfoListCallback(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
    (void)c;
    (void)userdata;
    
    if (eol < 0)
    {
        return;
    }
    
    if (eol > 0)
    {
        return;
    }
    
    if (i)
    {
        // Only include real sinks (not null sinks we created)
        // Skip RetroCapture sink itself
        if (strcmp(i->name, "RetroCapture") != 0)
        {
            AudioDeviceInfo info;
            info.id = i->name ? i->name : "";
            info.name = i->description ? i->description : info.id;
            info.description = i->description ? i->description : "";
            info.available = (i->state == PA_SINK_RUNNING || i->state == PA_SINK_IDLE);
            
            std::lock_guard<std::mutex> lock(g_sinksMutex);
            g_availableSinks.push_back(info);
        }
    }
}

std::vector<AudioDeviceInfo> AudioCapturePulse::listOutputSinks()
{
    std::vector<AudioDeviceInfo> sinks;
    
    if (!initializePulseAudio())
    {
        return sinks;
    }
    
    waitForContextReady();
    
    if (pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        LOG_ERROR("PulseAudio context not ready for listing sinks");
        return sinks;
    }
    
    // Clear previous list
    {
        std::lock_guard<std::mutex> lock(g_sinksMutex);
        g_availableSinks.clear();
    }
    
    // Request sink list
    pa_operation *op = pa_context_get_sink_info_list(m_context, sinkInfoListCallback, this);
    if (!op)
    {
        LOG_ERROR("Failed to request sink list");
        return sinks;
    }
    
    // Wait for operation to complete
    int ret = 0;
    int maxIterations = 100;
    int iteration = 0;
    
    while (iteration < maxIterations)
    {
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        
        if (iteration > 10)
        {
            pa_operation_state_t opState = pa_operation_get_state(op);
            if (opState == PA_OPERATION_DONE || opState == PA_OPERATION_CANCELLED)
            {
                break;
            }
        }
        
        usleep(10000);
        iteration++;
    }
    
    pa_operation_unref(op);
    
    // Copy results
    {
        std::lock_guard<std::mutex> lock(g_sinksMutex);
        sinks = g_availableSinks;
    }
    
    LOG_INFO("Found " + std::to_string(sinks.size()) + " audio output sinks");
    return sinks;
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
    // TODO: Implementar listagem de dispositivos PulseAudio
    return devices;
}

void AudioCapturePulse::setAudioCallback(std::function<void(const int16_t *data, size_t samples)> callback)
{
    m_audioCallback = callback;
}

// Static variables for async operations
static std::atomic<bool> g_sinkOperationSuccess{false};
static std::atomic<uint32_t> g_sinkIndex{PA_INVALID_INDEX};
static std::atomic<uint32_t> g_moduleIndex{PA_INVALID_INDEX};
static std::atomic<uint32_t> g_monitoringModuleIndex{PA_INVALID_INDEX};
static std::atomic<uint32_t> g_sinkInputIndex{PA_INVALID_INDEX};

void AudioCapturePulse::sinkInfoCallback(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
    (void)c;
    (void)userdata;
    if (eol < 0)
    {
        g_sinkOperationSuccess = false;
        return;
    }

    if (eol > 0)
    {
        return;
    }

    if (i && strcmp(i->name, "RetroCapture") == 0)
    {
        g_sinkIndex = i->index;
        g_sinkOperationSuccess = true;
    }
}

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
    g_sinkOperationSuccess = (success != 0);
}

bool AudioCapturePulse::createVirtualSink()
{
    if (m_virtualSinkIndex != PA_INVALID_INDEX)
    {
        return true; // Já criado
    }

    if (pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        LOG_ERROR("Context PulseAudio não está pronto");
        return false;
    }

    g_sinkOperationSuccess = false;
    g_sinkIndex = PA_INVALID_INDEX;

    LOG_INFO("Verificando se sink virtual 'RetroCapture' já existe...");
    pa_operation *op = pa_context_get_sink_info_by_name(m_context, "RetroCapture", sinkInfoCallback, this);
    if (op)
    {
        int ret = 0;
        int maxIterations = 50;
        int iteration = 0;

        while (iteration < maxIterations)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            if (g_sinkIndex != PA_INVALID_INDEX)
            {
                break;
            }
            usleep(10000);
            iteration++;
        }
        pa_operation_unref(op);

        if (g_sinkIndex != PA_INVALID_INDEX)
        {
            m_virtualSinkIndex = g_sinkIndex;
            m_moduleIndex = PA_INVALID_INDEX;
            LOG_INFO("Sink virtual 'RetroCapture' já existe (índice: " + std::to_string(m_virtualSinkIndex) + ")");
            return true;
        }
    }

    LOG_INFO("Sink virtual 'RetroCapture' não encontrado, criando novo...");

    g_sinkOperationSuccess = false;
    g_moduleIndex = PA_INVALID_INDEX;

    LOG_INFO("Carregando módulo module-null-sink...");
    const char *args = "sink_name=RetroCapture sink_properties='device.description=\"RetroCapture Audio Input\"'";
    op = pa_context_load_module(m_context, "module-null-sink", args, operationCallback, this);

    if (!op)
    {
        LOG_ERROR("Falha ao criar operação para carregar module-null-sink: " +
                  std::string(pa_strerror(pa_context_errno(m_context))));
        return false;
    }

    int ret = 0;
    int maxIterations = 100;
    int iteration = 0;

    while (iteration < maxIterations)
    {
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        if (g_sinkOperationSuccess && g_moduleIndex != PA_INVALID_INDEX)
        {
            m_moduleIndex = g_moduleIndex;
            LOG_INFO("Módulo module-null-sink carregado com sucesso (índice: " + std::to_string(g_moduleIndex) + ")");
            break;
        }
        usleep(10000);
        iteration++;
    }

    pa_operation_unref(op);

    if (!g_sinkOperationSuccess || g_moduleIndex == PA_INVALID_INDEX)
    {
        LOG_ERROR("Falha ao criar sink virtual");
        return false;
    }

    usleep(100000); // 100ms

    LOG_INFO("Buscando índice do sink virtual 'RetroCapture' criado...");
    g_sinkIndex = PA_INVALID_INDEX;
    g_sinkOperationSuccess = false;
    op = pa_context_get_sink_info_by_name(m_context, "RetroCapture", sinkInfoCallback, this);
    if (op)
    {
        iteration = 0;
        while (iteration < maxIterations)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            if (g_sinkIndex != PA_INVALID_INDEX)
            {
                break;
            }
            usleep(10000);
            iteration++;
        }
        pa_operation_unref(op);
    }

    if (g_sinkIndex == PA_INVALID_INDEX)
    {
        LOG_WARN("Falha ao obter índice do sink virtual criado");
        m_virtualSinkIndex = 0;
        return true;
    }

    m_virtualSinkIndex = g_sinkIndex;
    LOG_INFO("Sink virtual 'RetroCapture' criado com sucesso (índice: " + std::to_string(m_virtualSinkIndex) + ")");
    return true;
}

void AudioCapturePulse::removeVirtualSink()
{
    // Note: Loopbacks should be disconnected before calling this
    // (handled in close() and cleanupPulseAudio())
    
    if (m_moduleIndex == PA_INVALID_INDEX)
    {
        m_virtualSinkIndex = PA_INVALID_INDEX;
        return;
    }

    if (m_virtualSinkIndex == PA_INVALID_INDEX)
    {
        m_moduleIndex = PA_INVALID_INDEX;
        return;
    }

    if (!m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        m_virtualSinkIndex = PA_INVALID_INDEX;
        m_moduleIndex = PA_INVALID_INDEX;
        return;
    }

    LOG_INFO("Removendo sink virtual 'RetroCapture' (módulo: " + std::to_string(m_moduleIndex) + ")");
    g_sinkOperationSuccess = false;
    pa_operation *op = pa_context_unload_module(m_context, m_moduleIndex, unloadModuleCallback, this);
    if (op)
    {
        int ret = 0;
        int maxIterations = 50;
        int iteration = 0;

        while (iteration < maxIterations)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            if (g_sinkOperationSuccess)
            {
                break;
            }
            usleep(10000);
            iteration++;
        }
        pa_operation_unref(op);
    }

    m_virtualSinkIndex = PA_INVALID_INDEX;
    m_moduleIndex = PA_INVALID_INDEX;
    LOG_INFO("Sink virtual 'RetroCapture' removido");
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
    
    if (m_virtualSinkIndex == PA_INVALID_INDEX)
    {
        LOG_ERROR("Virtual sink not created");
        return false;
    }
    
    // Clean up orphaned loopbacks first
    cleanupOrphanedLoopbacks();
    
    // Disconnect previous source if any
    disconnectInputSource();
    
    // Use module-loopback to connect source to sink
    // Note: module-loopback doesn't support custom names directly, but we can use
    // sink_input_name parameter to give it a descriptive name
    std::string args = "source=" + sourceName + " sink=RetroCapture sink_input_name=\"RetroCapture Input\"";
    
    g_sinkOperationSuccess = false;
    
    LOG_INFO("Connecting input source '" + sourceName + "' to RetroCapture sink...");
    pa_operation *op = pa_context_load_module(m_context, "module-loopback", args.c_str(), operationCallback, this);
    
    if (!op)
    {
        LOG_ERROR("Failed to create operation to load module-loopback");
        return false;
    }
    
    int ret = 0;
    int maxIterations = 100;
    int iteration = 0;
    
    while (iteration < maxIterations)
    {
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        if (g_sinkOperationSuccess && g_moduleIndex != PA_INVALID_INDEX)
        {
            m_inputLoopbackModuleIndex = g_moduleIndex; // Store module index
            m_currentInputSourceName = sourceName;
            LOG_INFO("Input source '" + sourceName + "' connected to RetroCapture sink");
            break;
        }
        usleep(10000);
        iteration++;
    }
    
    pa_operation_unref(op);
    
    if (!g_sinkOperationSuccess || g_moduleIndex == PA_INVALID_INDEX)
    {
        LOG_ERROR("Failed to connect source to sink");
        return false;
    }
    
    return true;
}

void AudioCapturePulse::disconnectInputSource()
{
    if (m_inputLoopbackModuleIndex == PA_INVALID_INDEX)
    {
        return;
    }
    
    if (!m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        m_inputLoopbackModuleIndex = PA_INVALID_INDEX;
        m_currentInputSourceName.clear();
        return;
    }
    
    LOG_INFO("Disconnecting input source from RetroCapture sink...");
    g_sinkOperationSuccess = false;
    pa_operation *op = pa_context_unload_module(m_context, m_inputLoopbackModuleIndex, unloadModuleCallback, this);
    
    if (op)
    {
        int ret = 0;
        int maxIterations = 50;
        int iteration = 0;
        
        while (iteration < maxIterations)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            if (g_sinkOperationSuccess)
            {
                break;
            }
            usleep(10000);
            iteration++;
        }
        pa_operation_unref(op);
    }
    
    m_inputLoopbackModuleIndex = PA_INVALID_INDEX;
    m_currentInputSourceName.clear();
    LOG_INFO("Input source disconnected from RetroCapture sink");
}

bool AudioCapturePulse::setMonitoringOutput(const std::string &outputSinkName)
{
    if (outputSinkName.empty())
    {
        // Remove monitoring if empty string
        removeMonitoringOutput();
        return true;
    }
    
    if (!m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        LOG_ERROR("PulseAudio context not ready");
        return false;
    }
    
    if (m_virtualSinkIndex == PA_INVALID_INDEX)
    {
        LOG_ERROR("RetroCapture sink not created");
        return false;
    }
    
    // Clean up orphaned loopbacks first
    cleanupOrphanedLoopbacks();
    
    // Remove previous monitoring if any
    removeMonitoringOutput();
    
    // Create loopback from RetroCapture.monitor to selected output sink
    // This allows hearing what's being captured
    // Use sink_input_name to give it a descriptive name
    std::string args = "source=RetroCapture.monitor sink=" + outputSinkName + " sink_input_name=\"RetroCapture Monitor\"";
    
    g_sinkOperationSuccess = false;
    
    LOG_INFO("Setting monitoring output to '" + outputSinkName + "'...");
    pa_operation *op = pa_context_load_module(m_context, "module-loopback", args.c_str(), operationCallback, this);
    
    if (!op)
    {
        LOG_ERROR("Failed to create operation to load monitoring module-loopback");
        return false;
    }
    
    int ret = 0;
    int maxIterations = 100;
    int iteration = 0;
    
    while (iteration < maxIterations)
    {
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        if (g_sinkOperationSuccess && g_moduleIndex != PA_INVALID_INDEX)
        {
            m_outputLoopbackModuleIndex = g_moduleIndex;
            m_currentMonitoringOutputName = outputSinkName;
            LOG_INFO("Monitoring output set to '" + outputSinkName + "' successfully");
            break;
        }
        usleep(10000);
        iteration++;
    }
    
    pa_operation_unref(op);
    
    if (!g_sinkOperationSuccess || g_moduleIndex == PA_INVALID_INDEX)
    {
        LOG_ERROR("Failed to set monitoring output");
        return false;
    }
    
    return true;
}

void AudioCapturePulse::removeMonitoringOutput()
{
    if (m_outputLoopbackModuleIndex == PA_INVALID_INDEX)
    {
        return;
    }
    
    if (!m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        m_outputLoopbackModuleIndex = PA_INVALID_INDEX;
        m_currentMonitoringOutputName.clear();
        return;
    }
    
    LOG_INFO("Removing monitoring output...");
    g_sinkOperationSuccess = false;
    pa_operation *op = pa_context_unload_module(m_context, m_outputLoopbackModuleIndex, unloadModuleCallback, this);
    
    if (op)
    {
        int ret = 0;
        int maxIterations = 50;
        int iteration = 0;
        
        while (iteration < maxIterations)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            if (g_sinkOperationSuccess)
            {
                break;
            }
            usleep(10000);
            iteration++;
        }
        pa_operation_unref(op);
    }
    
    m_outputLoopbackModuleIndex = PA_INVALID_INDEX;
    m_currentMonitoringOutputName.clear();
    LOG_INFO("Monitoring output removed");
}

// Static callback for module info
static void moduleInfoCallback(pa_context *c, const pa_module_info *i, int eol, void *userdata)
{
    (void)c;
    std::vector<uint32_t> *moduleIndices = static_cast<std::vector<uint32_t> *>(userdata);
    
    if (eol < 0)
    {
        return;
    }
    
    if (eol > 0)
    {
        return;
    }
    
    if (i && i->name)
    {
        std::string moduleName = i->name ? i->name : "";
        // Check if this is a loopback module related to RetroCapture
        if (moduleName.find("loopback") != std::string::npos)
        {
            bool isRetroCaptureLoopback = false;
            
            if (i->argument)
            {
                std::string argStr = i->argument;
                // Check for our named loopbacks (using sink_input_name)
                if (argStr.find("sink_input_name=\"RetroCapture Input\"") != std::string::npos ||
                    argStr.find("sink_input_name=\"RetroCapture Monitor\"") != std::string::npos)
                {
                    isRetroCaptureLoopback = true;
                }
                // Also check for connections to RetroCapture (for backward compatibility)
                else if (argStr.find("RetroCapture") != std::string::npos || 
                         argStr.find("sink=RetroCapture") != std::string::npos ||
                         argStr.find("source=RetroCapture.monitor") != std::string::npos)
                {
                    isRetroCaptureLoopback = true;
                }
            }
            
            if (isRetroCaptureLoopback)
            {
                moduleIndices->push_back(i->index);
            }
        }
    }
}

void AudioCapturePulse::cleanupOrphanedLoopbacks()
{
    if (!m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        return;
    }
    
    LOG_INFO("Cleaning up orphaned RetroCapture loopbacks...");
    
    // Get list of all modules
    std::vector<uint32_t> orphanedModules;
    pa_operation *op = pa_context_get_module_info_list(m_context, moduleInfoCallback, &orphanedModules);
    
    if (op)
    {
        int ret = 0;
        int maxIterations = 100;
        int iteration = 0;
        
        while (iteration < maxIterations)
        {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            
            pa_operation_state_t opState = pa_operation_get_state(op);
            if (opState == PA_OPERATION_DONE || opState == PA_OPERATION_CANCELLED)
            {
                break;
            }
            
            usleep(10000);
            iteration++;
        }
        
        pa_operation_unref(op);
    }
    
    // Remove orphaned modules (excluding the ones we're currently using)
    for (uint32_t moduleIndex : orphanedModules)
    {
        if (moduleIndex != m_inputLoopbackModuleIndex && 
            moduleIndex != m_outputLoopbackModuleIndex)
        {
            LOG_INFO("Removing orphaned loopback module: " + std::to_string(moduleIndex));
            g_sinkOperationSuccess = false;
            pa_operation *unloadOp = pa_context_unload_module(m_context, moduleIndex, unloadModuleCallback, this);
            
            if (unloadOp)
            {
                int ret = 0;
                for (int i = 0; i < 50; i++)
                {
                    pa_mainloop_iterate(m_mainloop, 0, &ret);
                    if (g_sinkOperationSuccess)
                    {
                        break;
                    }
                    usleep(10000);
                }
                pa_operation_unref(unloadOp);
            }
        }
    }
    
    if (!orphanedModules.empty())
    {
        LOG_INFO("Cleaned up " + std::to_string(orphanedModules.size()) + " orphaned loopback(s)");
    }
}

