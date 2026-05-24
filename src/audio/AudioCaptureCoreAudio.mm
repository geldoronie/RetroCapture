#include "AudioCaptureCoreAudio.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cstring>

#ifdef __APPLE__
#import <CoreAudio/CoreAudio.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreFoundation/CoreFoundation.h>

// Estrutura para passar dados para o callback (definida antes do uso)
struct AudioCaptureContext
{
    AudioCaptureCoreAudio* capture;
    std::mutex* bufferMutex;
    std::vector<int16_t>* audioBuffer;
};

static OSStatus audioInputCallback(void *inRefCon,
                                   AudioUnitRenderActionFlags *ioActionFlags,
                                   const AudioTimeStamp *inTimeStamp,
                                   UInt32 inBusNumber,
                                   UInt32 inNumberFrames,
                                   AudioBufferList *ioData)
{
    (void)ioActionFlags;
    (void)inTimeStamp;
    (void)inBusNumber;
    (void)ioData;
    
    AudioCaptureContext* context = (AudioCaptureContext*)inRefCon;
    if (!context || !context->capture)
    {
        return noErr;
    }
    
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp timeStamp = *inTimeStamp;
    AudioBufferList bufferList;
    bufferList.mNumberBuffers = 1;
    bufferList.mBuffers[0].mNumberChannels = 2; // Stereo
    bufferList.mBuffers[0].mDataByteSize = inNumberFrames * 2 * sizeof(int16_t);
    bufferList.mBuffers[0].mData = nullptr;
    
    // Alocar buffer temporário
    std::vector<int16_t> tempBuffer(inNumberFrames * 2);
    bufferList.mBuffers[0].mData = tempBuffer.data();
    
    // Renderizar áudio
    AudioComponentInstance audioUnit = context->capture->getAudioUnit();
    OSStatus status = AudioUnitRender(audioUnit, &flags, &timeStamp, 1, inNumberFrames, &bufferList);
    
    if (status == noErr)
    {
        std::lock_guard<std::mutex> lock(*context->bufferMutex);
        context->audioBuffer->insert(context->audioBuffer->end(), 
                                     tempBuffer.begin(), 
                                     tempBuffer.end());
    }
    
    return status;
}
#endif

AudioCaptureCoreAudio::AudioCaptureCoreAudio()
#ifdef __APPLE__
    : m_audioUnit(nullptr)
    , m_audioComponent(nullptr)
    , m_context(nullptr)
    , m_bufferMutex()
    , m_sampleRate(44100)
    , m_channels(2)
    , m_bytesPerSample(2) // 16-bit
    , m_isOpen(false)
    , m_isCapturing(false)
#else
    : m_sampleRate(44100)
    , m_channels(2)
    , m_bytesPerSample(2) // 16-bit
    , m_isOpen(false)
    , m_isCapturing(false)
#endif
{
}

AudioCaptureCoreAudio::~AudioCaptureCoreAudio()
{
    close();
    cleanupAudioUnit();
}

#ifdef __APPLE__
AudioComponentInstance AudioCaptureCoreAudio::getAudioUnit() const
{
    return m_audioUnit;
}
#endif

bool AudioCaptureCoreAudio::open(const std::string &deviceName)
{
    if (m_isOpen)
    {
        LOG_WARN("Dispositivo de áudio já aberto, fechando primeiro");
        close();
    }

#ifdef __APPLE__
    if (!initializeAudioUnit())
    {
        LOG_ERROR("Falha ao inicializar AudioUnit");
        return false;
    }

    m_isOpen = true;
    LOG_INFO("Dispositivo de áudio aberto: " + (deviceName.empty() ? "default" : deviceName));
    return true;
#else
    LOG_ERROR("Core Audio só está disponível no macOS");
    return false;
#endif
}

void AudioCaptureCoreAudio::close()
{
    if (m_isCapturing)
    {
        stopCapture();
    }

    m_isOpen = false;
    LOG_INFO("Dispositivo de áudio fechado");
}

bool AudioCaptureCoreAudio::isOpen() const
{
    return m_isOpen;
}

size_t AudioCaptureCoreAudio::getSamples(std::vector<float> &samples)
{
    if (!m_isOpen || !m_isCapturing)
    {
        samples.clear();
        return 0;
    }

#ifdef __APPLE__
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    
    if (m_audioBuffer.empty())
    {
        samples.clear();
        return 0;
    }

    // Converter int16 para float
    samples.resize(m_audioBuffer.size());
    for (size_t i = 0; i < m_audioBuffer.size(); ++i)
    {
        samples[i] = m_audioBuffer[i] / 32768.0f;
    }

    m_audioBuffer.clear();
    return samples.size();
#else
    samples.clear();
    return 0;
#endif
}

uint32_t AudioCaptureCoreAudio::getSampleRate() const
{
    return m_sampleRate;
}

uint32_t AudioCaptureCoreAudio::getChannels() const
{
    return m_channels;
}

std::vector<AudioDeviceInfo> AudioCaptureCoreAudio::listDevices()
{
    std::vector<AudioDeviceInfo> devices;

#ifdef __APPLE__
    // Listar dispositivos de entrada via Core Audio
    UInt32 propertySize = 0;
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                     &propertyAddress,
                                                     0,
                                                     nullptr,
                                                     &propertySize);
    
    if (status == noErr && propertySize > 0)
    {
        UInt32 deviceCount = propertySize / sizeof(AudioDeviceID);
        AudioDeviceID* deviceIDs = new AudioDeviceID[deviceCount];
        
        status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                            &propertyAddress,
                                            0,
                                            nullptr,
                                            &propertySize,
                                            deviceIDs);
        
        if (status == noErr)
        {
            for (UInt32 i = 0; i < deviceCount; ++i)
            {
                // Verificar se é dispositivo de entrada
                UInt32 inputStreamCount = 0;
                propertySize = sizeof(inputStreamCount);
                AudioObjectPropertyAddress streamProperty = {
                    kAudioDevicePropertyStreams,
                    kAudioDevicePropertyScopeInput,
                    kAudioObjectPropertyElementMain
                };
                AudioObjectGetPropertyData(deviceIDs[i],
                                          &streamProperty,
                                          0,
                                          nullptr,
                                          &propertySize,
                                          &inputStreamCount);
                
                if (inputStreamCount > 0)
                {
                    // Obter nome do dispositivo
                    CFStringRef deviceName = nullptr;
                    propertySize = sizeof(deviceName);
                    AudioObjectPropertyAddress nameProperty = {
                        kAudioDevicePropertyDeviceNameCFString,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    AudioObjectGetPropertyData(deviceIDs[i],
                                              &nameProperty,
                                              0,
                                              nullptr,
                                              &propertySize,
                                              &deviceName);
                    
                    AudioDeviceInfo info;
                    info.id = std::to_string(deviceIDs[i]);
                    if (deviceName)
                    {
                        char buffer[256];
                        CFStringGetCString(deviceName, buffer, sizeof(buffer), kCFStringEncodingUTF8);
                        info.name = std::string(buffer);
                        CFRelease(deviceName);
                    }
                    else
                    {
                        info.name = "Audio Device " + info.id;
                    }
                    info.available = true;
                    devices.push_back(info);
                }
            }
        }
        
        delete[] deviceIDs;
    }
    
    // Adicionar dispositivo padrão se não houver dispositivos
    if (devices.empty())
    {
        AudioDeviceInfo defaultDevice;
        defaultDevice.id = "default";
        defaultDevice.name = "Default Audio Input";
        defaultDevice.available = true;
        devices.push_back(defaultDevice);
    }
#endif

    return devices;
}

void AudioCaptureCoreAudio::setDeviceStateCallback(std::function<void(const std::string &, bool)> callback)
{
    m_deviceStateCallback = callback;
}

bool AudioCaptureCoreAudio::startCapture()
{
    if (!m_isOpen)
    {
        LOG_ERROR("Dispositivo de áudio não aberto");
        return false;
    }

#ifdef __APPLE__
    OSStatus status = AudioOutputUnitStart(m_audioUnit);
    if (status == noErr)
    {
        m_isCapturing = true;
        LOG_INFO("Captura de áudio iniciada");
        return true;
    }
    else
    {
        LOG_ERROR("Falha ao iniciar captura de áudio: " + std::to_string(status));
        return false;
    }
#else
    return false;
#endif
}

void AudioCaptureCoreAudio::stopCapture()
{
    m_isCapturing = false;
#ifdef __APPLE__
    if (m_audioUnit)
    {
        AudioOutputUnitStop(m_audioUnit);
        LOG_INFO("Captura de áudio parada");
    }
#endif
}

size_t AudioCaptureCoreAudio::getSamples(int16_t *buffer, size_t maxSamples)
{
    if (!m_isOpen || !m_isCapturing || !buffer)
    {
        return 0;
    }

#ifdef __APPLE__
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    
    size_t samplesToCopy = std::min(maxSamples, m_audioBuffer.size());
    if (samplesToCopy > 0)
    {
        std::memcpy(buffer, m_audioBuffer.data(), samplesToCopy * sizeof(int16_t));
        m_audioBuffer.erase(m_audioBuffer.begin(), m_audioBuffer.begin() + samplesToCopy);
    }
    
    return samplesToCopy;
#else
    return 0;
#endif
}

bool AudioCaptureCoreAudio::initializeAudioUnit()
{
#ifdef __APPLE__
    // Descrição do componente de áudio
    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    
    // Encontrar componente
    m_audioComponent = AudioComponentFindNext(nullptr, &desc);
    if (!m_audioComponent)
    {
        LOG_ERROR("Falha ao encontrar AudioComponent");
        return false;
    }
    
    // Criar instância
    OSStatus status = AudioComponentInstanceNew(m_audioComponent, &m_audioUnit);
    if (status != noErr)
    {
        LOG_ERROR("Falha ao criar AudioUnit: " + std::to_string(status));
        return false;
    }
    
    // Habilitar input
    UInt32 enableInput = 1;
    status = AudioUnitSetProperty(m_audioUnit,
                                  kAudioOutputUnitProperty_EnableIO,
                                  kAudioUnitScope_Input,
                                  1, // Input element
                                  &enableInput,
                                  sizeof(enableInput));
    if (status != noErr)
    {
        LOG_ERROR("Falha ao habilitar input: " + std::to_string(status));
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
    }
    
    // Desabilitar output
    UInt32 enableOutput = 0;
    status = AudioUnitSetProperty(m_audioUnit,
                                  kAudioOutputUnitProperty_EnableIO,
                                  kAudioUnitScope_Output,
                                  0, // Output element
                                  &enableOutput,
                                  sizeof(enableOutput));
    if (status != noErr)
    {
        LOG_ERROR("Falha ao desabilitar output: " + std::to_string(status));
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
    }
    
    // Configurar formato de áudio
    AudioStreamBasicDescription streamFormat;
    streamFormat.mSampleRate = m_sampleRate;
    streamFormat.mFormatID = kAudioFormatLinearPCM;
    streamFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    streamFormat.mBitsPerChannel = 16;
    streamFormat.mChannelsPerFrame = m_channels;
    streamFormat.mBytesPerFrame = streamFormat.mChannelsPerFrame * (streamFormat.mBitsPerChannel / 8);
    streamFormat.mFramesPerPacket = 1;
    streamFormat.mBytesPerPacket = streamFormat.mBytesPerFrame * streamFormat.mFramesPerPacket;
    
    status = AudioUnitSetProperty(m_audioUnit,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output,
                                  1, // Input element
                                  &streamFormat,
                                  sizeof(streamFormat));
    if (status != noErr)
    {
        LOG_ERROR("Falha ao configurar formato de áudio: " + std::to_string(status));
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
    }
    
    // Configurar callback
    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc = audioInputCallback;
    
    // Criar contexto
    m_context = new AudioCaptureContext;
    m_context->capture = this;
    m_context->bufferMutex = &m_bufferMutex;
    m_context->audioBuffer = &m_audioBuffer;
    
    callbackStruct.inputProcRefCon = m_context;
    
    status = AudioUnitSetProperty(m_audioUnit,
                                  kAudioOutputUnitProperty_SetInputCallback,
                                  kAudioUnitScope_Global,
                                  1, // Input element
                                  &callbackStruct,
                                  sizeof(callbackStruct));
    if (status != noErr)
    {
        LOG_ERROR("Falha ao configurar callback: " + std::to_string(status));
        delete m_context;
        m_context = nullptr;
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
    }
    
    // Inicializar AudioUnit
    status = AudioUnitInitialize(m_audioUnit);
    if (status != noErr)
    {
        LOG_ERROR("Falha ao inicializar AudioUnit: " + std::to_string(status));
        delete m_context;
        m_context = nullptr;
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
    }
    
    LOG_INFO("AudioUnit inicializado com sucesso");
    return true;
#else
    return false;
#endif
}

void AudioCaptureCoreAudio::cleanupAudioUnit()
{
#ifdef __APPLE__
    if (m_audioUnit)
    {
        AudioUnitUninitialize(m_audioUnit);
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
    }
    
    if (m_context)
    {
        delete m_context;
        m_context = nullptr;
    }
    
    m_audioComponent = nullptr;
    m_audioBuffer.clear();
#endif
}
