#include "VideoCaptureFactory.h"

#ifdef __linux__
#include "VideoCaptureV4L2.h"
#elif defined(_WIN32)
#include "VideoCaptureDS.h"
#endif

std::unique_ptr<IVideoCapture> VideoCaptureFactory::create()
{
#ifdef __linux__
    return std::make_unique<VideoCaptureV4L2>();
#elif defined(_WIN32)
    return std::make_unique<VideoCaptureDS>();
#else
    #error "Unsupported platform"
#endif
}
