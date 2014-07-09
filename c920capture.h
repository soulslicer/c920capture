#ifndef C920_CAPTURE_H
#define C920_CAPTURE_H

//Included libraries
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <iostream>

#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <syslog.h>

#include <linux/videodev2.h>
#include <linux/uvcvideo.h>

#include "uvch264.h"

//Define V4L2 Pixel format
#ifndef V4L2_PIX_FMT_H264
#define V4L2_PIX_FMT_H264 v4l2_fourcc('H', '2', '6', '4')
#endif
#define CLEAR(x) memset(&(x), 0, sizeof(x))

//Format Types
const int YUYV = 0;
const int MJPEG = 1;
const int H264 = 2;

//Define Debug messages
//#ifdef DEBUG
#define DEBUG(fmt, ...) fprintf(stderr,fmt "\n", ## __VA_ARGS__)
//#else
//#define DEBUG(fmt, ...) {}
//#endif

//Exception class
class c920_exception_t
{
    private: int _errno;
    private: char _message[1024];

    public: c920_exception_t(const char* fmt, ...)
    {
        _errno = errno;

        va_list args;
        va_start(args, fmt);
        vsprintf(_message, fmt, args);
        va_end(args);
        syslog(LOG_DEBUG, fmt);
    }

    public: const char* message() const { return _message; }
    public: int error() const { return _errno; }
};

//Parameters and callback object
struct c920_parameters_t
{
    typedef int (*c920_buffer_cb)(void* data, size_t length, c920_parameters_t c920_parameters);
    public: const char* device_name;
    public: const char* directory;
    public: size_t width;
    public: size_t height;
    public: size_t fps;
    public: int frames;
    public: int format;
    public: c920_buffer_cb cb;
    public: void* pipe;
    public: int bitrate;
};

//Capture class
class c920_device_t
{
    private: bool   _playing;
    private: char*  _device_name;
    private: int    _fd;
    private: size_t _num_buffers;
    private: struct _buffer { void* data; size_t length; };
    private: _buffer* _buffers;
    private: c920_parameters_t _c920_parameters;

    //Constructor
    public: c920_device_t(c920_parameters_t c920_parameters)
    {
        struct stat st;
        v4l2_capability cap;
        v4l2_cropcap cropcap;
        v4l2_crop crop;
        v4l2_format fmt;
        v4l2_requestbuffers req;
        v4l2_streamparm parm;

        _device_name = 0;
        _playing = false;
        _c920_parameters = c920_parameters;

        memset(&cropcap, 0, sizeof(cropcap));
        memset(&crop, 0, sizeof(crop));
        memset(&fmt, 0, sizeof(fmt));
        memset(&req, 0, sizeof(req));
        memset(&parm, 0, sizeof(parm));

        /*****************************************************
        Get file status, check /dev/video*
        ******************************************************/
        DEBUG("Identifying device %s", c920_parameters.device_name);
        if (stat(c920_parameters.device_name, &st) == -1)
            throw c920_exception_t("unable to identify device %s", c920_parameters.device_name);

        /*****************************************************
        Check if this is a device
        ******************************************************/
        DEBUG("Testing to see if %s is a device", c920_parameters.device_name);
        if (!S_ISCHR(st.st_mode))
            throw c920_exception_t("%s is not a device", c920_parameters.device_name);\

        /*****************************************************
        Open device
        ******************************************************/
        DEBUG("Opening device %s as RDWR | NONBLOCK",c920_parameters.device_name);
        if ((_fd = open(c920_parameters.device_name, O_RDWR | O_NONBLOCK, 0)) == -1)
            throw c920_exception_t("cannot open device %s", c920_parameters.device_name);

        /*****************************************************
        Check if device is V4L2 capable
        ******************************************************/
        DEBUG("Querying V4L2 capabilities for device %s", c920_parameters.device_name);
        if (ioctl_ex(_fd, VIDIOC_QUERYCAP, &cap) == -1)
        {
            if (errno == EINVAL) throw c920_exception_t("%s is not a valid V4L2 device", c920_parameters.device_name);
            else throw c920_exception_t("error in ioctl VIDIOC_QUERYCAP");
        }

        /*****************************************************
        Check if it is a streaming capture device
        ******************************************************/
        DEBUG("Testing if device %s is a streaming capture device", c920_parameters.device_name);
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
            throw c920_exception_t("%s is not a capture device", c920_parameters.device_name);
        if (!(cap.capabilities & V4L2_CAP_STREAMING))
            throw c920_exception_t("%s is not a streaming device", c920_parameters.device_name);

        /*****************************************************
        Set crop rectangle
        ******************************************************/
        DEBUG("Trying to set crop rectange for device %s", c920_parameters.device_name);
        cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl_ex(_fd, VIDIOC_CROPCAP, &cropcap) == 0)
        {
            crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            crop.c = cropcap.defrect;
            if (ioctl_ex(_fd, VIDIOC_S_CROP, &crop) == -1)
                DEBUG("W: Unable to set crop for device %s", c920_parameters.device_name);
        }
        else DEBUG("W: Unable to get crop capabilities for device %s", c920_parameters.device_name);

        /*****************************************************
        Setting video format to H264
        ******************************************************/
        DEBUG("Setting video format to H.264 (w:%d, h:%d) for device %s", c920_parameters.width, c920_parameters.height, c920_parameters.device_name);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = c920_parameters.width;
        fmt.fmt.pix.height = c920_parameters.height;
        if(c920_parameters.format==MJPEG) fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        else if(c920_parameters.format==YUYV) fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        else if(c920_parameters.format==H264) fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
        else throw c920_exception_t("invalid format specified");
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
        if (ioctl_ex(_fd, VIDIOC_S_FMT, &fmt) == -1)
            throw c920_exception_t("error in ioctl VIDIOC_S_FMT");

        /*****************************************************
        Get streaming parameters
        ******************************************************/
        DEBUG("Getting video stream parameters for device %s", c920_parameters.device_name);
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl_ex(_fd, VIDIOC_G_PARM, &parm) == -1)
            throw c920_exception_t("unable to get stream parameters for %s", c920_parameters.device_name);

        /*****************************************************
        Set frame rate
        ******************************************************/
        DEBUG("Time per frame was: %d/%d", parm.parm.capture.timeperframe.numerator, parm.parm.capture.timeperframe.denominator);
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = c920_parameters.fps;
        DEBUG("Time per frame set: %d/%d", parm.parm.capture.timeperframe.numerator, parm.parm.capture.timeperframe.denominator);
        if (ioctl_ex(_fd, VIDIOC_S_PARM, &parm) == -1)
            throw c920_exception_t("unable to set stream parameters for %s", c920_parameters.device_name);
        DEBUG("Time per frame now: %d/%d", parm.parm.capture.timeperframe.numerator, parm.parm.capture.timeperframe.denominator);

        /*****************************************************
        Initialize MMAP (http://linuxtv.org/downloads/v4l-dvb-apis/mmap.html)
        ******************************************************/
        DEBUG("Initializing MMAP for device %s", c920_parameters.device_name);
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl_ex(_fd,VIDIOC_REQBUFS, &req) == -1)
        {
            if (errno == EINVAL) throw c920_exception_t("%s does not support MMAP", c920_parameters.device_name);
            else throw c920_exception_t("error in ioctl VIDIOC_REQBUFS");
        }
        DEBUG("Device %s can handle %d memory mapped buffers", c920_parameters.device_name, req.count);
        if (req.count < 2) throw c920_exception_t("insufficient memory on device %s", c920_parameters.device_name);

        /*****************************************************
        Allocate buffers to map
        ******************************************************/
        DEBUG("Allocating %d buffers to map", req.count);
        _buffers = (_buffer*) calloc(req.count, sizeof(_buffer));
        if (!_buffers) throw c920_exception_t("out of memory");

        for (_num_buffers = 0; _num_buffers < req.count; _num_buffers++)
        {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = _num_buffers;

            if (ioctl_ex(_fd, VIDIOC_QUERYBUF, &buf) == -1)
                throw c920_exception_t("error in ioctl VIDIOC_QUERYBUF");

            DEBUG("Mapping buffer %d", _num_buffers);
            _buffers[_num_buffers].length = buf.length;
            _buffers[_num_buffers].data = mmap(
                NULL,
                buf.length,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                _fd, buf.m.offset);

            if (_buffers[_num_buffers].data == MAP_FAILED)
                throw c920_exception_t("mmap failed");
        }

        /*****************************************************
        Queue buffers for device
        ******************************************************/
        DEBUG("Queueing %d buffers for device %s", _num_buffers, c920_parameters.device_name);
        for (size_t i=0; i<_num_buffers; i++)
        {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            DEBUG("Queueing buffer %d", i);
            if (ioctl_ex(_fd, VIDIOC_QBUF, &buf) == -1)
                throw c920_exception_t("error in ioctl VIDIOC_QBUF");
        }

        /*****************************************************
        Copy the device name so we can use it in error messages and set callback
        ******************************************************/
        _device_name = (char*) malloc(strlen(c920_parameters.device_name)+1);
        strcpy(_device_name, c920_parameters.device_name);
        DEBUG("Done with setup of device %s", c920_parameters.device_name);


    }

    //Destructor
    public: ~c920_device_t()
    {
        /*****************************************************
        Stop device playback
        ******************************************************/
        if (_playing) stop();

        /*****************************************************
        Destroy all buffers
        ******************************************************/
        DEBUG("Destroying memory mapped buffers for device %s", _device_name);
        for (size_t i=0; i<_num_buffers; i++)
        {
            DEBUG("Unmapping buffer %d", i);
            if (munmap(_buffers[i].data, _buffers[i].length) == -1)
                throw c920_exception_t("Unable to unmap buffer %d", i);
        }

        /*****************************************************
        Closing devices
        ******************************************************/
        DEBUG("Closing device %s", _device_name);
        if (close(_fd) == -1)
            throw c920_exception_t("Unable to close device %s", _device_name);
        if (_device_name) free(_device_name);
        fclose((FILE*)_c920_parameters.pipe);
    }

    //Stop the capture device
    public: void stop()
    {
        if (!_playing) return;
        _playing = false;

        DEBUG("Stopping device %s", _device_name);
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl_ex(_fd, VIDIOC_STREAMOFF, &type) == -1)
                throw c920_exception_t("error in ioctl VIDIOC_STREAMOFF");

        /*****************************************************
        Queue buffers if camera is stopped
        ******************************************************/
        DEBUG("Queueing %d buffers for device %s", _num_buffers, _c920_parameters.device_name);
        for (size_t i=0; i<_num_buffers; i++)
        {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            DEBUG("Queueing buffer %d", i);
            if (ioctl_ex(_fd, VIDIOC_QBUF, &buf) == -1)
                throw c920_exception_t("error in ioctl VIDIOC_QBUF");
       }

    }

    //Start the capture device
    public: void start()
    {
        if (_playing) return;
        _playing = true;

        DEBUG("Starting device %s", _device_name);
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl_ex(_fd, VIDIOC_STREAMON, &type) == -1)
            throw c920_exception_t("error in ioctl VIDIOC_STREAMON");

        set_bitrate(_c920_parameters.bitrate);
    }

    //Process a single frame from the capture stream, call this in a loop
    public: int process()
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(_fd, &fds);

        timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        //Select the device
        switch (select(_fd+1, &fds, NULL, NULL, &tv))
        {
            case -1:
            {
                if (errno == EINTR) return 1;
                else throw c920_exception_t("Could not select device %s", _device_name);
            }
            case 0:
            {
                throw c920_exception_t("timeout occurred while selecting device %s", _device_name);
            }
        }

        //Dequeue a buffer
        v4l2_buffer buffer = {0};
        CLEAR(buffer);
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;

        if (ioctl_ex(_fd, VIDIOC_DQBUF, &buffer) == -1)
        {
            if (errno == EAGAIN){
                DEBUG("errno == EAGAIN %s",_device_name);
                return 1;
            }
            else throw c920_exception_t("error in ioctl VIDIOC_DQBUF");
        }

        assert(buffer.index < _num_buffers);

        int r = 0;
        if (_c920_parameters.cb) r = _c920_parameters.cb(_buffers[buffer.index].data, buffer.bytesused, _c920_parameters);

        //Queue the buffer again
        if (ioctl_ex(_fd, VIDIOC_QBUF, &buffer) == -1)
            throw c920_exception_t("error in ioctl VIDIOC_QBUF");

        return r;
    }

    //Keep comm with device until done (http://man7.org/linux/man-pages/man2/ioctl.2.html)
    private: static int ioctl_ex(int fh, int request, void* arg)
    {
        int r;
        do { r = ioctl(fh, request, arg); } while (r == -1 && EINTR == errno);
        return r;
    }



    private: void set_bitrate(int bmin)
    {
        int bmax = bmin;
        int res;
        struct uvc_xu_control_query ctrl;
        uvcx_bitrate_layers_t  conf;
        ctrl.unit = 12;
        ctrl.size = 10;
        ctrl.selector = UVCX_BITRATE_LAYERS;
        ctrl.data = (unsigned char*)&conf;
        ctrl.query = UVC_GET_CUR;
        conf.wLayerID = 0;
        conf.dwPeakBitrate = conf.dwAverageBitrate = 0;
        res = ioctl_ex(_fd, UVCIOC_CTRL_QUERY, &ctrl);
        if (res)
        {
          DEBUG("ctrl_query error");
          return;
          //throw c920_exception_t("error in ioctl ctrl_query");
        }
        fprintf(stderr, "get before br %d %d\n", conf.dwPeakBitrate, conf.dwAverageBitrate);
        conf.dwPeakBitrate = bmax;
        conf.dwAverageBitrate = bmin;
        ctrl.query = UVC_SET_CUR;
        res = ioctl_ex(_fd, UVCIOC_CTRL_QUERY, &ctrl);
        if (res)
        {
          DEBUG("ctrl_query error");
          return;
          //throw c920_exception_t("error in ioctl ctrl_query");
        }
        fprintf(stderr, "set br %d %d\n", conf.dwPeakBitrate, conf.dwAverageBitrate);
        ctrl.query = UVC_GET_CUR;
        res = ioctl_ex(_fd, UVCIOC_CTRL_QUERY, &ctrl);
        if (res)
        {
          DEBUG("ctrl_query error");
          return;
          //throw c920_exception_t("error in ioctl ctrl_query");
        }
    }

};

//Specify options list
//./capture -W 1280 -H 720 -f IMAGE -d /dev/video0 -c 1 -p 1 -o stdout
//./capture -W 1280 -H 720 -f VIDEO -d /dev/video0 -c 300 -p 30 -b 500000 -o stdout
static const char short_options[] = "d:hmruW:H:I:f:t:T:p:c:o:l:b:";
static const struct option
  long_options[] = {
    { "device",        required_argument, NULL, 'd'},
    { "help",          no_argument,       NULL, 'h'},
    { "mmap",          no_argument,       NULL, 'm'},
    { "read",          no_argument,       NULL, 'r'},
    { "userp",         no_argument,       NULL, 'u'},
    { "width",         required_argument, NULL, 'W'},
    { "height",        required_argument, NULL, 'H'},
    { "interval",      required_argument, NULL, 'I'},
    { "format",        required_argument, NULL, 'f'},
    { "timeout",       required_argument, NULL, 't'},
    { "timeouts-max",  required_argument, NULL, 'T'},
    { "period",        required_argument, NULL, 'p'},
    { "count",         required_argument, NULL, 'c'},
    { "output",        required_argument, NULL, 'o'},
    { "directory",     required_argument, NULL, 'l'},
    { "bitrate",       required_argument, NULL, 'b'},
    { 0, 0, 0, 0}
};
void setParametersFromArgs(c920_parameters_t& params, int argc, char **argv){
    int idx, c;
    for(;;){
        c = getopt_long(argc, argv,short_options, long_options, &idx);
        if (-1 == c) break;
        switch(c){
            case 'W': //Width (Width of frame)
                params.width = atoi(optarg);
                break;
            case 'H': //Height (Height of frame)
                params.height = atoi(optarg);
                break;
            case 'f': //Format (Video or image)
                if(strcmp("YUYV",optarg)==0) params.format=YUYV;
                if(strcmp("MJPEG",optarg)==0) params.format=MJPEG;
                if(strcmp("H264",optarg)==0) params.format=H264;
                break;
            case 'd': //Device (Device selected)
                params.device_name = optarg;
                break;
            case 'c': //Count (Number of frames)
                params.frames = atoi(optarg);
                break;
            case 'p': //Interval (Framerate or interval)
                params.fps = atoi(optarg);
                break;
            case 'o': //Output (Output to location)
                if(strcmp("stdout",optarg)==0) params.pipe=stdout;
                else{
                    FILE* fp = fopen(optarg, "wb");
                    if (!fp){
                        fprintf(stderr, "Unable to open file for writing: %s", optarg);
                        exit(EXIT_FAILURE);
                    }
                    params.pipe=fp;
                }
                break;
            case 'l': //Directory (Directory)
                params.directory = optarg;
                break;
            case 'b':
                params.bitrate = atoi(optarg);
        }
    }
}

//We could use command line parser but that requires BOOST libraries. I'm lazy to install that

#endif














