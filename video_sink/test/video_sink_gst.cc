#include "video_sink_gst.h"

void VideoSinkGST::push_data(GstElement* appsrc, guint size, GSTData* data)
{
  static unsigned int frame_no = 1;
  GstBuffer* buffer;
  GstFlowReturn ret;

  // PacketPtr<ImagePacket> image_packet;
  // if (!thread_in_q->dequeue(image_packet)) {
  //   /* Queue is empty, stop pulling data */
  //   return FALSE;
  // }
  // buffer = image_packet->get_gst_buffer();

  while (!data->thread_in_q->dequeue(buffer)) {
    /* Queue is empty, wait a while */
    usleep(
      50); // TODO this may not be the safest way of doing things, can cause the callback to hang
  }

  if (!gst_buffer_is_writable(buffer)) {
    buffer = gst_buffer_make_writable(buffer);
  }
  GstVideoMeta* meta = gst_buffer_get_video_meta(buffer);
  if (!meta) {
    // std::cout << "Adding buffer meta..." << std::endl;
    gst_buffer_add_video_meta(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12,
                              data->options->img_width, data->options->img_height);
  }

  g_signal_emit_by_name(data->appsrc, "push-buffer", buffer, &ret);
  gst_buffer_unref(buffer);
  if (frame_no % 50 == 0) {
    std::cout << "VideoSink processed frame " << frame_no << std::endl;
  }

  if (ret != GST_FLOW_OK) {
    /* We got some error, stop sending data */
    std::cout << "GStreamer emit got error: " << ret << std::endl;
    // return;
  }
  frame_no++;
}

void VideoSinkGST::initialize(std::string& err_str)
{
  // for (int i = 0; i < 1920*1080; i++) { b_black[i] = 0; b_white[i] = 0xFFFF; }

  GstBus* bus;

  gst_init(NULL, NULL);

  std::stringstream pipeline_ss;
  // clang-format off
  /* App source */
  pipeline_ss << "appsrc name=nv12_frame_source ";
  /* Video setup */
  pipeline_ss << "! video/x-raw, ";
  pipeline_ss << "width=" << data.options->img_width << ", height=" << data.options->img_height << ", ";
  pipeline_ss << "format=NV12, framerate=30/1 "; // TODO use fps from proto i.e. data.options->fps
  // /* Encoder */
  pipeline_ss << "! queue ! omx" << data.options->out_type << "enc ";
  pipeline_ss << "qp-mode=auto ";
  // pipeline_ss << "gop-mode=" << data.options->gop_mode << " ";
  pipeline_ss << "control-rate=" << data.options->rate_control_mode << " ";
  if (data.options->target_bitrate) {
    pipeline_ss << "target-bitrate=" << data.options->target_bitrate << " ";
  }
  pipeline_ss << "gop-length=" << data.options->gop_length << " ";
  // pipeline_ss << "b-frames=" << data.options->bframes << " ";
  // TODO may wish to change these
  pipeline_ss << "gop-mode=low-delay-p gdr-mode=horizontal cpb-size=200 num-slices=8 periodicity-idr=270 ";
  pipeline_ss << "initial-delay=100 filler-data=false min-qp=15 max-qp=40 b-frames=0 low-bandwidth=false ";
  pipeline_ss << "! video/x-" << data.options->out_type << ", alignment=au ";
  pipeline_ss << "! queue ! rtp" << data.options->out_type << "pay name=pay0 pt=96 ";
  pipeline_ss << "! udpsink name=m_udpsink host=127.0.0.1 port=" << data.options->udp_port << " sync=false ";
  // pipeline_ss << "! filesink location=./out." << data.options->out_type << " async=false ";
  // pipeline_ss
  //   << "! perf ! rtspclientsink location=rtsp://avac.auperatechnologies.com:554/mystream_out";
  // clang-format on
  std::cout << "============ GST ENC PIPELINE ===========\n" << pipeline_ss.str() << std::endl;

  /* Setup Pipeline */
  data.pipeline = gst_parse_launch(pipeline_ss.str().c_str(), NULL);
  if (!data.pipeline) {
    err_str = "Issue creating GStreamer pipeline";
    return;
  }

  /* Setup appsrc with signals */
  data.appsrc = gst_bin_get_by_name(GST_BIN(data.pipeline), "nv12_frame_source");
  if (!data.appsrc || !GST_IS_APP_SRC(data.appsrc)) {
    err_str = "Issue grabbing GStreamer appsrc";
    return;
  }
  // clang-format off
  GstCaps* video_caps = gst_caps_new_simple(
    "video/x-raw",
    "format", G_TYPE_STRING, "NV12",
    "width", G_TYPE_INT, data.options->img_width,
    "height", G_TYPE_INT, data.options->img_height, 
    "framerate", GST_TYPE_FRACTION, 30, 1, 
    NULL
  );
  g_object_set(G_OBJECT(data.appsrc), 
               "caps", video_caps, 
               "format", GST_FORMAT_TIME, 
               "stream-type", 0, 
              //  "is-live", TRUE,
               NULL
  );
  // clang-format on
  g_signal_connect(data.appsrc, "need-data", G_CALLBACK(push_data), &data);
  gst_caps_unref(video_caps);

  /* Start playing the pipeline */
  gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
}

VideoSinkGST::~VideoSinkGST()
{
  std::cout << "Stopping pipeline" << std::endl;
  gst_element_set_state(data.pipeline, GST_STATE_NULL);

  std::cout << "Removing references to pipeline" << std::endl;
  gst_object_unref(data.appsrc);
  gst_object_unref(data.pipeline);
}

int UDPRTSP::initialize(std::string& err_str)
{
  gst_init(NULL, NULL);

  std::stringstream pipeline_ss;
  // clang-format off
  /* App source */
  pipeline_ss << "udpsrc port=" << udp_port << " ";
  pipeline_ss << "caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, ";
  if (out_type == "h264") {
    pipeline_ss << "encoding-name=(string)H264\" ";
  } else {
    err_str = "unknown encoding type " + out_type;
    return 1;
  }
  pipeline_ss << "! rtp" << out_type << "depay ";
  pipeline_ss << "! rtspclientsink protocols=tcp location=\"rtsp://avac.auperatechnologies.com:554/mystream_out\" ";
  std::cout << "============ RTSP OUT PIPELINE ===========\n" << pipeline_ss.str() << std::endl;

  /* Setup Pipeline */
  pipeline = gst_parse_launch(pipeline_ss.str().c_str(), NULL);
  if (!pipeline) {
    err_str = "Issue creating GStreamer pipeline";
    return 1;
  }

  /* Start playing the pipeline */
  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  return 0;
}

UDPRTSP::~UDPRTSP()
{
  std::cout << "Stopping pipeline" << std::endl;
  gst_element_set_state(pipeline, GST_STATE_NULL);

  std::cout << "Removing references to pipeline" << std::endl;
  gst_object_unref(pipeline);
}