#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h> 
#include <linux/videodev2.h>
#include <assert.h>
#include "uvc_linux.h"

char devname[512];
unsigned int n_buffers = 0;

#define thread_count 4
#define STREAM_BUFFERS 20

struct buffer {
    void* start;
    size_t length;
};

struct buffer* buffers = NULL;
int framecount = 0;

void remove_all_chars(char* str, char c) {
    char *pr = str, *pw = str;
    while (*pr)
    {
        *pw = *pr++;
        pw += (*pw != c);
    }
    *pw = '\0';
}


#define CLIP(x) ( (x)>=0xFF ? 0xFF : ( (x) <= 0x00 ? 0x00 : (x) ) )

struct parse_uvc_image_params
{
    int start_y;
    int end_y;
    int width;
    unsigned char* src;
    unsigned char* src_origin;
    unsigned char* dst_rgb;
    unsigned char* dst_rgb_origin;
};

void uvc_convertYUV422(void* params)
{
    parse_uvc_image_params* p = (parse_uvc_image_params*)params;
    unsigned char *py, *pu, *pv;

    /* In this format each four bytes is two pixels. Each four bytes is two Y's, a Cb and a Cr.
       Each Y goes to one of the pixels, and the Cb and Cr belong to both pixels. */
    py = p->src;
    pu = p->src + 1;
    pv = p->src + 3;
    unsigned char *tmp = p->dst_rgb;

    int line, column;
    for (line = p->start_y; line < p->end_y; ++line)
    {
        for (column = 0; column < p->width; ++column)
        {
            *tmp++ = CLIP((float)*py + 1.402*((float)*pv-128.0));
            *tmp++ = CLIP((float)*py - 0.344*((float)*pu-128.0) - 0.714*((float)*pv-128.0));
            *tmp++ = CLIP((float)*py + 1.772*((float)*pu-128.0));

            // increase py every time
            py += 2;
            // increase pu,pv every second time
            if ((column & 1) == 1)
            {
                pu += 4;
                pv += 4;
            }
        }
    }
}

void uvc_convertY8I(void* params)
{
    /*
     * Grayscale stereo image where 2 images are packed into one
     * Each pixel of the data contains a single 16-bit value that
     * has the first 8 bits describing the color of image one
     * and the next 8 bits describing the color of image two
     */

    parse_uvc_image_params* p = (parse_uvc_image_params*)params;
    int line, column;
    uint16_t* src16 = (uint16_t*)p->src_origin;
    uint16_t temp;

    for (line = p->start_y; line < p->end_y; ++line)
    {
        for (column = 0; column < p->width; ++column)
        {
            temp = src16[line * p->width + column];
            p->dst_rgb_origin[3 * (line * p->width * 2 + column)] = temp >> 8;
            p->dst_rgb_origin[3 * (line * p->width * 2 + column + p->width)] = 0xFF & temp;
        }
    }
}

// Try ioctl until ioctl completes with an error other than EINTR
int uvc_do_ioctl(int dev_fd, int request, void* argument)
{
    int ret = 0;
    int errcode = 0;
    do
    {
        ret = ioctl(dev_fd, request, argument);
        errcode = errno;
        // EINTR happens when some signal causes an interrupt, requiring calling ioctl again
    } while (ret == -1 && errcode == EINTR);

    return ret;
}

int uvc_enumerate(video_device_mode_info_t* video_modes, int count, int* devmodes)
{
    DIR* d;
    struct dirent *dir;
    d = opendir("/sys/class/video4linux");
    int base_len = strlen("/sys/class/video4linux/");
    int sub_len = strlen("/name");
    int file_len = 0;

    char name_path[1024];
    char dev_path[1024];
    FILE* read_name = NULL;
    char dev_basename[512];
    char dev_human_name[256];
    int dev_fd = 0;
    struct v4l2_capability capabilities;
    struct v4l2_fmtdesc format_desc;
    struct v4l2_frmsizeenum frame_size;
    int index = 0;
    int index2 = 0;
    int ret = 0;
    int inserted = 0;

    if (d)
    {
        struct stat stat_s;
        while ((dir = readdir(d)) != NULL)
        {
            if (strcmp(dir->d_name, ".") == 0)
                continue;
            if (strcmp(dir->d_name, "..") == 0)
                continue;

            int name_len = strlen(dir->d_name);
            name_len += base_len + sub_len;
            if (name_len > 1024)
            {
                fprintf(stderr, "Too long device name: %s\n", dir->d_name);
                continue;
            }

            strcpy(name_path, "/sys/class/video4linux/");
            strcat(name_path, dir->d_name);
            strcat(name_path, "/name");
            memset(dev_basename, '\0', 512);

            if (strlen(dir->d_name) > 64)
            {
                fprintf(stderr, "Device basename too long: %s\n", dir->d_name);
                continue;
            }

            strncpy(dev_basename, dir->d_name, strlen(dir->d_name));

            if (stat(name_path, &stat_s) == -1)
            {
                fprintf(stderr, "No name file for device: %s\n", name_path);
                continue;
            }

            if (S_ISREG(stat_s.st_mode) == 0)
            {
                fprintf(stderr, "Path is not a regular file: %s\n", name_path);
                continue;
            }

            memset(dev_path, '\0', 1024);
            strcpy(dev_path, "/dev/");
            strcat(dev_path, dev_basename);
            if (stat(dev_path, &stat_s) == -1)
            {
                fprintf(stderr, "Device file does not exist: %s\n", dev_path);
                continue;
            }

            read_name = fopen(name_path, "r");
            if (read_name == NULL)
            {
                fprintf(stderr, "Failed opening %s for reading\n", name_path);
                continue;
            }

            fseek(read_name, 0, SEEK_END);
            file_len = ftell(read_name);
            fseek(read_name, 0, SEEK_SET);

            char* buf = (char*)calloc(file_len, 1);
            if (buf == NULL)
            {
                fprintf(stderr, "Failed allocating %d bytes for reading file name %s\n", file_len, name_path);
                continue;
            }

            fread(buf, 1, file_len, read_name);
            fclose(read_name);

            remove_all_chars(buf, '\n');
            remove_all_chars(buf, '\r');
            //fprintf(stderr, "video device: %s -> %s (%s)\n", buf, dev_basename, name_path);

            int copy_size = strlen(buf);
            if (copy_size > 255)
                copy_size = 255;
            strncpy(dev_human_name, buf, copy_size);

            free(buf);

            dev_fd = open(dev_path, O_RDWR | O_NONBLOCK, 0);

            if (uvc_do_ioctl(dev_fd, VIDIOC_QUERYCAP, &capabilities) == -1)
            {
                fprintf(stderr, "ioctl VIDIOC_QUERYCAP failed: %s\n", dev_path);
                close(dev_fd);
                continue;
            }

            if (!(capabilities.capabilities & V4L2_CAP_VIDEO_CAPTURE))
            {
                fprintf(stderr, "Device does not support V4L2_CAP_VIDEO_CAPTURE: %s\n", dev_path);
                close(dev_fd);
                continue;
            }

            if (!(capabilities.capabilities & V4L2_CAP_STREAMING))
            {
                fprintf(stderr, "Device does not support V4L2_CAP_STREAMING, cannot mmap: %s\n", dev_path);
                close(dev_fd);
                continue;
            }

            for (index = 0; index < 1000; index++)
            {
                memset(&format_desc, 0, sizeof(format_desc));
                format_desc.index = index;
                format_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

                ret = ioctl(dev_fd, VIDIOC_ENUM_FMT, &format_desc);
                if (ret == EINVAL || ret == -1)
                    break;
                else if (ret != 0)
                {
                    fprintf(stderr, "ioctl VIDIOC_ENUM_FMT failed %d for device: %s\n", ret, dev_path);
                    break;
                }

                //fprintf(stderr, "\t%s (fourcc: %d)\n", format_desc.description, format_desc.pixelformat );

                index2 = 0;
                memset(&frame_size, 0, sizeof(frame_size));
                frame_size.pixel_format = format_desc.pixelformat;
                frame_size.index = index2;

                while (ioctl(dev_fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) >= 0)
                {
                    video_device_mode_info_t vmode;

                    vmode.pixel_format = frame_size.pixel_format;
                    strcpy(vmode.pixel_format_desc, (const char*)format_desc.description);
                    copy_size = strlen(dev_path);
                    if (copy_size > 128)
                    {
                        fprintf(stderr, "Truncated device name %s\n", dev_path);
                        copy_size = 128;
                    }
                    memset(vmode.dev_filename, '\0', 128);
                    memset(vmode.dev_name, '\0', 256);
                    strncpy(vmode.dev_filename, dev_path, copy_size);
                    strcpy(vmode.dev_name, dev_human_name);

                    if (frame_size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                    {
                        vmode.width = frame_size.discrete.width;
                        vmode.height = frame_size.discrete.height;
                    }
                    else if (frame_size.type == V4L2_FRMSIZE_TYPE_STEPWISE)
                    {
                        vmode.width = frame_size.stepwise.max_width;
                        vmode.height = frame_size.stepwise.max_height;
                    }

                    index2++;
                    frame_size.pixel_format = format_desc.pixelformat;

                    switch (format_desc.pixelformat)
                    {
                    case UVC_PIXELFORMAT_Y8I:
                        vmode.bytes_per_pixel = 2;
                        break;
                    case UVC_PIXELFORMAT_YUV422:
                        vmode.bytes_per_pixel = 2;
                        break;
                    default:
                        fprintf(stderr, "Unknown pixel format for video mode: %d. Assuming bytes_per_pixel = 2\n", format_desc.pixelformat);
                        vmode.bytes_per_pixel = 2;
                        break;
                    }

                    frame_size.index = index2;

                    if (count > inserted)
                    {
                        video_modes[inserted] = vmode;
                        inserted++;
                    }
                }
            }

            close(dev_fd);
        }

        closedir(d);
    }

    *devmodes = inserted;
    return 0;
}

int uvc_openDevice(int dev_fd, video_device_mode_info_t* vmode)
{
    memset(devname, '\0', 512);
    strcpy(devname, vmode->dev_filename);
    struct v4l2_capability capabilities;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    struct v4l2_format format;
    unsigned int min;

    if (uvc_do_ioctl(dev_fd, VIDIOC_QUERYCAP, &capabilities) == -1)
    {
        fprintf(stderr, "ioctl VIDIOC_QUERYCAP failed: %s\n", devname);
        return 1;
    }

    if (!(capabilities.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        fprintf(stderr, "Device does not support V4L2_CAP_VIDEO_CAPTURE: %s\n", devname);
        return 1;
    }

    if (!(capabilities.capabilities & V4L2_CAP_STREAMING))
    {
        fprintf(stderr, "Device does not support V4L2_CAP_STREAMING, cannot mmap: %s\n", devname);
        return 1;
    }

    memset(&cropcap, 0, sizeof(cropcap));

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (uvc_do_ioctl(dev_fd, VIDIOC_CROPCAP, &cropcap) == 0)
    {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect;
        if (uvc_do_ioctl(dev_fd, VIDIOC_S_CROP, &crop))
        {
            int errcode = errno;
            switch (errcode)
            {
                case EINVAL:
                    // Does not support cropping
                    break;
                default:
                    break;
            }
        }
    }

    memset(&format, 0, sizeof(format));

    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = vmode->width;
    format.fmt.pix.height = vmode->height;
    format.fmt.pix.pixelformat = vmode->pixel_format;
    format.fmt.pix.field = V4L2_FIELD_NONE;

    if (uvc_do_ioctl(dev_fd, VIDIOC_S_FMT, &format) == -1)
    {
        fprintf(stderr, "Failed setting device format: %s\n", devname);
        return 1;
    }

    if (vmode->width != format.fmt.pix.width || vmode->height != format.fmt.pix.height)
    {
        fprintf(stderr, "device uses different resolution: Requested: %d x %d, got: %d x %d\n",
                vmode->width, vmode->height, format.fmt.pix.width, format.fmt.pix.height);
        vmode->width = format.fmt.pix.width;
        vmode->height = format.fmt.pix.height;
    }

    if (vmode->pixel_format != format.fmt.pix.pixelformat)
    {
        fprintf(stderr, "Device uses different pixelformat: requested: %d, got: %d\n", vmode->pixel_format, format.fmt.pix.pixelformat);
        vmode->pixel_format = format.fmt.pix.pixelformat;
    }

    min = format.fmt.pix.width * 2;
    if (format.fmt.pix.bytesperline < min)
        format.fmt.pix.bytesperline = min;
    min = format.fmt.pix.bytesperline * format.fmt.pix.height;
    if (format.fmt.pix.sizeimage < min)
        format.fmt.pix.sizeimage = min;

    fprintf(stderr, "UVC resolution negotiated to %d, %d\n", vmode->width, vmode->height);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));

    // The request buffer count has some considerations:
    // A high count will result in some frames being stored in memory if the UVC device
    // produces frames faster than we can read. This can result in a delay in the video feed,
    // but minimizes lost frames
    // A low count makes sure the frame we're reading is always the most updated. This allows
    // a video feed with no delays, but can result in data loss if we take too long to read a new frame
    req.count = STREAM_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    // request buffers from device
    int ret = uvc_do_ioctl(dev_fd, VIDIOC_REQBUFS, &req) == -1;
    if (ret != 0)
    {
        if (ret == EINVAL)
        {
            fprintf(stderr, "cannot use mmap for device: %s\n", devname);
            return 1;
        }
        else
        {
            fprintf(stderr, "other error with allocating mmap for device: %s %s %d\n", devname, strerror(ret), ret);
            return 1;
        }
    }

    // The device may not grant all the buffers
    if (req.count < STREAM_BUFFERS)
    {
        fprintf(stderr, "Device does not have enough memory: %s\n", devname);
        return 1;
    }

    buffers = (buffer*)calloc(req.count, sizeof(*buffers));
    if (!buffers)
    {
        fprintf(stderr, "Out of memory when allocating buffers\n");
        return 1;
    }

    // n_buffers is set to whatever req.count is
    for (n_buffers = 0; n_buffers < req.count; n_buffers++)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;

        if (uvc_do_ioctl(dev_fd, VIDIOC_QUERYBUF, &buf) == -1)
        {
            fprintf(stderr, "Failed getting buffers from device: %s\n", devname);
            return 1;
        }

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, buf.m.offset);
        if (buffers[n_buffers].start == MAP_FAILED)
        {
            fprintf(stderr, "Failed mmapping device memory: %s\n", devname);
            return 1;
        }
    }

    fprintf(stderr, "mmaped device memory to %d buffers\n", n_buffers);

    return 0;
}

int uvc_cleanup(int dev_fd)
{
    unsigned int i = 0;
    for (i = 0; i < n_buffers; i++)
    {
        if (munmap(buffers[i].start, buffers[i].length) == -1)
        {
            fprintf(stderr, "Failed unmapping memory\n");
        }
    }

    free(buffers);

    if (close(dev_fd) == -1)
    {
        fprintf(stderr, "Failed closing device fd\n");
        return 1;
    }

    return 1;
}

int uvc_openStream(int dev_fd)
{
    unsigned int i;
    enum v4l2_buf_type type;

    for (i = 0; i < n_buffers; ++i)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        // enqueues the buffer for device output
        if (uvc_do_ioctl(dev_fd, VIDIOC_QBUF, &buf) == -1)
        {
            fprintf(stderr, "ioctl VIDIOC_QBUF failed\n");
            return 1;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // Tells the device to start streaming data into the enqueued buffers
    if (uvc_do_ioctl(dev_fd, VIDIOC_STREAMON, &type) == -1)
    {
        fprintf(stderr, "ioctl VIDIOC_STREAMON failed\n");
        return 1;
    }

    return 0;
}

int uvc_closeStream(int dev_fd)
{
    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (uvc_do_ioctl(dev_fd, VIDIOC_STREAMOFF, &type) == -1)
    {
        fprintf(stderr, "ioctl VIDIOC_STREAMOFF failed\n");
        return 1;
    }
    return 0;
}

int uvc_getData(int dev_fd, unsigned char* color_dest, video_device_mode_info_t* vmode)
{
    for (;;)
    {
        // Create a file descriptor set for select to fill
        fd_set fds;
        struct timeval tv;
        int r;

        // Reset file descriptor states
        FD_ZERO(&fds);
        // Add the device fd as the only entry for fds to wait
        FD_SET(dev_fd, &fds);

        tv.tv_sec = 2;
        tv.tv_usec = 0;

        // Select waits until the file desctiptors up to the first param are updated
        // or if timeout happens
        r = select(dev_fd + 1, &fds, NULL, NULL, &tv);
        int errcode = errno;
        if (r == -1)
        {
            if (errcode == EINTR)
            {
                // Try again
                continue;
            }

            fprintf(stderr, "select failed: %s %d\n", strerror(errcode), errcode);
            return 1;
        }
        else if (r == 0)
        {
            fprintf(stderr, "select timeout\n");
            return 1;
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        // Take this buffer away from the device's write queue
        if (uvc_do_ioctl(dev_fd, VIDIOC_DQBUF, &buf) == -1)
        {
            int errcode = errno;
            if (errcode == EAGAIN)
                continue;

            if (errcode == EIO)
            {
                fprintf(stderr, "EIO in ioctl VIDIOC_DQBUF\n");
            }
            else
            {
                fprintf(stderr, "error in ioctl VIDIOC_DQBUF: %s %d\n", strerror(errcode), errcode);
                return 1;
            }
        }

        assert(buf.index < n_buffers);
        unsigned char* source = (unsigned char*)buffers[buf.index].start;

        parse_uvc_image_params params;

        int work_start_y = 0;
        unsigned char* dest_write_color = color_dest;
        unsigned char* source_read = source;

        params.dst_rgb = dest_write_color;
        params.dst_rgb_origin = color_dest;
        params.src = source_read;
        params.src_origin = source;
        params.start_y = work_start_y;
        params.end_y = vmode->height;
        params.width = vmode->width;

        switch (vmode->pixel_format)
        {
        case UVC_PIXELFORMAT_YUV422:
            uvc_convertYUV422(&params);
            break;
        case UVC_PIXELFORMAT_Y8I:
            uvc_convertY8I(&params);
            break;
        default:
            fprintf(stderr, "Cannot decompress data: Unknown pixel format: %d\n", vmode->pixel_format);
            return 1;
        }

        // Tell the device it can again write data in this buffer
        if (uvc_do_ioctl(dev_fd ,VIDIOC_QBUF, &buf) == -1)
        {
            int errcode = errno;
            fprintf(stderr, "ioctl VIDIOC_QBUF failed: %s %d\n", strerror(errcode), errcode);
            return 1;
        }

        break;
    }

    return 0;
}

