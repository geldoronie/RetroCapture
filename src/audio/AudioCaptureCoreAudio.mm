#include "AudioCaptureCoreAudio.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cstring>

#ifdef __APPLE__
#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreFoundation/CoreFoundation.h>

// Estrutura para passar dados para o callback (definida antes do uso)
struct AudioCaptureContext
{
    AudioCaptureCoreAudio* capture;
    AudioBus *bus;
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

    // Top-of-callback heartbeat (rate-limited) so we can tell apart:
    //   - callback never called: no "input callback HIT" log at all
    //   - called but AudioUnitRender fails: HIT log + status warning
    //   - called and rendered: HIT log + "pushed N samples" log below
    static std::atomic<unsigned> hitCount{0};
    const unsigned h = ++hitCount;
    if (h <= 3 || (h % 1000) == 0)
    {
        LOG_DEBUG("Core Audio input callback HIT #" + std::to_string(h) +
                 " (inNumberFrames=" + std::to_string(inNumberFrames) + ")");
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
    if (status != noErr && (h <= 3 || (h % 1000) == 0))
    {
        LOG_WARN("Core Audio AudioUnitRender returned status=" +
                 std::to_string(static_cast<int>(status)));
    }
    
    if (status == noErr && context->bus)
    {
        context->bus->push(tempBuffer.data(), tempBuffer.size());

        // Log the first few callbacks plus a periodic heartbeat so a
        // silently-not-firing callback (microphone permission denied,
        // device produces no samples) is obvious from the log.
        static std::atomic<unsigned> cbCount{0};
        const unsigned n = ++cbCount;
        if (n <= 3 || (n % 1000) == 0)
        {
            LOG_DEBUG("Core Audio input callback #" + std::to_string(n) +
                     " — pushed " + std::to_string(tempBuffer.size()) + " samples");
        }
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
    , m_bytesPerSample(2)
    , m_isOpen(false)
    , m_isCapturing(false)
#else
    : m_sampleRate(44100)
    , m_channels(2)
    , m_bytesPerSample(2)
    , m_isOpen(false)
    , m_isCapturing(false)
#endif
{
    m_bus = std::make_unique<AudioBus>(m_sampleRate, m_channels);
    // ~2 s of slack at 44.1 kHz stereo, matching AudioCapturePulse on
    // Linux. Encoder/recorder pulls every video frame, so this is
    // comfortably above expected fill.
    m_localTap = m_bus->createTap(static_cast<size_t>(m_sampleRate) * m_channels * 2);
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

// #109 — synthetic device id for capturing the system audio output.
static const char *kSystemAudioId = "system-audio";

bool AudioCaptureCoreAudio::open(const std::string &deviceName)
{
    if (m_isOpen)
    {
        LOG_WARN("Dispositivo de áudio já aberto, fechando primeiro");
        close();
    }

    // #109 — system audio (output) uses ScreenCaptureKit, not the mic
    // AudioUnit. No mic permission, no local monitor playback (the user
    // already hears the system; playing it back would feed back). The
    // SCStream is started in startCapture().
    m_systemAudio = (deviceName == kSystemAudioId);
    if (m_systemAudio)
    {
        // ScreenCaptureKit delivers 48 kHz stereo float; report that so
        // the encoder/muxer and the SCStream(s) all agree on the rate.
        m_sampleRate = 48000;
        m_channels   = 2;
        m_isOpen = true;
        LOG_INFO("AudioCaptureCoreAudio: system-audio source (ScreenCaptureKit) — "
                 "local monitor disabled to avoid feedback");
        return true;
    }

#ifdef __APPLE__
    // macOS 10.14+ gates Core Audio input behind the microphone
    // privacy setting. Without an explicit grant the AudioUnit
    // "opens" successfully but the input callback never fires and
    // no samples ever reach the bus — silent failure that looks
    // exactly like "monitor doesn't play". Surface the current
    // status in the log, and synchronously request access on first
    // run so the consent prompt has a chance to appear (only works
    // for binaries inside an .app bundle with `NSMicrophoneUsage-
    // Description` in Info.plist; raw command-line binaries get the
    // request immediately denied without UI).
    @autoreleasepool
    {
        AVAuthorizationStatus s =
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        const char *label = "unknown";
        switch (s)
        {
            case AVAuthorizationStatusNotDetermined: label = "NotDetermined"; break;
            case AVAuthorizationStatusRestricted:    label = "Restricted";    break;
            case AVAuthorizationStatusDenied:        label = "Denied";        break;
            case AVAuthorizationStatusAuthorized:    label = "Authorized";    break;
        }
        LOG_INFO(std::string("Core Audio microphone authorization: ") + label);

        if (s == AVAuthorizationStatusNotDetermined)
        {
            __block bool grantedFlag  = false;
            __block bool finishedFlag = false;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL granted) {
                grantedFlag  = granted;
                finishedFlag = true;
            }];
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:3.0];
            while (!finishedFlag &&
                   [[NSDate date] compare:deadline] == NSOrderedAscending)
            {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            }
            LOG_INFO(std::string("Core Audio microphone prompt result: ") +
                     (grantedFlag ? "granted" : "denied/timeout"));
        }
        else if (s == AVAuthorizationStatusDenied || s == AVAuthorizationStatusRestricted)
        {
            LOG_WARN("Core Audio microphone access denied — open System Settings → "
                     "Privacy & Security → Microphone and enable RetroCapture (or the "
                     "binary's parent process if running uncodesigned). No audio will "
                     "flow until access is granted.");
        }
    }

    if (!initializeAudioUnit(deviceName))
    {
        LOG_ERROR("Falha ao inicializar AudioUnit");
        return false;
    }

    m_isOpen = true;
    LOG_INFO("Dispositivo de áudio aberto: " + (deviceName.empty() ? "default" : deviceName));

    // Stand up the host-side monitor playback — Core Audio counterpart
    // of Linux's MonitorPlayback (pa_simple writer). Failure is
    // non-fatal: capture still works for encoder/recorder, the user
    // just won't hear the input through the default output device.
    if (!startMonitor())
    {
        LOG_WARN("AudioCaptureCoreAudio: host-side monitor failed to start; "
                 "input will not be audible through the default output");
    }
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

    stopMonitor();
    m_isOpen = false;
    LOG_INFO("Dispositivo de áudio fechado");
}

bool AudioCaptureCoreAudio::isOpen() const
{
    return m_isOpen;
}

size_t AudioCaptureCoreAudio::getSamples(std::vector<float> &samples)
{
    if (!m_isOpen || !m_isCapturing || !m_localTap)
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

    // #109 — synthetic "system audio" capture (ScreenCaptureKit loopback
    // of the whole computer's output). Flagged isMonitor so the UI groups
    // it and shows the feedback note.
    {
        AudioDeviceInfo sysAudio;
        sysAudio.id        = kSystemAudioId;
        sysAudio.name      = "System audio (ScreenCaptureKit)";
        sysAudio.available = true;
        sysAudio.isMonitor = true;
        devices.push_back(sysAudio);
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

    if (m_systemAudio)
    {
        // Brokered by the hub: routes the screen-capture stream's audio
        // when screen capture is active, else runs its own audio-only
        // SCStream — never two SCStreams at once (#109).
        SckSystemAudioHub::instance().requestSystemAudio(m_bus.get(), m_sampleRate, m_channels);
        m_isCapturing = true;
        return true;
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
    if (m_systemAudio)
    {
        SckSystemAudioHub::instance().releaseSystemAudio();
        return;
    }
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
    if (!m_isOpen || !m_isCapturing || !buffer || maxSamples == 0 || !m_localTap)
    {
        return 0;
    }
    return m_localTap->pull(buffer, maxSamples);
}

bool AudioCaptureCoreAudio::initializeAudioUnit(const std::string &requestedDeviceId)
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
    
    // Bind the AudioUnit to the user-selected Core Audio device. MUST
    // happen before reading the device's native stream format below,
    // otherwise we'd read the AudioUnit's default (44100 Hz / 2 ch /
    // float32) instead of the actual device's format.
    if (!requestedDeviceId.empty())
    {
        AudioDeviceID devId = 0;
        try
        {
            devId = static_cast<AudioDeviceID>(std::stoul(requestedDeviceId));
        }
        catch (...)
        {
            LOG_WARN("AudioCaptureCoreAudio: unparseable device id '" +
                     requestedDeviceId + "' — falling back to system default");
        }
        if (devId != 0)
        {
            OSStatus bindStatus = AudioUnitSetProperty(m_audioUnit,
                                                       kAudioOutputUnitProperty_CurrentDevice,
                                                       kAudioUnitScope_Global,
                                                       0,
                                                       &devId,
                                                       sizeof(devId));
            if (bindStatus != noErr)
            {
                LOG_WARN("AudioCaptureCoreAudio: CurrentDevice set failed "
                         "(status=" + std::to_string(bindStatus) +
                         "), falling back to system default");
            }
            else
            {
                LOG_INFO("AudioCaptureCoreAudio: bound AudioUnit to device id " +
                         std::to_string(devId));
            }
        }
    }

    // Read the device's native stream format BEFORE setting our
    // requested format. Hard-coding 44.1 kHz / 2 ch / 16-bit caused
    // `AudioUnitRender` to return `-10863
    // (kAudioUnitErr_CannotDoInCurrentContext)` on UVC capture cards
    // that natively run at 48 kHz / mono / float32 — the AudioUnit's
    // internal converter can resample arbitrary rates but couldn't
    // bridge that big a gap in one shot. Pick up the device's actual
    // sample rate and channel count and use them; bit depth stays at
    // signed 16-bit interleaved (what the rest of the pipeline
    // expects, and what the converter does handle reliably).
    AudioStreamBasicDescription deviceFormat = {};
    UInt32 deviceFormatSize = sizeof(deviceFormat);
    OSStatus formatStatus = AudioUnitGetProperty(m_audioUnit,
                                                 kAudioUnitProperty_StreamFormat,
                                                 kAudioUnitScope_Input,
                                                 1,
                                                 &deviceFormat,
                                                 &deviceFormatSize);
    if (formatStatus == noErr)
    {
        LOG_INFO("Core Audio device native format: " +
                 std::to_string(static_cast<int>(deviceFormat.mSampleRate)) + " Hz, " +
                 std::to_string(deviceFormat.mChannelsPerFrame) + " ch, " +
                 std::to_string(deviceFormat.mBitsPerChannel) + "-bit");
        if (deviceFormat.mSampleRate > 0)
        {
            m_sampleRate = static_cast<uint32_t>(deviceFormat.mSampleRate);
        }
        if (deviceFormat.mChannelsPerFrame > 0)
        {
            m_channels = deviceFormat.mChannelsPerFrame;
        }
    }
    else
    {
        LOG_WARN("Core Audio: failed to query device native format (status=" +
                 std::to_string(formatStatus) +
                 "), falling back to hardcoded 44100 Hz / 2 ch");
    }

    // Build our requested format using the device's sample rate /
    // channel count so the AudioUnit only has to handle bit-depth
    // conversion (float / 24-bit / 32-bit → signed 16-bit). Some
    // mono devices will deliver a single channel; the rest of the
    // bus / monitor pipeline handles channel counts > 0 fine.
    AudioStreamBasicDescription streamFormat = {};
    streamFormat.mSampleRate       = m_sampleRate;
    streamFormat.mFormatID         = kAudioFormatLinearPCM;
    streamFormat.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    streamFormat.mBitsPerChannel   = 16;
    streamFormat.mChannelsPerFrame = m_channels;
    streamFormat.mBytesPerFrame    = streamFormat.mChannelsPerFrame * (streamFormat.mBitsPerChannel / 8);
    streamFormat.mFramesPerPacket  = 1;
    streamFormat.mBytesPerPacket   = streamFormat.mBytesPerFrame * streamFormat.mFramesPerPacket;

    status = AudioUnitSetProperty(m_audioUnit,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output,
                                  1,
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
    m_context->bus     = m_bus.get();
    
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
#endif
}

#ifdef __APPLE__

// Forward declaration: defined in AudioOutputCoreAudio.h.
// We don't include that header at the top of this file because it
// would pull Core Audio macros transitively into header consumers
// that don't need them.
extern std::unique_ptr<IAudioOutput> createAudioOutputCoreAudio();

bool AudioCaptureCoreAudio::startMonitor()
{
    if (m_monitor)
    {
        return true;
    }
    if (!m_bus)
    {
        return false;
    }

    m_monitor = createAudioOutputCoreAudio();
    if (!m_monitor)
    {
        LOG_ERROR("AudioCaptureCoreAudio: createAudioOutputCoreAudio returned null");
        return false;
    }
    if (!m_monitor->open("", m_sampleRate, m_channels))
    {
        LOG_ERROR("AudioCaptureCoreAudio: monitor open() failed");
        m_monitor.reset();
        return false;
    }
    if (!m_monitor->start())
    {
        LOG_ERROR("AudioCaptureCoreAudio: monitor start() failed");
        m_monitor->close();
        m_monitor.reset();
        return false;
    }
    m_monitor->setEnabled(true);

    // ~2 s slack on the monitor tap, matching the local-tap capacity
    // (and Linux's MonitorPlayback tap). Drop-oldest at this cap is
    // the safety net if the playback path stalls; the writer loop
    // does NOT actively skip — same minimal posture as Linux.
    m_monitorTap = m_bus->createTap(static_cast<size_t>(m_sampleRate) * m_channels * 2);
    m_monitorResyncPending = false;
    m_monitorRunning       = true;
    m_monitorThread        = std::thread(&AudioCaptureCoreAudio::monitorWriterLoop, this);

    LOG_INFO("AudioCaptureCoreAudio: monitor started (" +
             std::to_string(m_monitor->getSampleRate()) + " Hz x " +
             std::to_string(m_monitor->getChannels()) + " ch)");
    return true;
}

void AudioCaptureCoreAudio::stopMonitor()
{
    m_monitorRunning = false;
    if (m_monitorThread.joinable())
    {
        m_monitorThread.join();
    }
    if (m_monitor)
    {
        m_monitor->stop();
        m_monitor->close();
        m_monitor.reset();
    }
    m_monitorTap.reset();
    m_monitorResyncPending = false;
}

void AudioCaptureCoreAudio::monitorWriterLoop()
{
    // ~10 ms chunks at 44.1 kHz stereo, same sizing as Linux's
    // MonitorPlayback writer.
    constexpr size_t kChunkSamples = 882 * 2;
    std::vector<int16_t> chunk(kChunkSamples);
    std::vector<int16_t> drain;

    while (m_monitorRunning.load())
    {
        if (m_monitorResyncPending.exchange(false))
        {
            // Drop everything queued in the tap so the next write
            // restarts from the producer's newest samples.
            const size_t available = m_monitorTap ? m_monitorTap->available() : 0;
            if (available > 0)
            {
                drain.resize(available);
                m_monitorTap->pull(drain.data(), available);
            }
            LOG_INFO("AudioCaptureCoreAudio: monitor resync (dropped " +
                     std::to_string(available / (m_channels ? m_channels : 1)) +
                     " queued frames)");
        }

        if (!m_monitorTap || !m_monitor)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const size_t got = m_monitorTap->pull(chunk.data(), chunk.size());
        if (got == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (m_monitor->isEnabled() && m_monitor->isOpen())
        {
            const size_t written = m_monitor->write(chunk.data(), got);
            // Mirror the input-callback diagnostic — first few iterations
            // plus a periodic heartbeat so an open-but-silent monitor
            // playback path can be told apart from an empty input.
            static std::atomic<unsigned> wCount{0};
            const unsigned n = ++wCount;
            if (n <= 3 || (n % 1000) == 0)
            {
                LOG_DEBUG("Monitor writer iter #" + std::to_string(n) +
                         " — pulled " + std::to_string(got) +
                         " samples, wrote " + std::to_string(written));
            }
        }
    }
}

void AudioCaptureCoreAudio::resyncMonitor()
{
    m_monitorResyncPending = true;
}

#else // !__APPLE__

bool AudioCaptureCoreAudio::startMonitor() { return false; }
void AudioCaptureCoreAudio::stopMonitor()  {}
void AudioCaptureCoreAudio::monitorWriterLoop() {}
void AudioCaptureCoreAudio::resyncMonitor() {}

#endif
