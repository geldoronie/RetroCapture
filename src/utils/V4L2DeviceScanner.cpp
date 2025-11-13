#include "V4L2DeviceScanner.h"
#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <algorithm>

std::vector<std::string> V4L2DeviceScanner::scan()
{
    std::vector<std::string> devices;

    // Scan /dev/video* devices
    for (int i = 0; i < 32; ++i)
    {
        std::string devicePath = "/dev/video" + std::to_string(i);

        // Try to open device to check if it exists and is a V4L2 device
        int fd = open(devicePath.c_str(), O_RDWR | O_NONBLOCK);
        if (fd >= 0)
        {
            // Check if it's a video capture device
            struct v4l2_capability cap = {};
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) >= 0)
            {
                if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
                {
                    devices.push_back(devicePath);
                }
            }
            close(fd);
        }
    }

    // Sort devices
    std::sort(devices.begin(), devices.end());

    return devices;
}

