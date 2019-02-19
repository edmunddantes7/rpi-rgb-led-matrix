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

using rgb_matrix::Canvas;
using rgb_matrix::FrameCanvas;
using rgb_matrix::GPIO;
using rgb_matrix::RGBMatrix;
using rgb_matrix::StreamReader;

// global variables
const std::string SERVER_ADDRESS("tcp://iot.eclipse.org");
const std::string CLIENT_ID("darkNinja");
const std::string TOPIC("cthulhu");
const int QOS = 1;
const int N_RETRY_ATTEMPTS = 5;

typedef int64_t tmillis_t;
static const tmillis_t distant_future = (1LL << 40); // that is a while.

struct ImageParams
{
	// TODO: change wait_ms back to 1500.
	ImageParams() : anim_duration_ms(distant_future), wait_ms(distant_future),
									anim_delay_ms(-1), loops(-1) {}
	tmillis_t anim_duration_ms; // If this is an animation, duration to show.
	tmillis_t wait_ms;					// Regular image: duration to show.
	tmillis_t anim_delay_ms;		// Animation delay override.
	int loops;
};

struct FileInfo
{
	ImageParams params; // Each file might have specific timing settings
	bool is_multi_frame;
	rgb_matrix::StreamIO *content_stream;
};

static void InterruptHandler(int signo);
static std::vector<Magick::Image> LoadImageAndScale(
	std::string filename, int target_width, int target_height );
static void StoreInStream(const Magick::Image &img, int delay_time_us,
                          bool do_center,
                          rgb_matrix::FrameCanvas *scratch,
                          rgb_matrix::StreamWriter *output);
static void loadFileInfoFromFile();

static struct State
{
	RGBMatrix::Options matrix_options;
	rgb_matrix::RuntimeOptions runtime_opt;
	ImageParams img_param;

	const char *stream_output = NULL;
	rgb_matrix::StreamIO *stream_io = NULL;
	rgb_matrix::StreamWriter *global_stream_writer = NULL;

	RGBMatrix *matrix = NULL;
	FrameCanvas *offscreen_canvas = NULL;

	FileInfo *file_img;
	std::string imageFilename = "../img/1.png";
	volatile bool interrupt_received = false;
	volatile bool mqtt_message_received = false;

	// extra, do not use
	int vsync_multiple = 1;
	bool do_forever = false;
	bool do_center = false;
	bool do_shuffle = false;
	bool fill_width = false;
	bool fill_height = false;
} state;


int main(int argc, char *argv[])
{
	// TODO: move to the right place.
	signal(SIGTERM, InterruptHandler);
	signal(SIGINT, InterruptHandler);
	Magick::InitializeMagick(*argv);

	// options inited from command line args
	state.matrix_options.chain_length = 24;
	state.runtime_opt.do_gpio_init = true;
	
	state.matrix = CreateMatrixFromOptions(state.matrix_options, state.runtime_opt);	
	state.offscreen_canvas = state.matrix->CreateFrameCanvas();

	printf("Size: %dx%d. Hardware gpio mapping: %s\n",
				 state.matrix->width(), state.matrix->height(), state.matrix_options.hardware_mapping);
	
	////////////////////////////////////////////////////////////////////////////////////////
	std::vector<Magick::Image> image_sequence = 
		LoadImageAndScale(state.imageFilename, state.matrix->width(), state.matrix->height());	
	FileInfo file_info;
	if (image_sequence.size() > 0) {
		file_info.params = state.img_param;
		file_info.content_stream = rgb_matrix::MemStreamIO();
		file_info.is_multi_frame = image_sequence.size() > 1;
		rgb_matrix::StreamWriter out(&file_info.content_stream);

		for (size_t i = 0; i < image_sequence.size(); ++i) {
			const Magick::Image &img = image_sequence[i];
			int64_t delay_time_us;

			if (file_info.is_multi_frame) {
				delay_time_us = img.animationDelay() * 10000; // unit in 1/100s
			} else {
				delay_time_us = file_info->params.wait_ms * 1000;  // single image.
			}
			if (delay_time_us <= 0) delay_time_us = 100 * 1000;  // 1/10sec
			StoreInStream(img, delay_time_us, do_center, offscreen_canvas, &out);
		}
	}
	////////////////////////////////////////////////////////////////////////////////////////


	// rgb_matrix::StreamReader reader(file->content_stream);

	// TODO: fill offscreen_canvas with the image.
	// offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas, 1);
}

static void loadFileInfoFromFile()
{	
	std::string err_msg;
	std::vector<Magick::Image> image_sequence;
	
	state.file_img = new FileInfo();
	state.file_img->params = state.img_param;
	state.file_img->content_stream = new rgb_matrix::MemStreamIO();
	state.file_img->is_multi_frame = image_sequence.size() > 1;
	rgb_matrix::StreamWriter out(state.file_img->content_stream);

	for (size_t i = 0; i < image_sequence.size(); ++i) {
		const Magick::Image &img = image_sequence[i];
		int64_t delay_time_us;

		if (state.file_img->is_multi_frame)
			delay_time_us = img.animationDelay() * 10000; // unit in 1/100s
		else
			delay_time_us = state.file_img->params.wait_ms * 1000;  // single image.

		if (delay_time_us <= 0) delay_time_us = 100 * 1000;  // 1/10sec

		StoreInStream(img, delay_time_us, state.do_center, state.offscreen_canvas, &out);			
	}
	return;
}

// be aware this stores information in stream instead of just giving you the necessary information.
static void StoreInStream(const Magick::Image &img, int delay_time_us,
                          bool do_center,
                          rgb_matrix::FrameCanvas *scratch,
                          rgb_matrix::StreamWriter *output) {
  scratch->Clear();
  const int x_offset = do_center ? (scratch->width() - img.columns()) / 2 : 0;
  const int y_offset = do_center ? (scratch->height() - img.rows()) / 2 : 0;
  for (size_t y = 0; y < img.rows(); ++y) {
    for (size_t x = 0; x < img.columns(); ++x) {
      const Magick::Color &c = img.pixelColor(x, y);
      if (c.alphaQuantum() < 256) {
        scratch->SetPixel(x + x_offset, y + y_offset,
                          ScaleQuantumToChar(c.redQuantum()),
                          ScaleQuantumToChar(c.greenQuantum()),
                          ScaleQuantumToChar(c.blueQuantum()));
      }
    }
  }
  output->Stream(*scratch, delay_time_us);
}

// Load still image or animation.
// Scale, so that it fits in "width" and "height" and store in "result".
static std::vector<Magick::Image> LoadImageAndScale(std::string filename, int target_width, int target_height)
{
	std::vector<Magick::Image> frames;
	std::vector<Magick::Image> result;

	try {
		readImages(&frames, filename);
	} catch (std::exception &e) {
		if (e.what()) fprintf(stderr, "Error reading images: %s!\n", e.what());
		exit(1);		
	}

	if (frames.size() == 0) {
		fprintf(stderr, "No image found.");
		exit(0);
	}

	// Put together the animation from single frames. GIFs can have nasty
	// disposal modes, but they are handled nicely by coalesceImages()
	if (frames.size() > 1) {
		Magick::coalesceImages(&result, frames.begin(), frames.end());
	} else {
		result.push_back(frames[0]); // just a single still image.
	}

	for (size_t i = 0; i < result.size(); ++i) {
		result[i].scale(Magick::Geometry(target_width, target_height));
	}

	return result;	
}

static void InterruptHandler(int signo)
{
	state.interrupt_received = true;
	return;
}