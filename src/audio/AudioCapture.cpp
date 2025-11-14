#include "AudioCapture.h"
#include "../utils/Logger.h"
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <cstring>
#include <algorithm>
#include <atomic>

AudioCapture::AudioCapture()
    : m_mainloop(nullptr)
    , m_mainloopApi(nullptr)
    , m_context(nullptr)
    , m_stream(nullptr)
    , m_virtualSinkIndex(PA_INVALID_INDEX)
    , m_sampleRate(44100)
    , m_channels(2)
    , m_bytesPerSample(2) // 16-bit
    , m_isOpen(false)
    , m_isCapturing(false)
{
}

AudioCapture::~AudioCapture() {
    close();
    cleanupPulseAudio();
}

bool AudioCapture::initializePulseAudio() {
    if (m_mainloop) {
        return true; // Already initialized
    }
    
    m_mainloop = pa_mainloop_new();
    if (!m_mainloop) {
        LOG_ERROR("Falha ao criar PulseAudio mainloop");
        return false;
    }
    
    m_mainloopApi = pa_mainloop_get_api(m_mainloop);
    m_context = pa_context_new(m_mainloopApi, "RetroCapture");
    
    if (!m_context) {
        LOG_ERROR("Falha ao criar PulseAudio context");
        cleanupPulseAudio();
        return false;
    }
    
    pa_context_set_state_callback(m_context, contextStateCallback, this);
    
    pa_context_flags_t flags = PA_CONTEXT_NOFLAGS;
    if (pa_context_connect(m_context, nullptr, flags, nullptr) < 0) {
        LOG_ERROR("Falha ao conectar ao PulseAudio: " + std::string(pa_strerror(pa_context_errno(m_context))));
        cleanupPulseAudio();
        return false;
    }
    
    return true;
}

void AudioCapture::cleanupPulseAudio() {
    removeVirtualSink();
    
    if (m_stream) {
        pa_stream_unref(m_stream);
        m_stream = nullptr;
    }
    
    if (m_context) {
        pa_context_unref(m_context);
        m_context = nullptr;
    }
    
    if (m_mainloop) {
        pa_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
    }
    
    m_mainloopApi = nullptr;
    m_virtualSinkIndex = PA_INVALID_INDEX;
}

void AudioCapture::contextStateCallback(pa_context* c, void* userdata) {
    (void)c; // Unused
    AudioCapture* self = static_cast<AudioCapture*>(userdata);
    self->contextStateChanged();
}

void AudioCapture::contextStateChanged() {
    if (!m_context) {
        return;
    }
    
    pa_context_state_t state = pa_context_get_state(m_context);
    
    switch (state) {
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

void AudioCapture::streamStateCallback(pa_stream* s, void* userdata) {
    (void)s; // Unused
    AudioCapture* self = static_cast<AudioCapture*>(userdata);
    self->streamStateChanged();
}

void AudioCapture::streamStateChanged() {
    if (!m_stream) {
        return;
    }
    
    pa_stream_state_t state = pa_stream_get_state(m_stream);
    
    switch (state) {
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

void AudioCapture::streamReadCallback(pa_stream* s, size_t length, void* userdata) {
    (void)s; // Unused
    AudioCapture* self = static_cast<AudioCapture*>(userdata);
    self->streamRead(length);
}

void AudioCapture::streamRead(size_t length) {
    (void)length; // Unused - we read all available data
    if (!m_stream) {
        return;
    }
    
    const void* data;
    size_t bytes;
    
    if (pa_stream_peek(m_stream, &data, &bytes) < 0) {
        LOG_ERROR("Falha ao ler dados do stream PulseAudio");
        return;
    }
    
    if (data && bytes > 0) {
        // Converter bytes para samples (16-bit = 2 bytes por sample)
        size_t samples = bytes / m_bytesPerSample;
        const int16_t* sampleData = static_cast<const int16_t*>(data);
        
        // Adicionar ao buffer (thread-safe)
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            size_t oldSize = m_audioBuffer.size();
            m_audioBuffer.resize(oldSize + samples);
            std::memcpy(m_audioBuffer.data() + oldSize, sampleData, bytes);
        }
        
        // Chamar callback se configurado
        if (m_audioCallback) {
            m_audioCallback(sampleData, samples);
        }
    }
    
    // Remover dados do stream
    pa_stream_drop(m_stream);
}

void AudioCapture::streamSuccessCallback(pa_stream* s, int success, void* userdata) {
    // Callback para operações assíncronas
    (void)s;
    (void)success;
    (void)userdata;
}

bool AudioCapture::open(const std::string& deviceName) {
    if (m_isOpen) {
        LOG_WARN("AudioCapture já está aberto");
        return true;
    }
    
    if (!initializePulseAudio()) {
        return false;
    }
    
    m_deviceName = deviceName;
    
    // Processar eventos até o context estar pronto
    int ret = 0;
    int maxIterations = 100;
    int iteration = 0;
    
    while (iteration < maxIterations) {
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        
        pa_context_state_t state = pa_context_get_state(m_context);
        if (state == PA_CONTEXT_READY) {
            break;
        } else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            LOG_ERROR("Falha ao conectar ao PulseAudio");
            return false;
        }
        
        usleep(10000); // 10ms
        iteration++;
    }
    
    if (pa_context_get_state(m_context) != PA_CONTEXT_READY) {
        LOG_ERROR("Timeout ao conectar ao PulseAudio");
        return false;
    }
    
    // Criar sink virtual se não foi especificado um device
    if (deviceName.empty()) {
        if (!createVirtualSink()) {
            LOG_ERROR("Falha ao criar sink virtual");
            return false;
        }
    }
    
    // Criar stream de captura
    pa_sample_spec sampleSpec;
    sampleSpec.format = PA_SAMPLE_S16LE; // 16-bit little-endian
    sampleSpec.rate = m_sampleRate;
    sampleSpec.channels = m_channels;
    
    pa_buffer_attr bufferAttr;
    bufferAttr.maxlength = static_cast<uint32_t>(-1);
    bufferAttr.tlength = static_cast<uint32_t>(-1);
    bufferAttr.prebuf = static_cast<uint32_t>(-1);
    bufferAttr.minreq = static_cast<uint32_t>(-1);
    bufferAttr.fragsize = static_cast<uint32_t>(m_sampleRate * m_bytesPerSample * m_channels / 10); // 100ms
    
    const char* streamName = "RetroCapture Audio Capture";
    m_stream = pa_stream_new(m_context, streamName, &sampleSpec, nullptr);
    
    if (!m_stream) {
        LOG_ERROR("Falha ao criar PulseAudio stream");
        removeVirtualSink();
        return false;
    }
    
    pa_stream_set_state_callback(m_stream, streamStateCallback, this);
    pa_stream_set_read_callback(m_stream, streamReadCallback, this);
    
    // Conectar stream - usar monitor do sink virtual se criado, senão usar device especificado
    pa_stream_flags_t flags = static_cast<pa_stream_flags>(
        PA_STREAM_START_CORKED | PA_STREAM_ADJUST_LATENCY);
    
    std::string monitorDevice;
    if (m_virtualSinkIndex != PA_INVALID_INDEX) {
        // Usar monitor do sink virtual
        monitorDevice = "RetroCapture.monitor";
    } else if (!deviceName.empty()) {
        monitorDevice = deviceName;
    }
    
    const char* device = monitorDevice.empty() ? nullptr : monitorDevice.c_str();
    
    if (pa_stream_connect_record(m_stream, device, &bufferAttr, flags) < 0) {
        LOG_ERROR("Falha ao conectar stream de captura: " + 
                  std::string(pa_strerror(pa_context_errno(m_context))));
        pa_stream_unref(m_stream);
        m_stream = nullptr;
        removeVirtualSink();
        return false;
    }
    
    // Processar eventos até o stream estar pronto
    iteration = 0;
    while (iteration < maxIterations) {
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        
        pa_stream_state_t state = pa_stream_get_state(m_stream);
        if (state == PA_STREAM_READY) {
            break;
        } else if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
            LOG_ERROR("Falha ao criar stream de captura");
            pa_stream_unref(m_stream);
            m_stream = nullptr;
            removeVirtualSink();
            return false;
        }
        
        usleep(10000); // 10ms
        iteration++;
    }
    
    if (pa_stream_get_state(m_stream) != PA_STREAM_READY) {
        LOG_ERROR("Timeout ao criar stream de captura");
        pa_stream_unref(m_stream);
        m_stream = nullptr;
        removeVirtualSink();
        return false;
    }
    
    m_isOpen = true;
    if (m_virtualSinkIndex != PA_INVALID_INDEX) {
        LOG_INFO("AudioCapture aberto com sink virtual 'RetroCapture' (visível no qpwgraph)");
    } else {
        LOG_INFO("AudioCapture aberto: " + std::to_string(m_sampleRate) + "Hz, " + 
                 std::to_string(m_channels) + " canais");
    }
    
    return true;
}

void AudioCapture::close() {
    if (!m_isOpen) {
        return;
    }
    
    stopCapture();
    
    if (m_stream) {
        pa_stream_disconnect(m_stream);
        pa_stream_unref(m_stream);
        m_stream = nullptr;
    }
    
    removeVirtualSink();
    
    m_isOpen = false;
    LOG_INFO("AudioCapture fechado");
}

bool AudioCapture::startCapture() {
    if (!m_isOpen) {
        LOG_ERROR("AudioCapture não está aberto");
        return false;
    }
    
    if (m_isCapturing) {
        LOG_WARN("AudioCapture já está capturando");
        return true;
    }
    
    if (!m_stream) {
        LOG_ERROR("Stream não está disponível");
        return false;
    }
    
    // Descorkar o stream para começar a captura
    pa_operation* op = pa_stream_cork(m_stream, 0, streamSuccessCallback, this);
    if (op) {
        pa_operation_unref(op);
    }
    
    m_isCapturing = true;
    LOG_INFO("AudioCapture iniciado");
    
    return true;
}

void AudioCapture::stopCapture() {
    if (!m_isCapturing) {
        return;
    }
    
    if (m_stream) {
        // Corkar o stream para parar a captura
        pa_operation* op = pa_stream_cork(m_stream, 1, streamSuccessCallback, this);
        if (op) {
            pa_operation_unref(op);
        }
    }
    
    m_isCapturing = false;
    LOG_INFO("AudioCapture parado");
}

size_t AudioCapture::getSamples(int16_t* buffer, size_t maxSamples) {
    if (!m_isOpen || !buffer || maxSamples == 0) {
        return 0;
    }
    
    // Processar eventos do PulseAudio
    if (m_mainloop) {
        int ret = 0;
        pa_mainloop_iterate(m_mainloop, 0, &ret);
    }
    
    // Copiar samples do buffer (thread-safe)
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    
    size_t samplesToCopy = std::min(maxSamples, m_audioBuffer.size());
    if (samplesToCopy > 0) {
        std::memcpy(buffer, m_audioBuffer.data(), samplesToCopy * sizeof(int16_t));
        
        // Remover samples copiados
        m_audioBuffer.erase(m_audioBuffer.begin(), 
                           m_audioBuffer.begin() + samplesToCopy);
    }
    
    return samplesToCopy;
}

std::vector<std::string> AudioCapture::getAvailableDevices() {
    std::vector<std::string> devices;
    
    // TODO: Implementar enumeração de dispositivos PulseAudio
    // Por enquanto, retornar lista vazia (usar dispositivo padrão)
    
    return devices;
}

void AudioCapture::setAudioCallback(std::function<void(const int16_t* data, size_t samples)> callback) {
    m_audioCallback = callback;
}

// Static variables for async operations
static std::atomic<bool> g_sinkOperationSuccess{false};
static std::atomic<uint32_t> g_sinkIndex{PA_INVALID_INDEX};
static std::atomic<uint32_t> g_moduleIndex{PA_INVALID_INDEX};

void AudioCapture::sinkInfoCallback(pa_context* c, const pa_sink_info* i, int eol, void* userdata) {
    (void)c;
    (void)userdata;
    if (eol < 0) {
        g_sinkOperationSuccess = false;
        return;
    }
    
    if (eol > 0) {
        return;
    }
    
    if (i && strcmp(i->name, "RetroCapture") == 0) {
        g_sinkIndex = i->index;
        g_sinkOperationSuccess = true;
    }
}

void AudioCapture::operationCallback(pa_context* c, uint32_t index, void* userdata) {
    (void)c;
    (void)userdata;
    g_moduleIndex = index;
    g_sinkOperationSuccess = (index != PA_INVALID_INDEX);
}

bool AudioCapture::createVirtualSink() {
    if (m_virtualSinkIndex != PA_INVALID_INDEX) {
        return true; // Já criado
    }
    
    if (pa_context_get_state(m_context) != PA_CONTEXT_READY) {
        LOG_ERROR("Context PulseAudio não está pronto");
        return false;
    }
    
    // Primeiro, verificar se o sink já existe
    g_sinkOperationSuccess = false;
    g_sinkIndex = PA_INVALID_INDEX;
    
    pa_operation* op = pa_context_get_sink_info_by_name(m_context, "RetroCapture", sinkInfoCallback, this);
    if (op) {
        int ret = 0;
        int maxIterations = 50;
        int iteration = 0;
        
        while (iteration < maxIterations) {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            if (g_sinkIndex != PA_INVALID_INDEX || !g_sinkOperationSuccess) {
                break;
            }
            usleep(10000);
            iteration++;
        }
        pa_operation_unref(op);
        
        if (g_sinkIndex != PA_INVALID_INDEX) {
            // Sink já existe
            m_virtualSinkIndex = g_sinkIndex;
            LOG_INFO("Sink virtual 'RetroCapture' já existe (índice: " + std::to_string(m_virtualSinkIndex) + ")");
            return true;
        }
    }
    
    // Criar sink virtual usando module-null-sink
    // Isso cria um sink que aparece no qpwgraph
    g_sinkOperationSuccess = false;
    g_moduleIndex = PA_INVALID_INDEX;
    
    const char* args = "sink_name=RetroCapture sink_properties='device.description=\"RetroCapture Audio Input\"'";
    op = pa_context_load_module(m_context, "module-null-sink", args, operationCallback, this);
    
    if (!op) {
        LOG_ERROR("Falha ao criar operação para carregar module-null-sink");
        return false;
    }
    
    // Processar eventos até a operação completar
    int ret = 0;
    int maxIterations = 50;
    int iteration = 0;
    
    while (iteration < maxIterations) {
        pa_mainloop_iterate(m_mainloop, 0, &ret);
        if (g_sinkOperationSuccess && g_moduleIndex != PA_INVALID_INDEX) {
            break;
        }
        usleep(10000);
        iteration++;
    }
    
    pa_operation_unref(op);
    
    if (!g_sinkOperationSuccess || g_moduleIndex == PA_INVALID_INDEX) {
        LOG_ERROR("Falha ao criar sink virtual (timeout ou erro)");
        return false;
    }
    
    // Buscar o índice do sink criado
    g_sinkIndex = PA_INVALID_INDEX;
    op = pa_context_get_sink_info_by_name(m_context, "RetroCapture", sinkInfoCallback, this);
    if (op) {
        iteration = 0;
        while (iteration < maxIterations) {
            pa_mainloop_iterate(m_mainloop, 0, &ret);
            if (g_sinkIndex != PA_INVALID_INDEX) {
                break;
            }
            usleep(10000);
            iteration++;
        }
        pa_operation_unref(op);
    }
    
    if (g_sinkIndex == PA_INVALID_INDEX) {
        LOG_ERROR("Falha ao obter índice do sink virtual criado");
        return false;
    }
    
    m_virtualSinkIndex = g_sinkIndex;
    LOG_INFO("Sink virtual 'RetroCapture' criado com sucesso (índice: " + std::to_string(m_virtualSinkIndex) + ")");
    LOG_INFO("O sink aparecerá no qpwgraph - conecte outras aplicações a ele para capturar áudio");
    
    return true;
}

void AudioCapture::removeVirtualSink() {
    if (m_virtualSinkIndex == PA_INVALID_INDEX) {
        return;
    }
    
    if (pa_context_get_state(m_context) != PA_CONTEXT_READY) {
        m_virtualSinkIndex = PA_INVALID_INDEX;
        return;
    }
    
    // Descobrir o módulo ID do sink virtual
    // Isso é um pouco complicado - vamos usar uma abordagem mais simples:
    // descarregar todos os módulos null-sink e recriar se necessário
    // Por enquanto, vamos apenas marcar como removido e deixar o PulseAudio gerenciar
    
    // Na prática, é melhor deixar o sink existir mesmo após fechar,
    // para que o usuário não precise recriar toda vez
    // Mas podemos adicionar uma opção para removê-lo se necessário
    
    LOG_INFO("Sink virtual 'RetroCapture' mantido (pode ser usado novamente)");
    m_virtualSinkIndex = PA_INVALID_INDEX;
}

