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

// functions 
static void InterruptHandler(int signo);

// MQTT 
// globals
const std::string SERVER_ADDRESS("tcp://iot.eclipse.org");
const std::string CLIENT_ID("darkNinja");
const std::string TOPIC("cthulhu");
const int QOS = 1;
const int N_RETRY_ATTEMPTS = 5;


// ================================================ CLASSES ==============================================

class action_listener : public virtual mqtt::iaction_listener
{
	std::string name_;

	void on_failure(const mqtt::token& tok) override {
		std::cout << name_ << " failure";
		if (tok.get_message_id() != 0)
			std::cout << " for token: [" << tok.get_message_id() << "]" << std::endl;
		std::cout << std::endl;
	}

	void on_success(const mqtt::token& tok) override {
		std::cout << name_ << " success";
		if (tok.get_message_id() != 0)
			std::cout << " for token: [" << tok.get_message_id() << "]" << std::endl;
		auto top = tok.get_topics();
		if (top && !top->empty())
			std::cout << "\ttoken topic: '" << (*top)[0] << "', ..." << std::endl;
		std::cout << std::endl;
	}

public:
	action_listener(const std::string& name) : name_(name) {}
};


/**
 * Local callback & listener class for use with the client connection.
 * This is primarily intended to receive messages, but it will also monitor
 * the connection to the broker. If the connection is lost, it will attempt
 * to restore the connection and re-subscribe to the topic.
 */
class callback : public virtual mqtt::callback,
					public virtual mqtt::iaction_listener

{
	// Counter for the number of connection retries
	int nretry_;
	// The MQTT client
	mqtt::async_client& cli_;
	// Options to use if we need to reconnect
	mqtt::connect_options& connOpts_;
	// An action listener to display the result of actions.
	action_listener subListener_;

	// This deomonstrates manually reconnecting to the broker by calling
	// connect() again. This is a possibility for an application that keeps
	// a copy of it's original connect_options, or if the app wants to
	// reconnect with different options.
	// Another way this can be done manually, if using the same options, is
	// to just call the async_client::reconnect() method.
	void reconnect() {
		std::this_thread::sleep_for(std::chrono::milliseconds(2500));
		try {
			cli_.connect(connOpts_, nullptr, *this);
		}
		catch (const mqtt::exception& exc) {
			std::cerr << "Error: " << exc.what() << std::endl;
			exit(1);
		}
	}

	// Re-connection failure
	void on_failure(const mqtt::token& tok) override {
		std::cout << "Connection attempt failed" << std::endl;
		if (++nretry_ > N_RETRY_ATTEMPTS)
			exit(1);
		reconnect();
	}

	// (Re)connection success
	// Either this or connected() can be used for callbacks.
	void on_success(const mqtt::token& tok) override {}

	// (Re)connection success
	void connected(const std::string& cause) override {
		std::cout << "\nConnection success" << std::endl;
		std::cout << "\nSubscribing to topic '" << TOPIC << "'\n"
			<< "\tfor client " << CLIENT_ID
			<< " using QoS" << QOS << "\n"
			<< "\nPress Q<Enter> to quit\n" << std::endl;

		cli_.subscribe(TOPIC, QOS, nullptr, subListener_);
	}

	// Callback for when the connection is lost.
	// This will initiate the attempt to manually reconnect.
	void connection_lost(const std::string& cause) override {
		std::cout << "\nConnection lost" << std::endl;
		if (!cause.empty())
			std::cout << "\tcause: " << cause << std::endl;

		std::cout << "Reconnecting..." << std::endl;
		nretry_ = 0;
		reconnect();
	}

	// Callback for when a message arrives.
	void message_arrived(mqtt::const_message_ptr msg) override {
		std::cout << "Message arrived" << std::endl;
		std::cout << "\ttopic: '" << msg->get_topic() << "'" << std::endl;
		std::cout << "\tpayload: '" << msg->to_string() << "'\n" << std::endl;
		// change the data inside myfilename
		//myfilename = msg->to_string();
		//processImage(numberOfArgs, arguments);
	}

	void delivery_complete(mqtt::delivery_token_ptr token) override {}

public:
	callback(mqtt::async_client& cli, mqtt::connect_options& connOpts)
				: nretry_(0), cli_(cli), connOpts_(connOpts), subListener_("Subscription") {}
};


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

// ================================================ /CLASSES ==============================================

volatile bool interrupt_received = false;

static tmillis_t GetTimeInMillis() {
  struct timeval tp;
  gettimeofday(&tp, NULL);
  return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

static void SleepMillis(tmillis_t milli_seconds) {
  if (milli_seconds <= 0) return;
  struct timespec ts;
  ts.tv_sec = milli_seconds / 1000;
  ts.tv_nsec = (milli_seconds % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

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

static void CopyStream(rgb_matrix::StreamReader *r,
                       rgb_matrix::StreamWriter *w,
                       rgb_matrix::FrameCanvas *scratch) {
  uint32_t delay_us;
  while (r->GetNext(scratch, &delay_us)) {
    w->Stream(*scratch, delay_us);
  }
}

// Load still image or animation.
// Scale, so that it fits in "width" and "height" and store in "result".
static bool LoadImageAndScale(const char *filename,
                              int target_width, int target_height,
                              bool fill_width, bool fill_height,
                              std::vector<Magick::Image> *result,
                              std::string *err_msg) {
  std::vector<Magick::Image> frames;
  try {
    readImages(&frames, filename);
  } catch (std::exception& e) {
    if (e.what()) *err_msg = e.what();
    return false;
  }
  if (frames.size() == 0) {
    fprintf(stderr, "No image found.");
    return false;
  }

  // Put together the animation from single frames. GIFs can have nasty
  // disposal modes, but they are handled nicely by coalesceImages()
  if (frames.size() > 1) {
    Magick::coalesceImages(result, frames.begin(), frames.end());
  } else {
    result->push_back(frames[0]);   // just a single still image.
  }

  const int img_width = (*result)[0].columns();
  const int img_height = (*result)[0].rows();
  const float width_fraction = (float)target_width / img_width;
  const float height_fraction = (float)target_height / img_height;
  if (fill_width && fill_height) {
    // Scrolling diagonally. Fill as much as we can get in available space.
    // Largest scale fraction determines that.
    const float larger_fraction = (width_fraction > height_fraction)
      ? width_fraction
      : height_fraction;
    target_width = (int) roundf(larger_fraction * img_width);
    target_height = (int) roundf(larger_fraction * img_height);
  }
  else if (fill_height) {
    // Horizontal scrolling: Make things fit in vertical space.
    // While the height constraint stays the same, we can expand to full
    // width as we scroll along that axis.
    target_width = (int) roundf(height_fraction * img_width);
  }
  else if (fill_width) {
    // dito, vertical. Make things fit in horizontal space.
    target_height = (int) roundf(width_fraction * img_height);
  }

  for (size_t i = 0; i < result->size(); ++i) {
    (*result)[i].scale(Magick::Geometry(target_width, target_height));
  }

  return true;
}

void DisplayAnimation(const FileInfo *file,
                      RGBMatrix *matrix, FrameCanvas *offscreen_canvas,
                      int vsync_multiple) {
  const tmillis_t duration_ms = (file->is_multi_frame
                                 ? file->params.anim_duration_ms
                                 : file->params.wait_ms);
  rgb_matrix::StreamReader reader(file->content_stream);
  int loops = file->params.loops;
  const tmillis_t end_time_ms = GetTimeInMillis() + duration_ms;
  const tmillis_t override_anim_delay = file->params.anim_delay_ms;
  for (int k = 0;
       (loops < 0 || k < loops)
         && !interrupt_received
         && GetTimeInMillis() < end_time_ms;
       ++k) {
    uint32_t delay_us = 0;
    while (!interrupt_received && GetTimeInMillis() <= end_time_ms
           && reader.GetNext(offscreen_canvas, &delay_us)) {
      const tmillis_t anim_delay_ms =
        override_anim_delay >= 0 ? override_anim_delay : delay_us / 1000;
      const tmillis_t start_wait_ms = GetTimeInMillis();
      offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas, vsync_multiple);
      const tmillis_t time_already_spent = GetTimeInMillis() - start_wait_ms;
      SleepMillis(anim_delay_ms - time_already_spent);
    }
    reader.Rewind();
  }
}

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
} state;


static int usage(const char *progname);
void setImageParamsFromArgv(int argc, char* argv[], State state);
void setDefaultFilenameParams(State state);
void setFilenameParamsFromImageParams(State state); 

// globals that cannot be put above everything.

int main(int argc, char* argv[]) 
{
	// copy argc and argv into state
	state.argc = argc;
	state.argv = static_cast<char **>(malloc((argc+1) * sizeof *argv));
    for(int i = 0; i < argc; ++i)
    {
        size_t length = strlen(argv[i])+1;
        state.argv[i] = static_cast<char *>(malloc(length));
        memcpy(state.argv[i], argv[i], length);
    }
    state.argv[argc] = NULL;

	////////////////////////////////////////////////////////////////////////////////////////////////////////
	// MQTT
	// init mqtt
	mqtt::connect_options connOpts;
	connOpts.set_keep_alive_interval(20);
	connOpts.set_clean_session(true);

	mqtt::async_client client(SERVER_ADDRESS, CLIENT_ID);

	callback cb(client, connOpts);
	client.set_callback(cb);

	// Start the connection.
	// When completed, the callback will subscribe to topic.
	try {
		std::cout << "Connecting to the MQTT server..." << std::flush;
		client.connect(connOpts, nullptr, cb);
	}
	catch (const mqtt::exception&) {
		std::cerr << "\nERROR: Unable to connect to MQTT server: '"
			<< SERVER_ADDRESS << "'" << std::endl;
		return 1;
	}

	
	////////////////////////////////////////////////////////////////////////////////////////////////////////
 	Magick::InitializeMagick(*argv);

	if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &state.matrix_options, &state.runtime_opt)) {
		return usage(argv[0]);
	}

	setDefaultFilenameParams(state);
	setImageParamsFromArgv(argc, argv, state); // sets on state.
	setFilenameParamsFromImageParams(state);

	// can't tell if there is a filename, don't forget to supply a filename
	// TODO: do tell?

    state.runtime_opt.do_gpio_init = (state.stream_output == NULL);
  	state.matrix = CreateMatrixFromOptions(state.matrix_options, state.runtime_opt);
	if (state.matrix == NULL) return 1;

	state.offscreen_canvas = state.matrix->CreateFrameCanvas();

  	printf("Size: %dx%d. Hardware gpio mapping: %s\n",
         state.matrix->width(), state.matrix->height(), state.matrix_options.hardware_mapping);

	if (stream_output) {
		int fd = open(stream_output, O_CREAT|O_WRONLY, 0644);
		if (fd < 0) {
		  perror("Couldn't open output stream");
		  return 1;
		}
		stream_io = new rgb_matrix::FileStreamIO(fd);
		global_stream_writer = new rgb_matrix::StreamWriter(stream_io);
	}

	prepareImages();
	////////////////////////////////////////////////////////////////////////////////////////////////////////
	// MQTT
	// Disconnect
	try {
		std::cout << "\nDisconnecting from the MQTT server..." << std::flush;
		client.disconnect()->wait();
		std::cout << "OK" << std::endl;
	}
	catch (const mqtt::exception& exc) {
		std::cerr << exc.what() << std::endl;
		return 1;
	}


	return 0;
}

void prepareImage(std::string fname)
{
	const char *filename = fname.c_str();
	FileInfo *file_info = NULL;

	std::string err_msg;
	std::vector<Magick::Image> image_sequence;
	if (LoadImageAndScale(filename, state.matrix->width(), state.matrix->height(),
						  fill_width, fill_height, &image_sequence, &err_msg)) {
	  file_info = new FileInfo();
	  file_info->params = filename_params[filename];
	  file_info->content_stream = new rgb_matrix::MemStreamIO();
	  file_info->is_multi_frame = image_sequence.size() > 1;
	  rgb_matrix::StreamWriter out(file_info->content_stream);
	  for (size_t i = 0; i < image_sequence.size(); ++i) {
		const Magick::Image &img = image_sequence[i];
		int64_t delay_time_us;
		if (file_info->is_multi_frame) {
		  delay_time_us = img.animationDelay() * 10000; // unit in 1/100s
		} else {
		  delay_time_us = file_info->params.wait_ms * 1000;  // single image.
		}
		if (delay_time_us <= 0) delay_time_us = 100 * 1000;  // 1/10sec
		StoreInStream(img, delay_time_us, state.do_center, state.offscreen_canvas,
					  global_stream_writer ? global_stream_writer : &out);
	  }
	} else {
	  // Ok, not an image. Let's see if it is one of our streams.
	  int fd = open(filename, O_RDONLY);
	  if (fd >= 0) {
		file_info = new FileInfo();
		file_info->params = filename_params[filename];
		file_info->content_stream = new rgb_matrix::FileStreamIO(fd);
		StreamReader reader(file_info->content_stream);
		if (reader.GetNext(offscreen_canvas, NULL)) {  // header+size ok
		  file_info->is_multi_frame = reader.GetNext(offscreen_canvas, NULL);
		  reader.Rewind();
		  if (global_stream_writer) {
			CopyStream(&reader, global_stream_writer, offscreen_canvas);
		  }
		} else {
		  err_msg = "Can't read as image or compatible stream";
		  delete file_info->content_stream;
		  delete file_info;
		  file_info = NULL;
		}
	  }
	}

	return;
}

static void InterruptHandler(int signo) 
{
	interrupt_received = true;
	return;
}

void setDefaultFilenameParams(State state) 
{
	for (int i = 0; i < state.argc; ++i) {
    	state.filename_params[state.argv[i]] = state.img_param;
	}
	return;
}

void setFilenameParamsFromImageParams(State state) {
	 // Starting from the current file, set all the remaining files to
    // the latest change.
    for (int i = optind; i < state.argc; ++i) {
      state.filename_params[state.argv[i]] = state.img_param;
    }
}

// TODO: Wire argv, argc from state
void setImageParamsFromArgv(int argc, char* argv[], State state)
{
	int opt;

	while ((opt = getopt(argc, argv, "w:t:l:fr:c:P:LhCR:sO:V:D:")) != -1) {
		switch (opt) {
			case 'w':
			  state.img_param.wait_ms = roundf(atof(optarg) * 1000.0f);
			  break;
			case 't':
			  state.img_param.anim_duration_ms = roundf(atof(optarg) * 1000.0f);
			  break;
			case 'l':
			  state.img_param.loops = atoi(optarg);
			  break;
			case 'D':
			  state.img_param.anim_delay_ms = atoi(optarg);
			  break;
			case 'f':
			  state.do_forever = true;
			  break;
			case 'C':
			  state.do_center = true;
			  break;
			case 's':
			  state.do_shuffle = true;
			  break;
			case 'r':
			  state.matrix_options.rows = atoi(optarg);
			  break;
			case 'c':
			  state.matrix_options.chain_length = atoi(optarg);
			  break;
			case 'P':
			  state.matrix_options.parallel = atoi(optarg);
			  break;
			case 'O':
			  state.stream_output = strdup(optarg);
			  break;
			case 'V':
			  state.vsync_multiple = atoi(optarg);
			  if (state.vsync_multiple < 1) state.vsync_multiple = 1;
			  break;
			case 'h':
			default:
			  usage(argv[0]);
		}
	}
	return;
}

static int usage(const char *progname) {
  fprintf(stderr, "usage: %s [options] <image> [option] [<image> ...]\n",
          progname);

  fprintf(stderr, "Options:\n"
          "\t-O<streamfile>            : Output to stream-file instead of matrix (Don't need to be root).\n"
          "\t-C                        : Center images.\n"

          "\nThese options affect images following them on the command line:\n"
          "\t-w<seconds>               : Regular image: "
          "Wait time in seconds before next image is shown (default: 1.5).\n"
          "\t-t<seconds>               : "
          "For animations: stop after this time.\n"
          "\t-l<loop-count>            : "
          "For animations: number of loops through a full cycle.\n"
          "\t-D<animation-delay-ms>    : "
          "For animations: override the delay between frames given in the\n"
          "\t                            gif/stream animation with this value. Use -1 to use default value.\n"

          "\nOptions affecting display of multiple images:\n"
          "\t-f                        : "
          "Forever cycle through the list of files on the command line.\n"
          "\t-s                        : If multiple images are given: shuffle.\n"
          "\nDisplay Options:\n"
          "\t-V<vsync-multiple>        : Expert: Only do frame vsync-swaps on multiples of refresh (default: 1)\n"
          );

  fprintf(stderr, "\nGeneral LED matrix options:\n");
  rgb_matrix::PrintMatrixFlags(stderr);

  fprintf(stderr,
          "\nSwitch time between files: "
          "-w for static images; -t/-l for animations\n"
          "Animated gifs: If both -l and -t are given, "
          "whatever finishes first determines duration.\n");

  fprintf(stderr, "\nThe -w, -t and -l options apply to the following images "
          "until a new instance of one of these options is seen.\n"
          "So you can choose different durations for different images.\n");

	exit(1);
  return 1;
}

