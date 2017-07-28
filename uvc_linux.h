#ifndef __UVC_LINUX_H_
#define __UVC_LINUX_H_

#include <stdint.h>

#define UVC_PIXELFORMAT_YUV422 1448695129
#define UVC_PIXELFORMAT_Y8I 541669465

struct video_device_mode_info_t
{
    unsigned int width;
    unsigned int height;
    unsigned int bytes_per_pixel;
    int pixel_format;
    char pixel_format_desc[32];
    char dev_filename[128];
    char dev_name[256];
};

/**
 * @brief Fills the given video_modes container with up to count elements
 * This function uses /sys/class/video4linux to find any video devices and queries their
 * properties.
 * @param video_modes: A struct to be filled with data
 * @param count: Max number of elements that can be inserted in the video_modes
 * @param devmodes: Will be filled with the number of video devices added in the struct
 * @return 0 on success
 */
extern int uvc_enumerate(video_device_mode_info_t* video_modes, int count, int* devmodes);
extern int uvc_do_ioctl(int dev_fd, int request, void* argument);

/**
 * @brief Opens the video device associated with the given file descriptor using the requested video mode
 * @param dev_fd: An open file descriptor to a video device
 * @param vmode: The video mode that is to be opened
 *               If the video is not suitable, this struct will be modified with the mode that was actually used
 * @return 0 on success
 */
int uvc_openDevice(int dev_fd, video_device_mode_info_t* vmode);
extern int uvc_cleanup(int dev_fd);
extern int uvc_openStream(int dev_fd);
extern int uvc_closeStream(int dev_fd);

/**
 * @brief Fills the given buffer with new video data from the given device file descriptor
 * @param dev_fd: An open file descriptor that's outputting video streams to the device file descriptor
 * @param color_dest: A buffer of vmode->width * vmode->height * 3 size, to be filled with RGB data
 * @param vmode: The video mode that was used to open the stream
 * @return 0 on success
 */
int uvc_getData(int dev_fd, unsigned char* color_dest, video_device_mode_info_t* vmode);

#endif // __UVC_LINUX_H_

