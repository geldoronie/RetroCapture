#include "AudioCaptureFactory.h"

#ifdef __linux__
#include "AudioCapturePulse.h"
#elif defined(_WIN32)
#include "AudioCaptureWASAPI.h"
#elif defined(__APPLE__)
// Para macOS, precisamos incluir o header Objective-C++
// Mas isso só funciona se o arquivo for compilado como Objective-C++
// Como este arquivo é .cpp, vamos criar uma função helper no .mm
extern std::unique_ptr<IAudioCapture> createAudioCaptureCoreAudio();
#endif

std::unique_ptr<IAudioCapture> AudioCaptureFactory::create()
{
#ifdef __linux__
    return std::make_unique<AudioCapturePulse>();
#elif defined(_WIN32)
    return std::make_unique<AudioCaptureWASAPI>();
#elif defined(__APPLE__)
    return createAudioCaptureCoreAudio();
#else
    #error "Unsupported platform"
#endif
}
