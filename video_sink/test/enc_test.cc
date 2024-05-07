#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "video_sink_gst.h"
#include <gst/app/gstappsink.h>

class VideoSourceGST
{
private:
  typedef struct _GSTData
  {
    GstElement* pipeline = NULL;
    GstElement* appsink  = NULL;
  } GSTData;

  GSTData data;
  GSTThreadSafeQueue<GstBuffer*>* thread_out_q;

  static GstFlowReturn new_sample(GstAppSink* appsink, GSTThreadSafeQueue<GstBuffer*>* out_queue);

public:
  void initialize();
  VideoSourceGST(GSTThreadSafeQueue<GstBuffer*>* out_queue) : thread_out_q(out_queue) {}
  ~VideoSourceGST();
};

// The callback function
GstFlowReturn VideoSourceGST::new_sample(GstAppSink* appsink,
                                         GSTThreadSafeQueue<GstBuffer*>* out_queue)
{
  static unsigned int frame_no = 1;
  GstSample* sample            = gst_app_sink_pull_sample(appsink);
  GstBuffer* buffer            = gst_sample_get_buffer(sample);
  // GstVideoMeta *video_meta = gst_buffer_get_video_meta(buffer);
  // if (video_meta) {
  //     std::cout << "Video Meta: Width = " << video_meta->width << ", Height = " <<
  //     video_meta->height << std::endl;
  // } else {
  //     std::cout << "No video meta available" << std::endl;
  // }
  GstBuffer* buffer_copy = gst_buffer_copy(buffer);
  // if (!gst_buffer_is_writable(buffer_copy)) {
  //   std::cout << "Buffer is not writable" << std::endl;
  //   buffer_copy = gst_buffer_make_writable(buffer_copy);
  //   if (!gst_buffer_is_writable(buffer_copy)) {
  //     std::cout << "BIG BAD WARNING!!!";
  //   }
  // }
  out_queue->enqueue(buffer_copy);
  gst_sample_unref(sample);
  if (frame_no % 50 == 0) {
    std::cout << "VideoSource Processed frame " << frame_no << std::endl;
  }

  frame_no++;
  return GST_FLOW_OK;
}

void VideoSourceGST::initialize()
{
  gst_init(NULL, NULL);

  // Build the pipeline
  // clang-format off
  data.pipeline = gst_parse_launch(
      "rtspsrc location=\"rtsp://avac.auperatechnologies.com:554/car\" ! queue "
      "! rtph264depay ! queue ! h264parse ! queue ! omxh264dec ! video/x-raw, "
      "width=1920, height=1080, format=NV12 , framerate=30/1 ! appsink "
      "name=nv12_app_sink",
      NULL);
  // clang-format on

  // Get the appsink element
  data.appsink = gst_bin_get_by_name(GST_BIN(data.pipeline), "nv12_app_sink");
  // clang-format off
  GstCaps* video_caps = gst_caps_new_simple(  
    "video/x-raw", 
    "format", G_TYPE_STRING, "NV12", 
    "width", G_TYPE_INT, 1920,
    "height", G_TYPE_INT, 1080, 
    "framerate", GST_TYPE_FRACTION, 30, 1, 
    NULL
  );
  g_object_set(G_OBJECT(data.appsink), 
               "emit-signals", TRUE, 
               "sync", FALSE, 
               "caps", video_caps,
               NULL
  );
  // clang-format on
  g_signal_connect(data.appsink, "new-sample", G_CALLBACK(new_sample), thread_out_q);
  gst_caps_unref(video_caps);

  // Start playing
  gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
}

VideoSourceGST::~VideoSourceGST()
{
  std::cout << "Stopping pipeline" << std::endl;
  gst_element_set_state(data.pipeline, GST_STATE_NULL);

  std::cout << "Removing references to pipeline" << std::endl;
  g_object_unref(data.appsink);
  gst_object_unref(GST_OBJECT(data.pipeline));
}

static void sigint_handler(int signum);
volatile static int done = 0;

int main(int argc, char* argv[])
{
  VideoSinkGSTOptions gst_options;

  gst_options.out_type          = "h264";
  gst_options.rate_control_mode = "low-latency";
  gst_options.gop_mode          = "basic";
  gst_options.target_bitrate    = (guint)3000;
  gst_options.gop_length        = (guint)60;
  gst_options.bframes           = (guint)0;
  gst_options.img_width         = (guint)1920;
  gst_options.img_height        = (guint)1080;
  gst_options.udp_port          = (guint)5000;

  GSTThreadSafeQueue<GstBuffer*> my_queue;
  VideoSourceGST gst_source_instance(&my_queue);
  VideoSinkGST gst_sink_instance(&gst_options, &my_queue);

  gst_source_instance.initialize();

  std::string gst_err_str;
  gst_sink_instance.initialize(gst_err_str);
  if (!gst_err_str.empty()) {
    std::cout << "err_str is: " << gst_err_str << std::endl;
    return 1;
  }

  UDPRTSP rtsp_pipeline;
  if (rtsp_pipeline.initialize(gst_err_str) != 0) {
    std::cout << "err_str is: " << gst_err_str << std::endl;
    return 1;
  }

  if (signal(SIGINT, sigint_handler) == SIG_ERR) {
    perror("signal()");
    exit(1);
  }

  while (!done) {
    std::cout << "queue size is " << my_queue.get_size() << std::endl;
    sleep(1);
  }

  printf("got SIGINT\n");
}

void sigint_handler(int signum) { done = 1; }