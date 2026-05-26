#include "AudioOutputCoreAudio.h"
#include "IAudioOutput.h"

#include <memory>

// Tiny .mm shim so a C++ caller can instantiate AudioOutputCoreAudio
// without pulling the Core Audio Objective-C++ headers across the
// translation unit boundary. Mirrors the same pattern used for
// VideoCaptureAVFoundationFactory / AudioCaptureCoreAudioFactory.
std::unique_ptr<IAudioOutput> createAudioOutputCoreAudio()
{
    return std::make_unique<AudioOutputCoreAudio>();
}
