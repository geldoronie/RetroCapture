#include "AudioCaptureFactory.h"

#ifdef __linux__
#include "AudioCapturePulse.h"
#elif defined(_WIN32)
#include "AudioCaptureWASAPI.h"
#endif

std::unique_ptr<IAudioCapture> AudioCaptureFactory::create()
{
#ifdef __linux__
    return std::make_unique<AudioCapturePulse>();
#elif defined(_WIN32)
    return std::make_unique<AudioCaptureWASAPI>();
#else
    #error "Unsupported platform"
#endif
}
