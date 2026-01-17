// Helper file para criar VideoCaptureAVFoundation sem incluir header Objective-C em arquivo C++
#include "VideoCaptureAVFoundation.h"
#include "IVideoCapture.h"
#include <memory>

std::unique_ptr<IVideoCapture> createVideoCaptureAVFoundation()
{
    return std::make_unique<VideoCaptureAVFoundation>();
}
