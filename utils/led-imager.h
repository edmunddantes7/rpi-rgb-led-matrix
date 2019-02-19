#include "led-matrix.h"
#include "pixel-mapper.h"
#include "content-streamer.h"
#include "mqtt/async_client.h"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <Magick++.h>
#include <magick/image.h>

using rgb_matrix::GPIO;
using rgb_matrix::Canvas;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::StreamReader;

// global variables
const std::string SERVER_ADDRESS("tcp://iot.eclipse.org");
const std::string CLIENT_ID("darkNinja");
const std::string TOPIC("cthulhu");
const int QOS = 1;
const int N_RETRY_ATTEMPTS = 5;

typedef int64_t tmillis_t;
static const tmillis_t distant_future = (1LL<<40); // that is a while.

struct ImageParams {
  ImageParams() : anim_duration_ms(distant_future), wait_ms(1500),
                  anim_delay_ms(-1), loops(-1) {}
  tmillis_t anim_duration_ms;  // If this is an animation, duration to show.
  tmillis_t wait_ms;           // Regular image: duration to show.
  tmillis_t anim_delay_ms;     // Animation delay override.
  int loops;
};

struct FileInfo {
  ImageParams params;      // Each file might have specific timing settings
  bool is_multi_frame;
  rgb_matrix::StreamIO *content_stream;
};

// functions
static tmillis_t GetTimeInMillis();
static void InterruptHandler(int signo);
static void SleepMillis(tmillis_t milli_seconds); 
static void StoreInStream(const Magick::Image &img, int delay_time_us,
                          bool do_center,
                          rgb_matrix::FrameCanvas *scratch,
                          rgb_matrix::StreamWriter *output); 
static void CopyStream(rgb_matrix::StreamReader *r,
                       rgb_matrix::StreamWriter *w,
                       rgb_matrix::FrameCanvas *scratch);
static bool LoadImageAndScale(const char *filename,
                              int target_width, int target_height,
                              bool fill_width, bool fill_height,
                              std::vector<Magick::Image> *result,
                              std::string *err_msg);
void DisplayAnimation(const FileInfo *file,
                      RGBMatrix *matrix, FrameCanvas *offscreen_canvas,
                      int vsync_multiple);

static int usage(const char *progname);
void setImageParamsFromArgv(int argc, char* argv[]);
void setDefaultFilenameParams();
void setFilenameParamsFromImageParams(); 
void prepareImage(std::string fname);
void attachWaitTimeOnImage();
void displayImage();




