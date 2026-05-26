#include "AudioOutputCoreAudio.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cmath>

#ifdef __APPLE__
#import <CoreAudio/CoreAudio.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreFoundation/CoreFoundation.h>

// Context structure for audio output callback
struct AudioOutputContext
{
    AudioOutputCoreAudio* output;
    std::mutex* bufferMutex;
    std::vector<int16_t>* audioBuffer;
    std::atomic<float>* volume;
    std::atomic<bool>* enabled;
};

// Audio output render callback
static OSStatus audioOutputCallback(void* inRefCon,
                                     AudioUnitRenderActionFlags* ioActionFlags,
                                     const AudioTimeStamp* inTimeStamp,
                                     UInt32 inBusNumber,
                                     UInt32 inNumberFrames,
                                     AudioBufferList* ioData)
{
    (void)ioActionFlags;
    (void)inTimeStamp;
    (void)inBusNumber;
    
    AudioOutputContext* context = (AudioOutputContext*)inRefCon;
    if (!context || !context->output || !context->enabled || !context->enabled->load())
    {
        // Output silence if disabled or context invalid
        for (UInt32 i = 0; i < ioData->mNumberBuffers; ++i)
        {
            memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
        }
        return noErr;
    }
    
    std::lock_guard<std::mutex> lock(*context->bufferMutex);
    
    // Get samples from buffer
    size_t samplesNeeded = inNumberFrames * ioData->mBuffers[0].mNumberChannels;
    size_t samplesAvailable = context->audioBuffer->size();
    size_t samplesToCopy = std::min(samplesNeeded, samplesAvailable);
    
    if (samplesToCopy > 0)
    {
        // Copy samples to output buffer
        int16_t* outputBuffer = (int16_t*)ioData->mBuffers[0].mData;
        const int16_t* inputBuffer = context->audioBuffer->data();
        
        // Apply volume and copy
        float volume = context->volume->load();
        for (size_t i = 0; i < samplesToCopy; ++i)
        {
            outputBuffer[i] = static_cast<int16_t>(inputBuffer[i] * volume);
        }
        
        // Remove copied samples from buffer
        context->audioBuffer->erase(context->audioBuffer->begin(),
                                    context->audioBuffer->begin() + samplesToCopy);
        
        // Fill remaining with silence if needed
        if (samplesToCopy < samplesNeeded)
        {
            memset(outputBuffer + samplesToCopy, 0, (samplesNeeded - samplesToCopy) * sizeof(int16_t));
        }
    }
    else
    {
        // No samples available, output silence
        for (UInt32 i = 0; i < ioData->mNumberBuffers; ++i)
        {
            memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
        }
    }
    
    return noErr;
}
#endif

AudioOutputCoreAudio::AudioOutputCoreAudio()
#ifdef __APPLE__
    : m_audioUnit(nullptr)
    , m_audioComponent(nullptr)
    , m_context(nullptr)
    , m_bufferMutex()
    , m_audioBuffer()
    , m_volume(1.0f)
    , m_enabled(true)
    , m_sampleRate(44100)
    , m_channels(2)
    , m_isOpen(false)
    , m_isRunning(false)
#else
    : m_sampleRate(44100)
    , m_channels(2)
    , m_isOpen(false)
    , m_isRunning(false)
#endif
{
}

AudioOutputCoreAudio::~AudioOutputCoreAudio()
{
    close();
    cleanupAudioUnit();
}

bool AudioOutputCoreAudio::open(const std::string& deviceName, uint32_t sampleRate, uint32_t channels)
{
    if (m_isOpen)
    {
        LOG_WARN("Audio output already open, closing first");
        close();
    }

    // Update sample rate and channels if provided
    if (sampleRate > 0)
    {
        m_sampleRate = sampleRate;
    }
    if (channels > 0)
    {
        m_channels = channels;
    }

#ifdef __APPLE__
    if (!initializeAudioUnit())
    {
        LOG_ERROR("Failed to initialize AudioUnit for output");
        return false;
    }

    m_isOpen = true;
    LOG_INFO("Audio output opened: " + (deviceName.empty() ? "default" : deviceName) + 
             " (" + std::to_string(m_sampleRate) + "Hz, " + std::to_string(m_channels) + " channels)");
    return true;
#else
    (void)deviceName;
    LOG_ERROR("Core Audio output only available on macOS");
    return false;
#endif
}

void AudioOutputCoreAudio::close()
{
    if (m_isRunning)
    {
        stop();
    }

    m_isOpen = false;
    LOG_INFO("Audio output closed");
}

bool AudioOutputCoreAudio::isOpen() const
{
    return m_isOpen;
}

bool AudioOutputCoreAudio::start()
{
    if (!m_isOpen)
    {
        LOG_ERROR("Cannot start audio output: not open");
        return false;
    }

    if (m_isRunning)
    {
        LOG_WARN("Audio output already running");
        return true;
    }

#ifdef __APPLE__
    OSStatus status = AudioOutputUnitStart(m_audioUnit);
    if (status != noErr)
    {
        LOG_ERROR("Failed to start audio output: " + std::to_string(status));
        return false;
    }

    m_isRunning = true;
    LOG_INFO("Audio output started");
    return true;
#else
    return false;
#endif
}

void AudioOutputCoreAudio::stop()
{
    if (!m_isRunning)
    {
        return;
    }

#ifdef __APPLE__
    OSStatus status = AudioOutputUnitStop(m_audioUnit);
    if (status != noErr)
    {
        LOG_ERROR("Failed to stop audio output: " + std::to_string(status));
    }
#endif

    m_isRunning = false;
    
    // Clear buffer
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_audioBuffer.clear();
    }
    
    LOG_INFO("Audio output stopped");
}

size_t AudioOutputCoreAudio::write(const int16_t* samples, size_t numSamples)
{
    if (!m_isOpen || !samples || numSamples == 0 || !m_enabled.load())
    {
        return 0;
    }

#ifdef __APPLE__
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    
    // Limit buffer size to prevent excessive memory usage (keep max 1 second of audio)
    size_t maxSamples = m_sampleRate * m_channels;
    if (m_audioBuffer.size() + numSamples > maxSamples)
    {
        // Remove oldest samples to make room
        size_t samplesToRemove = (m_audioBuffer.size() + numSamples) - maxSamples;
        m_audioBuffer.erase(m_audioBuffer.begin(), m_audioBuffer.begin() + samplesToRemove);
    }
    
    // Add new samples
    m_audioBuffer.insert(m_audioBuffer.end(), samples, samples + numSamples);
    
    // Debug: log occasionally to verify audio is being written
    static size_t totalSamplesWritten = 0;
    totalSamplesWritten += numSamples;
    if (totalSamplesWritten % (m_sampleRate * m_channels) < numSamples) // Log once per second
    {
        LOG_INFO("Audio output: " + std::to_string(m_audioBuffer.size()) + " samples in buffer, " +
                 std::to_string(totalSamplesWritten) + " total samples written");
    }
    
    return numSamples;
#else
    (void)samples;
    (void)numSamples;
    return 0;
#endif
}

uint32_t AudioOutputCoreAudio::getSampleRate() const
{
    return m_sampleRate;
}

uint32_t AudioOutputCoreAudio::getChannels() const
{
    return m_channels;
}

void AudioOutputCoreAudio::setVolume(float volume)
{
    m_volume.store(std::max(0.0f, std::min(1.0f, volume)));
}

float AudioOutputCoreAudio::getVolume() const
{
    return m_volume.load();
}

bool AudioOutputCoreAudio::isEnabled() const
{
    return m_enabled.load();
}

void AudioOutputCoreAudio::setEnabled(bool enabled)
{
    m_enabled.store(enabled);
}

#ifdef __APPLE__
bool AudioOutputCoreAudio::initializeAudioUnit()
{
    // Audio component description for output
    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    
    // Find component
    m_audioComponent = AudioComponentFindNext(nullptr, &desc);
    if (!m_audioComponent)
    {
        LOG_ERROR("Failed to find AudioComponent for output");
        return false;
    }
    
    // Create instance
    OSStatus status = AudioComponentInstanceNew(m_audioComponent, &m_audioUnit);
    if (status != noErr)
    {
        LOG_ERROR("Failed to create AudioUnit: " + std::to_string(status));
        return false;
    }
    
    // Configure audio format
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
                                  kAudioUnitScope_Input,
                                  0, // Output element
                                  &streamFormat,
                                  sizeof(streamFormat));
    if (status != noErr)
    {
        LOG_ERROR("Failed to set audio format: " + std::to_string(status));
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
    }
    
    // Set render callback
    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc = audioOutputCallback;
    
    // Create context
    m_context = new AudioOutputContext;
    m_context->output = this;
    m_context->bufferMutex = &m_bufferMutex;
    m_context->audioBuffer = &m_audioBuffer;
    m_context->volume = &m_volume;
    m_context->enabled = &m_enabled;
    
    callbackStruct.inputProcRefCon = m_context;
    
    status = AudioUnitSetProperty(m_audioUnit,
                                  kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input,
                                  0, // Output element
                                  &callbackStruct,
                                  sizeof(callbackStruct));
    if (status != noErr)
    {
        LOG_ERROR("Failed to set render callback: " + std::to_string(status));
        delete m_context;
        m_context = nullptr;
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
    }
    
    // Initialize audio unit
    status = AudioUnitInitialize(m_audioUnit);
    if (status != noErr)
    {
        LOG_ERROR("Failed to initialize AudioUnit: " + std::to_string(status));
        delete m_context;
        m_context = nullptr;
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
    }
    
    return true;
}

void AudioOutputCoreAudio::cleanupAudioUnit()
{
    if (m_audioUnit)
    {
        if (m_isRunning)
        {
            AudioOutputUnitStop(m_audioUnit);
        }
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
}
#endif
