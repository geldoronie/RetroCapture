#include "VideoCaptureFactory.h"

#ifdef __linux__
#include "VideoCaptureV4L2.h"
#elif defined(_WIN32)
#include "VideoCaptureDS.h"
#elif defined(__APPLE__)
// Para macOS, precisamos incluir o header Objective-C++
// Mas isso só funciona se o arquivo for compilado como Objective-C++
// Como este arquivo é .cpp, vamos criar uma função helper no .mm
extern std::unique_ptr<IVideoCapture> createVideoCaptureAVFoundation();
#endif

std::unique_ptr<IVideoCapture> VideoCaptureFactory::create()
{
#ifdef __linux__
    return std::make_unique<VideoCaptureV4L2>();
#elif defined(_WIN32)
    return std::make_unique<VideoCaptureDS>();
#elif defined(__APPLE__)
    return createVideoCaptureAVFoundation();
#else
    #error "Unsupported platform"
#endif
}
