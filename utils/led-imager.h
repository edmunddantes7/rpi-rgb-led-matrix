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
                      int vsync_multiple, std::string);

static int usage(const char *progname);
void setImageParamsFromArgv(int argc, char* argv[]);
void setDefaultFilenameParams();
void setFilenameParamsFromImageParams(); 
void prepareImage(std::string);
void attachWaitTimeOnImage();
void displayImage(std::string);
void updateImage();

// all globals.
static struct State {
	tmillis_t start_load = GetTimeInMillis();

	int vsync_multiple = 1;
	bool do_forever = false;
	bool do_center = false;
	bool do_shuffle = false;

	RGBMatrix::Options matrix_options;
	rgb_matrix::RuntimeOptions runtime_opt;
	ImageParams img_param;
	std::map<const void *, struct ImageParams> filename_params;

	const char *stream_output = NULL;

	int argc = 0;
	char **argv;
	
	bool fill_width = false;
	bool fill_height = false;

	rgb_matrix::StreamIO *stream_io = NULL;
	rgb_matrix::StreamWriter *global_stream_writer = NULL;

	RGBMatrix *matrix = NULL;
	FrameCanvas *offscreen_canvas = NULL;
  	std::vector<FileInfo*> file_imgs;
	std::string imageFilename = "../img/1.png";
	volatile bool interrupt_received = false;
	volatile bool mqtt_message_received = false;
} state;



