#include "uvc_linux.h"
#include <string.h>
#include <iostream>
#include <fcntl.h>
#include <chrono>

int main(int argc, char** argv)
{
    int video_modes_available;
    video_device_mode_info_t video_modes[128];
    video_device_mode_info_t requested_mode;
    video_device_mode_info_t used_mode;
    bool have_good_mode;

    ///dev/video0 Astra Pro HD Camera 1448695129 YUYV 4:2:2 1280 x 720
    memset(&used_mode, 0, sizeof(used_mode));
    requested_mode.width = 1280;
    requested_mode.height = 720;
    memset(requested_mode.dev_name, '\0', 256);
    strcpy(requested_mode.dev_name, "Astra Pro HD Camera");
    //requested_mode.pixel_format = UVC_PIXELFORMAT_YUV422;
    requested_mode.pixel_format = 1448695129;

    for (int i = 0; i < 128; i++)
        memset(&video_modes[i], 0, sizeof(video_device_mode_info_t));

    int devmodes = 0;

    uvc_enumerate(video_modes, 128, &devmodes);

    for (int i = 0; i < devmodes; i++)
    {
        std::cout << video_modes[i].dev_filename << " " << video_modes[i].dev_name << " "
                  << video_modes[i].pixel_format << " " << video_modes[i].pixel_format_desc << " "
                  << video_modes[i].width << " x " << video_modes[i].height << std::endl;

        if (strstr(video_modes[i].dev_name, requested_mode.dev_name) != NULL)
        {
            if (video_modes[i].width == requested_mode.width && video_modes[i].height == requested_mode.height
                    && video_modes[i].pixel_format == requested_mode.pixel_format)
            {
                std::cout << "Found requested mode: " << video_modes[i].dev_filename << ", " <<
                             video_modes[i].dev_name << ", " << video_modes[i].pixel_format <<
                             ", " << video_modes[i].width << ", " << video_modes[i].height << std::endl;
                have_good_mode = true;
                used_mode = video_modes[i];
            }
        }
    }

    if (devmodes == 0)
    {
        std::cout << "No video devices available." << std::endl;
        return 1;
    }

    if (have_good_mode == false)
    {
        used_mode = video_modes[0];
        std::cout << "There are no devices matching the requirements: " << std::endl;
        std::cout << requested_mode.dev_name << ", " << requested_mode.width << ", " << requested_mode.height << std::endl;
    }

    char devname[256];
    memset(devname, '\0', 256);

    std::cout << "using video mode:" << std::endl;
    std::cout << used_mode.dev_filename << ", " << used_mode.dev_name
              << ", " << used_mode.width << ", " << used_mode.height << ", " << used_mode.pixel_format_desc << std::endl;
    strcpy(devname, used_mode.dev_filename);

    int dev_fd = open(devname, O_RDWR | O_NONBLOCK, 0);
    int errcode = 0;
    if (dev_fd == -1)
    {
        errcode = errno;
        fprintf(stderr, "Failed opening device: %s %s %d\n", devname, strerror(errcode), errcode);
        return false;
    }

    if (uvc_openDevice(dev_fd, &used_mode) != 0)
    {
        uvc_cleanup(dev_fd);
        return false;
    }

    if (uvc_openStream(dev_fd) != 0)
    {
        uvc_cleanup(dev_fd);
        return false;
    }


    unsigned char* colorbuf = new unsigned char[used_mode.width * used_mode.height * 3];

    while (true)
    {
        std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
        if (uvc_getData(dev_fd, colorbuf, &used_mode) != 0)
            return false;
        std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();

        int fps = 1000000 / dur;

        std::cout << "delta: " << dur <<  " us (" << fps << " FPS)" << std::endl;
    }

    return 0;
}

