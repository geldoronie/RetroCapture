// Helper file para criar AudioCaptureCoreAudio sem incluir header Objective-C em arquivo C++
#include "AudioCaptureCoreAudio.h"
#include "IAudioCapture.h"
#include <memory>

std::unique_ptr<IAudioCapture> createAudioCaptureCoreAudio()
{
    return std::make_unique<AudioCaptureCoreAudio>();
}
