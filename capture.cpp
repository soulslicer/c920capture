//#define DEBUG
#define MB(x) (x*1024*1024)
#include "c920capture.h"


//Callback for process frame
int process_frame(void* data, size_t length, c920_parameters_t c920_parameters)
{
    static long bytes = 0;
    static long fcount = 0;

    //Save file
    FILE* fp = (FILE*) c920_parameters.pipe;
    fwrite(data, 1, length, fp);
    fflush(fp);

    //Increment Values
    bytes+=length;
    fcount++;
    return fcount < c920_parameters.frames ? 1 : 0;
    //return bytes < MB(5) ? 1 : 0;
}

int main(int argc, char **argv)
{
    try
    {
        //Set params
        c920_parameters_t params;
        params.cb=process_frame;
        setParametersFromArgs(params,argc,argv);

        //Set up camera and start it
        c920_device_t* camera = new c920_device_t(params);

        //Start, capture and stop
        camera->start();
        while(camera->process());
        camera->stop();

        //Delete camera
        delete camera;
    }
    catch (c920_exception_t &e)
    {
        printf("%s", e.message());
        if (e.error())
        {
            printf(" (%d: %s)", e.error(), strerror(e.error()));
        }
        printf("\n");
    }

    return 0;
}
