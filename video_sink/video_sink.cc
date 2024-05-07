#include "video_sink.h"

/* ========== Helpers =========== */

#define DEFAULT_RTSP_PORT 554

int parse_rtsp_url(string url, string& domain, int& port, string& extension, string& err_str)
{
  std::regex rgx("rtsp:\\/\\/([^:]*):?([0-9]*)\\/(.*)");
  std::smatch match;
  if (!std::regex_search(url, match, rgx)) {
    return 1;
  }
  if (match.size() != 4) {
    err_str = "Invalid url format \"" + url + "\"";
    return 1;
  }
  if (match[1].length() == 0) {
    err_str = "Invalid domain";
    return 1;
  }
  if (match[3].length() == 0) {
    err_str = "Invalid extension";
    return 1;
  }
  if (match[2].length() == 0) {
    // no port specified, use default port
    port = DEFAULT_RTSP_PORT;
  } else {
    port = std::stoi(match[2]);
  }
  domain    = match[1];
  extension = match[3];
  return 0;
}

/* ============================== */
/* ======== VideoSinkGST ======== */
/* ============================== */

#define TIMEOUT 10000000 // 10 seconds, in micro-seconds

void VideoSinkGST::push_data(GstElement*, guint, GSTData* data)
{
  static unsigned int frame_no = 1;
  unsigned int num_attempts    = 0;
  GstBuffer* buffer;
  GstFlowReturn ret;

  PacketPtr<const ImagePacket> image_packet;
  while (!data->thread_in_q->dequeue(image_packet)) {
    /* Queue is empty, stop pulling data */
    usleep(1000);
    num_attempts++;
    if (num_attempts * 1000 > TIMEOUT) {
      AUP_AVAF_LOG_NODE(data->node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                        __func__ << ": Gstreamer reached timeout waiting for input data");
      g_signal_emit_by_name(data->appsrc, "end-of-stream", &ret);
      return;
    }
    if (!data->running) {
      AUP_AVAF_LOG_NODE(data->node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                        __func__ << ": Gstreamer not running, stopping stream and exiting "
                                 << __func__);
      g_signal_emit_by_name(data->appsrc, "end-of-stream", &ret);
      return;
    }
  }
  ErrorCode err = image_packet->get_gst_buffer(buffer);
  if (err != ErrorCode::OK) {
    AUP_AVAF_LOG_NODE(data->node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      __func__ << ": Could not grab the GstBuffer from ImagePacket");
    g_signal_emit_by_name(data->appsrc, "end-of-stream", &ret);
    return;
  }

  // GstClockTime pts = GST_BUFFER_PTS(buffer);
  // AUP_AVAF_LOG_NODE(data->node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
  //                     __func__ << ": PTS is " << pts);

  // if (!gst_buffer_is_writable(buffer)) {
  //   buffer = gst_buffer_make_writable(buffer);
  // }
  // GstVideoMeta* meta = gst_buffer_get_video_meta(buffer);
  // if (!meta) {
  //   gst_buffer_add_video_meta(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12,
  //                             data->options->img_width, data->options->img_height);
  // }

  g_signal_emit_by_name(data->appsrc, "push-buffer", buffer, &ret);
  // gst_buffer_unref(buffer); // TODO no unreffing here! done in imagepacket destructor
  if (ret != GST_FLOW_OK) {
    /* We got some error, stop sending data */
    AUP_AVAF_LOG_NODE(data->node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "GStreamer emit got error: " << ret);
    g_signal_emit_by_name(data->appsrc, "end-of-stream", &ret);
    return;
  }

  if (frame_no % 50 == 0) {
    AUP_AVAF_LOG_NODE(data->node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                      "VideoSink processed frame "
                        << frame_no << " with pts:" << image_packet->get_pres_timestamp()
                        << ", sts:" << image_packet->get_sync_timestamp());
  }
  frame_no++;
}

ErrorCode VideoSinkGST::initialize(std::string& err_str)
{
  if (!data.thread_in_q || !data.options || !data.node) {
    err_str =
      "Could not initialize VideoSinkGST: options, thread_in_q, or node are NULL, are you sure "
      "you passed those to the constructor?";
    return ErrorCode::ERROR;
  }

  gst_init(NULL, NULL);

  std::stringstream pipeline_ss;
  // clang-format off
  /* App source */
  pipeline_ss << "appsrc name=nv12_frame_source ";
  /* Video setup */
  pipeline_ss << "! video/x-raw, ";
  pipeline_ss << "width=" << data.options->img_width << ", height=" << data.options->img_height << ", ";
  pipeline_ss << "format=NV12, framerate=" << data.options->framerate_numerator << "/" << data.options->framerate_denominator << " ";
  // /* Encoder */
  pipeline_ss << "! queue ! omx" << data.options->out_type << "enc ";
  pipeline_ss << "qp-mode=auto ";
  pipeline_ss << "gop-mode=" << data.options->gop_mode << " ";
  pipeline_ss << "control-rate=" << data.options->rate_control_mode << " ";
  if (data.options->target_bitrate) {
    pipeline_ss << "target-bitrate=" << data.options->target_bitrate << " ";
  }
  pipeline_ss << "gop-length=" << data.options->gop_length << " ";
  pipeline_ss << "b-frames=" << data.options->bframes << " ";
  // TODO may wish to change these
  pipeline_ss << "gdr-mode=horizontal cpb-size=200 num-slices=8 periodicity-idr=270 ";
  pipeline_ss << "initial-delay=100 filler-data=false min-qp=15 max-qp=40 low-bandwidth=false ";
  pipeline_ss << "! video/x-" << data.options->out_type << ", alignment=au ";
  pipeline_ss << "! queue ! rtp" << data.options->out_type << "pay name=pay0 pt=96 ";
  pipeline_ss << "! udpsink name=m_udpsink host=127.0.0.1 port=" << data.options->udp_port << " sync=false ";
  // clang-format on
  AUP_AVAF_LOG_NODE(data.node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "encoder pipeline : " << pipeline_ss.str());

  /* Setup Pipeline */
  data.pipeline = gst_parse_launch(pipeline_ss.str().c_str(), NULL);
  if (!data.pipeline) {
    err_str = "Issue creating GStreamer pipeline";
    return ErrorCode::ERROR;
  }

  /* Setup appsrc with signals */
  data.appsrc = gst_bin_get_by_name(GST_BIN(data.pipeline), "nv12_frame_source");
  if (!data.appsrc || !GST_IS_APP_SRC(data.appsrc)) {
    err_str = "Issue grabbing GStreamer appsrc";
    return ErrorCode::ERROR;
  }
  // clang-format off
  GstCaps* video_caps = gst_caps_new_simple(
    "video/x-raw",
    "format", G_TYPE_STRING, "NV12",
    "width", G_TYPE_INT, data.options->img_width,
    "height", G_TYPE_INT, data.options->img_height, 
    "framerate", GST_TYPE_FRACTION, data.options->framerate_numerator, data.options->framerate_denominator, 
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

  data.running = true;
  gst_element_set_state(data.pipeline, GST_STATE_PLAYING);

  return ErrorCode::OK;
}

VideoSinkGST::~VideoSinkGST()
{
  AUP_AVAF_LOG_NODE(data.node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "Stopping VideoSinkGST pipeline");
  data.running = false;
  gst_element_set_state(data.pipeline, GST_STATE_NULL);

  AUP_AVAF_LOG_NODE(data.node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "Removing references to VideoSinkGST pipeline");
  gst_object_unref(data.appsrc);
  gst_object_unref(data.pipeline);

  delete data.options;
  data.options = NULL;
}

/* =============================== */
/* ========== UDPtoRTSP ========== */
/* =============================== */

int UDPtoRTSP::initialize(std::string& err_str)
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
  pipeline_ss << "! rtspclientsink protocols=tcp ";
  pipeline_ss << "location=\"" << out_url << "\" ";
  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, "udp to rtsp pipeline: " << pipeline_ss.str());

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

UDPtoRTSP::~UDPtoRTSP()
{
  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, "Stopping UDPtoRTSP pipeline");
  gst_element_set_state(pipeline, GST_STATE_NULL);

  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, "Removing references to UDPtoRTSP pipeline");
  gst_object_unref(pipeline);
}

/* =============================== */
/* ===== VideoSinkCalculator ===== */
/* =============================== */

std::atomic<int> VideoSinkCalculator::id_counter;

ErrorCode VideoSinkCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
  if (contract->input_stream_names.size() != 2) {
    err_str = "node requires side packets hence needs exactly two inputs streams";
    return ErrorCode::INVALID_CONTRACT;
  }
  contract->sample_input_packets[0] = make_packet<ImagePacket>();
  contract->sample_input_packets[1] = make_packet<VideoStreamInfoPacket>();
  contract->input_attrs_arr[1].set_type(contract->input_attrs_arr[1].SIDE_PACKET);
  return ErrorCode::OK;
}

aup::avaf::ErrorCode VideoSinkCalculator::initialize(std::string& err_str)
{
  if (options->path().empty() && !node->get_output_url().empty()) {
    options->set_path(node->get_output_url());
  }
  if (!gst_debug_is_active()) {
    gst_debug_set_active(TRUE);
    GstDebugLevel dbglevel = gst_debug_get_default_threshold();
    if (dbglevel < GST_LEVEL_ERROR) {
      dbglevel = GST_LEVEL_ERROR;
      gst_debug_set_default_threshold(dbglevel);
    }
  }
  VideoSinkGSTOptions* gst_options     = new VideoSinkGSTOptions();

  ErrorCode ec = node->dequeue_block(1, i_img_stream_info);
  if (ec != ErrorCode::OK) {
    err_str = "Issue reading side packet for stream info: " + to_string(ec);
    return ec;
  }
  if (i_img_stream_info->pixfmt != PIXFMT_NV12) {
    err_str = "VideoSink only accepts NV12 input pixfmt, got " + aup::avaf::PixFmt_Name(i_img_stream_info->pixfmt); 
    return ErrorCode::ERROR;
  }

  switch (options->codec_type()) {
    case CODEC_TYPE_H264:
      gst_options->out_type  = "h264";
      break;
    // case VideoCodecOptions::Encoder::H265:
    //   gst_options->out_type = "h265";
    //   break;
    default:
      err_str =
        " Not setting correct codec type, only support CODEC_TYPE_H264 (h264)"; // "/
                                                                                // CODEC_TYPE_H265
                                                                                // (h265)";
      return ErrorCode::ERROR;
  }
  if (options->rc_mode() == "Low Latency") {
    gst_options->rate_control_mode = "low-latency";
    // case "CBR":
    //   gst_options->rate_control_mode = "constant";
    //   break;
    // case "VBR":
    //   gst_options->rate_control_mode = "variable";
    //   break;
  } else {
    err_str =
      " Not setting correct bitrate control mode (rc_mode), only support `Low Latency`"; // "CBR /
                                                                                         // VBR";
    return ErrorCode::ERROR;
  }
  if (options->gop_mode() == "low-latency-P") {
    gst_options->gop_mode = "low-delay-p";
    // case "default":
    //   gst_options->gop_mode = "basic";
    //   break;
    // case "low-latency-B":
    //   gst_options->gop_mode = "low-delay-b";
    //   break;
    // case "adaptive-B":
    //   gst_options->gop_mode = "adaptive";
    //   break;
  } else {
    err_str =
      " Not setting correct gop mode, only support low-latency-P"; // default, low-latency-B, "
                                                                   // "low-latency-P, adaptive-B";
    return ErrorCode::ERROR;
  }
  // TODO set default values for bitrate, gop, etc.
  gst_options->target_bitrate = (guint)options->bitrate();
  gst_options->gop_length     = (guint)options->gop_size();
  gst_options->bframes        = (guint)options->bframes();
  if (gst_options->bframes != 0) {
    err_str = " Only supporting bframes = 0 at this time";
    return ErrorCode::ERROR;
  }
  gst_options->img_width             = (guint)i_img_stream_info->w;
  gst_options->img_height            = (guint)i_img_stream_info->h;
  gst_options->framerate_numerator   = (guint)i_img_stream_info->framerate_numerator;
  gst_options->framerate_denominator = (guint)i_img_stream_info->framerate_denominator;

  // New port is selected based on unique object id
  if(options->udp_port() != 0) {
    gst_options->udp_port = options->udp_port();
  } else {
    gst_options->udp_port  = 10555 + obj_id;
    options->set_udp_port(gst_options->udp_port);
  }

  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    __func__ << " video source options: " << options->DebugString());
  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "video source width = " << to_string(i_img_stream_info->w));
  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "video source height = " << to_string(i_img_stream_info->h));
  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "video source fps = " << to_string(i_img_stream_info->framerate_numerator)
                                          << "/"
                                          << to_string(i_img_stream_info->framerate_denominator));

  gst_instance = new VideoSinkGST(gst_options, &buffer_queue, node);

  std::string gst_err_str;
  ec = gst_instance->initialize(gst_err_str);
  if (ec != ErrorCode::OK) {
    err_str = "Error initializing gstreamer encoder: " + gst_err_str;
    return ec;
  }

  std::vector<std::string> ips = GetIp();
  ips.push_back("localhost");
  string output_domain, output_extension;
  int output_port;
  if(parse_rtsp_url(options->path(), output_domain, output_port, output_extension, gst_err_str) != 0) {
    err_str = "Error parsing output url: " + gst_err_str;
    return ErrorCode::ERROR;
  }

  if (std::find(std::begin(ips), std::end(ips), output_domain) != std::end(ips)) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "Using local ip " << output_domain << ", starting local RTSP server");
    UDPToRTSPServerOptions* rtsp_options = new UDPToRTSPServerOptions();
    rtsp_options->out_type = gst_options->out_type;
    rtsp_options->udp_port = gst_options->udp_port;
    rtsp_options->out_port = (guint)output_port;
    rtsp_options->stream_name = output_extension;
    rtsp_server_instance = new UDPToRTSPServer(rtsp_options);
    if (rtsp_server_instance->initialize(gst_err_str) != 0) {
      err_str = "Error initializing gstreamer rtsp server: " + gst_err_str;
      return ErrorCode::ERROR;
    }
  } else {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "Output domain is not local, connecting via rtspclientsink");
    rtsp_instance = new UDPtoRTSP(gst_options->udp_port, gst_options->out_type, options->path(), node);
    if (rtsp_instance->initialize(gst_err_str) != 0) {
      err_str = "Error initializing gstreamer rtsp: " + gst_err_str;
      return ErrorCode::ERROR;
    }
  }
  return ErrorCode::OK;
}

ErrorCode VideoSinkCalculator::execute()
{
  PacketPtr<const ImagePacket> vfrm;
  auto ec = node->get_packet(0, vfrm);
  if (ec != ErrorCode::OK) {
    return ec;
  }
  buffer_queue.enqueue(vfrm);
  send_img_packet_cnt++;
  AUP_AVAF_DBG_NODE(node, __func__ << " send " << send_img_packet_cnt
                                   << " packet to gst done, pts:" << vfrm->get_pres_timestamp()
                                   << ", sts:" << vfrm->get_sync_timestamp());
  return ErrorCode::OK;
}

AUP_AVAF_REGISTER_CALCULATOR_EXT("Aupera", "video_sink", VideoSinkCalculator, VideoSinkOptions,
                                 true, "Aupera's video sink calculator.", {})