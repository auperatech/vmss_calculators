// stl headers
#include <chrono>
#include <cmath>
#include <fcntl.h> // for open()
#include <filesystem>
#include <iostream>
#include <linux/videodev2.h>
#include <memory>
#include <numeric>
#include <regex>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h> // for close()
#include <vector>

// SDK Headers
#include <opencv2/opencv.hpp>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
}

// avaf headers
#include "aup/avaf/calculator.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/image_packet.h"
#include "aup/avaf/packets/video_stream_info_packet.h"
#include "aup/avaf/thread_name.h"

// avap headers
#include "aup/avap/video_source.pb.h"

using namespace std;
using namespace cv;
using namespace aup::avaf;
namespace fs = std::filesystem;

class VideoSourceCalculator : public CalculatorBase<VideoSourceOptions>
{
  struct ResolutionFPS
  {
    uint32_t width;
    uint32_t height;
    uint32_t framerate_numerator;
    uint32_t framerate_denominator;
  };
  PacketPtr<VideoStreamInfoPacket> video_stream_info;
  PacketPtr<VideoStreamInfoPacket> video_stream_info_nv12;
  VideoCapture vidcap;
  thread usb_video_capture_thread;
  void usb_video_capture_worker();
  thread rtsp_video_capture_thread;
  void rtsp_file_video_capture_worker();
  bool running      = false;
  uint64_t next_pts = 1'000'000;
  shared_ptr<ImagePacket::Allocator> allocator;
  vector<ResolutionFPS> get_usbcam_resfpr(std::string cam_path);
  ErrorCode update_and_validate_options(string& err_str);
  ErrorCode update_and_validate_options_rtsp_file(string& err_str);
  ErrorCode initialize_usbcam(string& err_str);
  ErrorCode initialize_rtsp_file(string& err_str);
  timestamp_t last_sts        = timestamp_min;
  timestamp_t last_sts_now_us = timestamp_min;
  timestamp_t sts_pts_offset  = 0;
  int frame_distance_us       = 0;

protected:
  ErrorCode fill_contract(std::shared_ptr<Contract>& contract, std::string& err_str) override;

public:
  VideoSourceCalculator(const ::aup::avaf::Node* node) : CalculatorBase(node) {}
  ~VideoSourceCalculator();
  ErrorCode initialize(std::string& err_str) override;
  GstFlowReturn new_sample(GstAppSink* appsink);
  GstFlowReturn new_sample_2outputs(GstAppSink* appsink);
};

VideoSourceCalculator::~VideoSourceCalculator()
{
  running = false;
  AUP_AVAF_THREAD_JOIN_NOTERM(usb_video_capture_thread);
  AUP_AVAF_THREAD_JOIN_NOTERM(rtsp_video_capture_thread);
}

void VideoSourceCalculator::usb_video_capture_worker()
{
  AUP_AVAF_HANDLE_THREAD_NAME();
  while (true) {
    auto graph_status = node->get_graph_status();
    if (graph_status == GraphStatus::FAILED || graph_status == GraphStatus::FINISHED) {
      return;
    }
    if (graph_status == GraphStatus::RUNNING) {
      break;
    }
    using namespace std::chrono_literals;
    this_thread::sleep_for(100us);
  }
  while (running) {
    auto graph_status = node->get_graph_status();
    if (graph_status == GraphStatus::FAILED || graph_status == GraphStatus::FINISHED) {
      return;
    }
    using namespace std::chrono_literals;
    this_thread::sleep_for(100us);
    PacketPtr<ImagePacket> image_packet;
    ErrorCode ec = ErrorCode::OK;
    while (node->get_graph_status() == GraphStatus::RUNNING) {
      image_packet = make_packet<ImagePacket>(next_pts, false, allocator, ec);
      if (ec == ErrorCode::OK) {
        break;
      }
      using namespace std::chrono_literals;
      this_thread::sleep_for(100us);
    }
    auto& cv_mat = image_packet->get_cv_mat();
    vidcap >> cv_mat;
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_DEBUG,
                      "Got image with pts " << next_pts);
    image_packet->set_pres_timestamp(next_pts);
    next_pts += (uint32_t)1'000'000 / video_stream_info->fps;
    if ((ec = node->enqueue(0, image_packet)) != ErrorCode::OK) {
      AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
                        "Issue " << ec << " enqueueing.");
    }
  }
}

ErrorCode VideoSourceCalculator::fill_contract(std::shared_ptr<Contract>& contract,
                                               std::string& err_str)
{
  if (contract->input_stream_names.size() != 0) {
    err_str = "node cannot have any inputs";
    return ErrorCode::INVALID_CONTRACT;
  }
  uint32_t sz_output = (uint32_t)contract->output_stream_names.size();
  if (sz_output != 2 && sz_output != 4) {
    err_str = "node has exactly two or four outputs";
    return ErrorCode::INVALID_CONTRACT;
  }
  contract->sample_output_packets[0] = make_packet<ImagePacket>(); // BGR
  contract->sample_output_packets[1] = make_packet<VideoStreamInfoPacket>();
  if (sz_output == 4) {
    contract->sample_output_packets[2] = make_packet<ImagePacket>(); // NV12
    contract->sample_output_packets[3] = make_packet<VideoStreamInfoPacket>();
  }
  return ErrorCode::OK;
}

static bool is_readable_regular_file(const std::string& path_string)
{
  fs::path path(path_string);

  try {
    // Resolve all symbolic links, and normalize the path
    fs::path resolved_path = fs::canonical(path);

    // Check if the resolved path is a regular file
    if (fs::is_regular_file(resolved_path)) {
      // Check for read permissions
      std::error_code ec;
      auto perms = fs::status(resolved_path, ec).permissions();
      if (ec) {
        std::cerr << "Error accessing permissions: " << ec.message() << std::endl;
        return false;
      }

      return (perms & fs::perms::owner_read) != fs::perms::none ||
             (perms & fs::perms::group_read) != fs::perms::none ||
             (perms & fs::perms::others_read) != fs::perms::none;
    }
  } catch (const fs::filesystem_error& e) {
    std::cerr << "Error resolving path: " << e.what() << std::endl;
    // Handle cases like loops in symlinks or a symlink chain leading to a non-existent file
  }

  return false;
}

ErrorCode VideoSourceCalculator::update_and_validate_options_rtsp_file(string& err_str)
{
  auto rtsp_url                   = options->path();
  AVFormatContext* fmt_ctx        = nullptr;
  AVCodecParameters* codec_params = nullptr;
  int vid_stream_idx              = -1;
  AVDictionary* opts              = NULL;
  string err_str_local;
  int av_err;
  char av_err_str[AV_ERROR_MAX_STRING_SIZE];
  CodecType video_codec_type = CODEC_TYPE_NONE;
  // Validate RTSP URL
  AUP_AVAF_TRACE_NODE(node);
  if (rtsp_url.find("rtsp://") == 0) {
    options->set_source_type(VideoSourceOptions::RTSP);
  } else if (is_readable_regular_file(rtsp_url)) {
    options->set_source_type(VideoSourceOptions::FILE);
  } else {
    err_str += "Invalid RTSP URL or file path. It must start with 'rtsp://' or must point to a "
               "valid readable file. ";
    return ErrorCode::ERROR;
  }

  bool is_validate_only = options->codec_type() && options->framerate_numerator() &&
                          options->framerate_denominator() && options->width() &&
                          options->height() && options->codec_type();
  if (is_validate_only) {
    return ErrorCode::OK;
  }
  ErrorCode ret = is_validate_only ? ErrorCode::OK : ErrorCode::ERROR;

  // Initialize libavformat and register all formats and codecs
#if LIBAVFORMAT_VERSION_MAJOR < 58
  av_register_all();
#endif
  avformat_network_init();

  if ((av_err = av_dict_set(&opts, "rtsp_transport", "tcp", 0)) < 0) {
    av_make_error_string(av_err_str, sizeof(av_err_str), av_err);
    err_str_local = "Could not set tcp options with av_error " + string(av_err_str) + ". ";
    goto close_finish;
  }

  if (false && (av_err = av_dict_set_int(&opts, "timeout", 5, 0)) < 0) {
    av_make_error_string(av_err_str, sizeof(av_err_str), av_err);
    err_str_local = "Could not set timeout options with av_error " + string(av_err_str) + ". ";
    goto close_finish;
  }

  // Open the RTSP stream
  if ((av_err = avformat_open_input(&fmt_ctx, rtsp_url.c_str(), nullptr, &opts)) != 0) {
    av_make_error_string(av_err_str, sizeof(av_err_str), av_err);
    err_str_local += "Failed to open input. av_error: " + string(av_err_str) + " ";
    goto deinit_finish;
  }

  if ((av_err = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
    av_make_error_string(av_err_str, sizeof(av_err_str), av_err);
    err_str_local = "Could not find stream information with av_error " + string(av_err_str) + ". ";
    goto close_finish;
  }

  // Find the first video stream
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    codec_params = fmt_ctx->streams[i]->codecpar;
    if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
      vid_stream_idx = i;
      break;
    }
  }

  if (vid_stream_idx == -1) {
    err_str_local += "Failed to find a video stream. ";
    goto close_finish;
  }

  // Check the codec ID
  switch (codec_params->codec_id) {
    case AV_CODEC_ID_H264:
      video_codec_type = CODEC_TYPE_H264;
      break;
    case AV_CODEC_ID_HEVC:
      video_codec_type = CODEC_TYPE_H265;
      break;
    default: {
      const AVCodecDescriptor* codec_descriptor = avcodec_descriptor_get(codec_params->codec_id);
      if (!codec_descriptor) {
        err_str_local += "Cannot decode codec of unknown type. ";
      } else {
        err_str_local += "Cannot decode codec of type '" + string(codec_descriptor->name) + "'. ";
      }
      goto close_finish;
    }
  }
  if (options->codec_type() == CODEC_TYPE_NONE) {
    options->set_codec_type(video_codec_type);
  } else if (options->codec_type() != video_codec_type) {
    err_str_local = "codec type is configured as " + CodecType_Name(options->codec_type()) +
                    " but the actual codec type is " + CodecType_Name(video_codec_type) + ". ";
    ret = ErrorCode::ERROR;
    goto close_finish;
  }
  if (options->framerate_numerator() == 0 || options->framerate_denominator() == 0) {
    options->set_framerate_numerator(fmt_ctx->streams[vid_stream_idx]->avg_frame_rate.num);
    options->set_framerate_denominator(fmt_ctx->streams[vid_stream_idx]->avg_frame_rate.den);
  } else if ((decltype(fmt_ctx->streams[vid_stream_idx]->avg_frame_rate.num))options
                 ->framerate_numerator() != fmt_ctx->streams[vid_stream_idx]->avg_frame_rate.num ||
             (decltype(fmt_ctx->streams[vid_stream_idx]->avg_frame_rate.den))
                 options->framerate_denominator() !=
               fmt_ctx->streams[vid_stream_idx]->avg_frame_rate.den) {
    err_str_local = "Framerate conversion is not supported. Must match input framerate.";
    ret           = ErrorCode::ERROR;
    goto close_finish;
  }
  if (options->width() == 0 || options->height() == 0) {
    options->set_width(codec_params->width);
    options->set_height(codec_params->height);
  } else if ((decltype(codec_params->width))options->width() != codec_params->width ||
             (decltype(codec_params->height))options->height() != codec_params->height) {
    err_str_local = "resolution conversion is not supported. Must match input resolution.";
    ret           = ErrorCode::ERROR;
    goto close_finish;
  }

  ret = ErrorCode::OK;
  // Clean up
close_finish:
  avformat_close_input(&fmt_ctx);
deinit_finish:
  avformat_network_deinit();
  if (is_validate_only) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
                      "Could not validate the options because " << err_str_local);
  } else {
    err_str += err_str_local;
  }
  return ret;
}

ErrorCode VideoSourceCalculator::update_and_validate_options(string& err_str)
{
  if (options->path().empty()) {
    vector<string> cam_paths;
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                      "No path provided, looking for usb cameras");
    for (const auto& entry : fs::directory_iterator("/dev")) {
      const auto& path     = entry.path();
      const auto& filename = path.filename().string();

      if (filename.find("video") == 0) {
        auto cam_path      = "/dev/" + filename;
        auto usbcam_resfpr = get_usbcam_resfpr(cam_path);
        if (!usbcam_resfpr.empty()) {
          cam_paths.push_back(cam_path);
        }
      }
    }
    std::sort(cam_paths.begin(), cam_paths.end());
    if (cam_paths.empty()) {
      err_str = "path field is empty and was not able to locate any devices.";
      return ErrorCode::ERROR;
    }
    options->set_path(cam_paths[0]);
    options->set_source_type(VideoSourceOptions::USB);
  }
  if (options->source_type() != VideoSourceOptions::AUTO_SOURCE_TYPE) {
    return ErrorCode::OK;
  }
  if (options->path().empty()) {
    err_str += "Cannot determine source_type on empty path.";
    return ErrorCode::ERROR;
  }
  auto usb_cam_resfps = get_usbcam_resfpr(options->path());
  if (!usb_cam_resfps.empty()) {
    options->set_source_type(VideoSourceOptions::USB);
    return ErrorCode::OK;
  }
  return update_and_validate_options_rtsp_file(err_str);
}

vector<VideoSourceCalculator::ResolutionFPS>
VideoSourceCalculator::get_usbcam_resfpr(std::string cam_path)
{
  vector<ResolutionFPS> ret;
  int fd = open(cam_path.c_str(), O_RDWR);
  if (fd == -1) {
    return ret;
  }

  // Enumerate supported formats
  v4l2_fmtdesc fmtdesc = {};
  fmtdesc.type         = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    if (fmtdesc.pixelformat != V4L2_PIX_FMT_YUYV) {
      fmtdesc.index++;
      continue;
    }

    // Enumerate frame sizes for the YUYV format
    v4l2_frmsizeenum frmsize = {};
    frmsize.pixel_format     = fmtdesc.pixelformat;

    while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
      if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
        frmsize.index++;

        continue;
      }
      ResolutionFPS res_fps{
        .width  = frmsize.discrete.width,
        .height = frmsize.discrete.height,
      };
      // Enumerate FPS for each resolution
      v4l2_frmivalenum frmival = {
        .pixel_format = fmtdesc.pixelformat,
        .width        = frmsize.discrete.width,
        .height       = frmsize.discrete.height,
      };

      while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
          res_fps.framerate_numerator   = frmival.discrete.denominator;
          res_fps.framerate_denominator = frmival.discrete.numerator;
          ret.push_back(res_fps);
        }
        frmival.index++;
      }

      frmsize.index++;
    }

    fmtdesc.index++;
  }

  close(fd);
  return ret;
}

ErrorCode VideoSourceCalculator::initialize_usbcam(std::string& err_str)
{
  auto cam_path = options->path();
  if (!fs::exists(cam_path)) {
    err_str = "Error: File" + options->path() + " does not exist!";
    return ErrorCode::ERROR;
  }

  auto usbcam_resfpr = get_usbcam_resfpr(cam_path);
  // Check if it's a potential video device (optional)
  struct stat file_stat;
  if (stat(cam_path.c_str(), &file_stat) == 0) {
    if (!S_ISCHR(file_stat.st_mode)) {
      err_str = "device " + cam_path + " is not a character device.";
      return ErrorCode::ERROR;
    }
  } else {
    err_str = "Error checking file " + cam_path;
    return ErrorCode::ERROR;
  }
  auto res_fps_arr = get_usbcam_resfpr(cam_path);
  stringstream ss;
  ss << "Supported resolution and framerate combinations:\n";
  for (const auto& entry : res_fps_arr) {
    ss << "width:" << entry.width << " height:" << entry.height
       << " framerate:" << entry.framerate_numerator << "/" << entry.framerate_denominator << ", ";
  }
  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO, ss.str());

  // TODO check resolution and framerate
  regex cam_path_idx_regex(R"(video(\d+))");
  smatch match;
  if (!regex_search(cam_path, match, cam_path_idx_regex)) {
    err_str = "Internal error in regex search of " + cam_path;
    return ErrorCode::ERROR;
  }
  int idx = stoi(match[1]);
  vidcap.open(idx);
  if (!vidcap.isOpened()) {
    err_str = "Could not open video capture file " + options->path();
  }
  if (options->width() == 0 || options->height() == 0) {
    options->set_width(0);
    options->set_height(0);
  }
  if (options->framerate_numerator() == 0 || options->framerate_denominator() == 0) {
    options->set_framerate_numerator(0);
    options->set_framerate_denominator(0);
  }
  if (options->width() == 0 && options->framerate_numerator() != 0) {
    ResolutionFPS matched_entry = {};
    bool matched                = false;
    for (auto& entry : res_fps_arr) {
      if (entry.width > 1920 || entry.height > 1080) {
        continue;
      }
      if (abs(((float)entry.framerate_numerator) / ((float)entry.framerate_denominator) -
              ((float)options->framerate_numerator() / ((float)options->framerate_denominator()))) <
            0.01 &&
          (!matched || matched_entry.height < entry.height)) {
        matched_entry = entry;
        matched       = true;
      }
    }
    if (!matched) {
      err_str = "Could not find a resolution matching framerate " +
                to_string(options->framerate_numerator()) + "/" +
                to_string(options->framerate_denominator());
      return ErrorCode::ERROR;
    }
    options->set_framerate_numerator(matched_entry.framerate_numerator);
    options->set_width(matched_entry.width);
    options->set_height(matched_entry.height);
  }
  if (options->width() != 0 && options->framerate_numerator() == 0) {
    ResolutionFPS matched_entry = {};
    bool matched                = false;
    for (auto& entry : res_fps_arr) {
      if (options->width() == entry.width && options->height() == entry.height &&
          (!matched ||
           ((float)matched_entry.framerate_numerator) /
               ((float)matched_entry.framerate_denominator) <
             ((float)entry.framerate_numerator) / ((float)entry.framerate_denominator))) {
        matched_entry = entry;
        matched       = true;
      }
    }
    if (!matched) {
      err_str = "Could not find any framerate matching resolution " + to_string(options->width()) +
                "x" + to_string(options->height());
      return ErrorCode::ERROR;
    }
    options->set_framerate_numerator(matched_entry.framerate_numerator);
    options->set_framerate_denominator(matched_entry.framerate_denominator);
  }
  if (options->width() == 0 && options->framerate_numerator() == 0) {
    ResolutionFPS matched_entry = {};
    bool matched                = false;
    for (auto& entry : res_fps_arr) {
      if (entry.width > 1920 || entry.height > 1080) {
        continue;
      }
      if (!matched) {
        matched_entry = entry;
        matched       = true;
        continue;
      }
      if (entry.width == matched_entry.width && entry.height == matched_entry.height &&
          ((float)entry.framerate_numerator) / ((float)entry.framerate_denominator) >
            ((float)matched_entry.framerate_numerator) / ((float)entry.framerate_denominator)) {
        matched_entry = entry;
        matched       = true;
        continue;
      }
      if (matched_entry.width == 1920 && matched_entry.height == 1080 && entry.width == 1280 &&
          entry.height == 720 &&
          ((float)entry.framerate_numerator) / ((float)entry.framerate_denominator) >=
            2 * ((float)matched_entry.framerate_numerator) /
              ((float)matched_entry.framerate_denominator)) {
        matched_entry = entry;
        matched       = true;
        continue;
      }
      if (entry.width == 1280 && entry.height == 720 &&
          ((float)entry.framerate_numerator) / ((float)entry.framerate_denominator) >=
            2 * ((float)matched_entry.framerate_numerator) /
              ((float)matched_entry.framerate_denominator)) {
        matched_entry = entry;
        matched       = true;
        continue;
      }
      if (entry.width == 1920 && entry.height == 1080 &&
          2 * ((float)entry.framerate_numerator) / ((float)entry.framerate_denominator) >
            ((float)matched_entry.framerate_numerator) /
              ((float)matched_entry.framerate_denominator)) {
        matched_entry = entry;
        matched       = true;
        continue;
      }
    }
    if (!matched) {
      err_str = "Could not find any framerate matching resolution " + to_string(options->width()) +
                "x" + to_string(options->height());
      return ErrorCode::ERROR;
    }
    options->set_width(matched_entry.width);
    options->set_height(matched_entry.height);
    options->set_framerate_numerator(matched_entry.framerate_numerator);
    options->set_framerate_denominator(matched_entry.framerate_denominator);
  }

  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "final options: " << options->DebugString());
  video_stream_info                        = make_packet<VideoStreamInfoPacket>();
  video_stream_info->iframe_extract        = false;
  video_stream_info->w                     = options->width();
  video_stream_info->h                     = options->height();
  video_stream_info->max_bframes           = true;
  video_stream_info->codec_type            = CODEC_TYPE_H265;
  video_stream_info->framerate_numerator   = options->framerate_numerator();
  video_stream_info->framerate_denominator = options->framerate_denominator();
  video_stream_info->fps =
    (float)video_stream_info->framerate_numerator / (float)video_stream_info->framerate_denominator;
  video_stream_info->pixfmt = PIXFMT_BGR24;

  ErrorCode ec = ErrorCode::OK;
  if ((ec = node->enqueue(1, video_stream_info)) != ErrorCode::OK) {
    err_str = "image_stream_initialize did not receive input stream info. code: " + to_string(ec);
    return ec;
  }
  vidcap.set(cv::CAP_PROP_FRAME_WIDTH, options->width());
  vidcap.set(cv::CAP_PROP_FRAME_HEIGHT, options->height());
  vidcap.set(cv::CAP_PROP_FPS,
             ((float)options->framerate_numerator()) / ((float)options->framerate_denominator()));
  allocator = ImagePacket::Allocator::new_normal_allocator(
    options->width(), options->height(), PIXFMT_BGR24, options->pool_size() ?: 12, ec);
  if (ec != ErrorCode::OK) {
    err_str = "issue instatiating allocator for video stream.";
    return ec;
  }
  running                  = true;
  usb_video_capture_thread = thread([&] { this->usb_video_capture_worker(); });
  return ErrorCode::OK;
}

GstFlowReturn VideoSourceCalculator::new_sample(GstAppSink* appsink)
{
  GstSample* gst_sample;
  GstBuffer* buffer;
  PacketPtr<ImagePacket> bgr_img_pkt = nullptr;
  ErrorCode ec                       = ErrorCode::OK;
  if (node->get_graph_status() != GraphStatus::RUNNING) {
    return GST_FLOW_OK;
  }
  while (node->get_graph_status() == GraphStatus::RUNNING) {
    bgr_img_pkt = make_packet<ImagePacket>(0, false, allocator, ec);
    if (ec == ErrorCode::OK) {
      break;
    }
    if (options->drop_packet_on_full_data_stream()) {
      AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
                        "image packet is dropped.");
      return GST_FLOW_OK;
    }
    usleep(100);
  }
  if (node->get_graph_status() != GraphStatus::RUNNING) {
    return GST_FLOW_OK;
  }
  AUP_AVAF_TRACE_NODE(node);
  gst_sample = gst_app_sink_pull_sample(appsink);
  if (!(buffer = gst_sample_get_buffer(gst_sample))) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "GST:Issue getting buffer from sample");
    return GST_FLOW_ERROR;
  }
  GstClockTime pts = GST_BUFFER_PTS(buffer);
  if (pts == GST_CLOCK_TIME_NONE) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "GST: no presentation timestamp");
    return GST_FLOW_ERROR;
  }
  timestamp_t this_sts        = pts + sts_pts_offset;
  timestamp_t this_sts_now_us = get_now_us();
  if (this_sts <= last_sts) {
    timestamp_t sts_pts_offset_change =
      ((this_sts_now_us - last_sts_now_us) / frame_distance_us) * frame_distance_us;
    sts_pts_offset += sts_pts_offset_change;
    this_sts += sts_pts_offset_change;
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
                      AUP_AVAF_TERM_COLOR_FG_MAGENTA
                      "PTS value dropped. Decoder will increase PTS offset value "
                      "accordingly" AUP_AVAF_TERM_FORMAT_RESET_ALL);
  }
  if (this_sts <= last_sts) {
    timestamp_t sts_pts_offset_change = last_sts - this_sts + frame_distance_us;
    sts_pts_offset += sts_pts_offset_change;
    this_sts += sts_pts_offset_change;
  }
  bgr_img_pkt->set_sync_timestamp(this_sts);
  bgr_img_pkt->set_pres_timestamp(pts);
  last_sts = this_sts;

  auto& bgr_cv_mat = bgr_img_pkt->get_cv_mat();

  GstMapInfo gst_map;
  if (!gst_buffer_map(buffer, &gst_map, GST_MAP_READ)) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "GST:issue mapping gst buffer");
    return GST_FLOW_ERROR;
  }
  cv::Mat yplane  = cv::Mat(cv::Size(options->width(), options->height()), CV_8UC1, gst_map.data);
  cv::Mat uvplane = cv::Mat(cv::Size(options->width() / 2, options->height() / 2), CV_8UC2,
                            gst_map.data + options->width() * options->height());
  cv::cvtColorTwoPlane(yplane, uvplane, bgr_cv_mat, cv::COLOR_YUV2BGR_NV12);
  if ((ec = node->enqueue(0, bgr_img_pkt)) != ErrorCode::OK) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_FATAL,
                      "Issue enqueueing BGR24 Image Packet " << ec);
    return GST_FLOW_ERROR;
  }
  gst_buffer_unmap(buffer, &gst_map);
  gst_sample_unref(gst_sample);

  return GST_FLOW_OK;
}

GstFlowReturn VideoSourceCalculator::new_sample_2outputs(GstAppSink* appsink)
{
  GstSample* gst_sample;
  GstBuffer* buffer;
  GstBuffer* buffer_copy;
  PacketPtr<ImagePacket> bgr_img_pkt = nullptr;
  ErrorCode ec                       = ErrorCode::OK;
  if (node->get_graph_status() != GraphStatus::RUNNING) {
    return GST_FLOW_OK;
  }
  while (node->get_graph_status() == GraphStatus::RUNNING) {
    bgr_img_pkt = make_packet<ImagePacket>(0, false, allocator, ec);
    if (ec == ErrorCode::OK) {
      break;
    }
    if (options->drop_packet_on_full_data_stream()) {
      AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
                        "image packet is dropped.");
      return GST_FLOW_OK;
    }
    usleep(100);
  }
  if (node->get_graph_status() != GraphStatus::RUNNING) {
    return GST_FLOW_OK;
  }
  gst_sample = gst_app_sink_pull_sample(appsink);
  if (!(buffer = gst_sample_get_buffer(gst_sample))) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "GST:Issue getting buffer from sample");
    return GST_FLOW_ERROR;
  }
  buffer_copy = gst_buffer_copy(buffer);
  gst_sample_unref(gst_sample);
  auto nv12_img_pkt =
    make_packet<ImagePacket>(buffer_copy, options->width(), options->height(), ec);
  if (ec != ErrorCode::OK) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "Issue creating NV12 image packet");
    return GST_FLOW_ERROR;
  }
  timestamp_t this_sts        = nv12_img_pkt->get_pres_timestamp() + sts_pts_offset;
  timestamp_t this_sts_now_us = get_now_us();
  if (this_sts <= last_sts) {
    timestamp_t sts_pts_offset_change =
      ((this_sts_now_us - last_sts_now_us) / frame_distance_us) * frame_distance_us;
    sts_pts_offset += sts_pts_offset_change;
    this_sts += sts_pts_offset_change;
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_WARN,
                      AUP_AVAF_TERM_COLOR_FG_MAGENTA
                      "PTS value dropped. Decoder will increase PTS offset value "
                      "accordingly" AUP_AVAF_TERM_FORMAT_RESET_ALL);
  }
  if (this_sts <= last_sts) {
    timestamp_t sts_pts_offset_change = last_sts - this_sts + frame_distance_us;
    sts_pts_offset += sts_pts_offset_change;
    this_sts += sts_pts_offset_change;
  }
  nv12_img_pkt->set_sync_timestamp(this_sts);
  bgr_img_pkt->set_sync_timestamp(this_sts);
  bgr_img_pkt->set_pres_timestamp(nv12_img_pkt->get_pres_timestamp());
  last_sts = this_sts;

  if (node->get_graph_status() != GraphStatus::RUNNING) {
    return GST_FLOW_OK;
  }
  auto& bgr_cv_mat = bgr_img_pkt->get_cv_mat();

  cv::Mat yplane, uvplane;
  nv12_img_pkt->get_yplane_nv12_cvmat(yplane);
  nv12_img_pkt->get_uvplane_nv12_cvmat(uvplane);
  cv::cvtColorTwoPlane(yplane, uvplane, bgr_cv_mat, cv::COLOR_YUV2BGR_NV12);
  if ((ec = node->enqueue(0, bgr_img_pkt)) != ErrorCode::OK) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_FATAL,
                      "Issue enqueueing BGR24 Image Packet " << ec);
    return GST_FLOW_ERROR;
  }
  if ((ec = node->enqueue(2, nv12_img_pkt)) != ErrorCode::OK) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_FATAL,
                      "Issue enqueueing NV12 Image Packet " << ec);
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static GstFlowReturn new_sample_gl(GstAppSink* appsink, gpointer gp_video_source_calculator)
{
  VideoSourceCalculator* video_source_calculator =
    static_cast<VideoSourceCalculator*>(gp_video_source_calculator);
  return video_source_calculator->new_sample(appsink);
}

static GstFlowReturn new_sample_2outputs_gl(GstAppSink* appsink,
                                            gpointer gp_video_source_calculator)
{
  VideoSourceCalculator* video_source_calculator =
    static_cast<VideoSourceCalculator*>(gp_video_source_calculator);
  return video_source_calculator->new_sample_2outputs(appsink);
}

ErrorCode VideoSourceCalculator::initialize_rtsp_file(std::string& err_str)
{
  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "final options: " << options->DebugString());
  video_stream_info                        = make_packet<VideoStreamInfoPacket>();
  video_stream_info->iframe_extract        = false;
  video_stream_info->w                     = options->width();
  video_stream_info->h                     = options->height();
  video_stream_info->max_bframes           = true;
  video_stream_info->codec_type            = options->codec_type();
  video_stream_info->framerate_numerator   = options->framerate_numerator();
  video_stream_info->framerate_denominator = options->framerate_denominator();
  video_stream_info->fps =
    (float)video_stream_info->framerate_numerator / (float)video_stream_info->framerate_denominator;
  video_stream_info->pixfmt = PIXFMT_BGR24;
  ErrorCode ec              = ErrorCode::OK;
  if ((ec = node->enqueue(1, video_stream_info)) != ErrorCode::OK) {
    err_str += "Could not send out video stream info side packet. ";
    return ec;
  }
  if (node->output_streams.size() == 4) {
    video_stream_info_nv12                        = make_packet<VideoStreamInfoPacket>();
    video_stream_info_nv12->iframe_extract        = false;
    video_stream_info_nv12->w                     = options->width();
    video_stream_info_nv12->h                     = options->height();
    video_stream_info_nv12->max_bframes           = true;
    video_stream_info_nv12->codec_type            = CODEC_TYPE_H265;
    video_stream_info_nv12->framerate_numerator   = options->framerate_numerator();
    video_stream_info_nv12->framerate_denominator = options->framerate_denominator();
    video_stream_info_nv12->fps = (float)video_stream_info_nv12->framerate_numerator /
                                  (float)video_stream_info_nv12->framerate_denominator;
    video_stream_info_nv12->pixfmt = PIXFMT_NV12;

    if ((ec = node->enqueue(3, video_stream_info_nv12)) != ErrorCode::OK) {
      err_str = "image_stream_initialize did not receive input stream info. code: " + to_string(ec);
      return ec;
    }
  }
  frame_distance_us =
    (int)(1'000'000 * options->framerate_numerator() / options->framerate_denominator());
  rtsp_video_capture_thread = thread([&] { this->rtsp_file_video_capture_worker(); });

  return ErrorCode::OK;
}

void VideoSourceCalculator::rtsp_file_video_capture_worker()
{
  gst_init(NULL, NULL);
  string codec_str;
  switch (options->codec_type()) {
    case CODEC_TYPE_H264:
      codec_str = "h264";
      break;
    case CODEC_TYPE_H265:
      codec_str = "h265";
      break;
    default:
      AUP_AVAF_RUNTIME_ERROR("Invalid codec value " + CodecType_Name(options->codec_type()));
  }
  stringstream pipeline_ss;
  if (options->source_type() == VideoSourceOptions::RTSP) {
    pipeline_ss << "rtspsrc location=\"" << options->path() << "\" ! ";
    pipeline_ss << "queue ! rtp" << codec_str << "depay ! queue";
  } else {
    pipeline_ss << (options->play_file_once() ? "filesrc" : "multifilesrc");
    pipeline_ss << " location=" << options->path();
  }
  pipeline_ss << " ! " << codec_str << "parse ! ";
  pipeline_ss << "queue ! omx" << codec_str << "dec ! video/x-raw, width=" << options->width();
  pipeline_ss << ", height=" << options->height()
              << ", format=NV12, framerate=" << options->framerate_numerator() << "/"
              << options->framerate_denominator() << " ! appsink name=nv12_frame_sink";
  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "decoder pipeline: " << pipeline_ss.str());
  GstElement* pipeline = gst_parse_launch(pipeline_ss.str().c_str(), NULL);
  GstElement* appsink  = gst_bin_get_by_name(GST_BIN(pipeline), "nv12_frame_sink");
  g_object_set(G_OBJECT(appsink), "emit-signals", TRUE, "sync", FALSE, NULL);
  g_signal_connect(
    appsink, "new-sample",
    G_CALLBACK(node->output_streams.size() == 4 ? new_sample_2outputs_gl : new_sample_gl), this);

  ErrorCode ec = ErrorCode::OK;
  allocator    = ImagePacket::Allocator::new_normal_allocator(
    options->width(), options->height(), PIXFMT_BGR24, options->pool_size() ?: 12, ec);
  if (ec != ErrorCode::OK) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "issue instatiating allocator for video stream: " << ec);
    return;
  }
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  running = true;

  do {
    using namespace std::chrono_literals;
    this_thread::sleep_for(100us);
  } while (running);

  // Cleanup
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(pipeline));
}

ErrorCode VideoSourceCalculator::initialize(std::string& err_str)
{
  if (options->path().empty() && !node->get_input_url().empty()) {
    options->set_path(node->get_input_url());
  }
  auto ec = update_and_validate_options(err_str);
  if (ec != ErrorCode::OK) {
    return ec;
  }

  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "Video source options:\n"
                      << options->DebugString());

  switch (options->source_type()) {
    case VideoSourceOptions::USB:
      return initialize_usbcam(err_str);
    case VideoSourceOptions::RTSP:
    case VideoSourceOptions::FILE:
      return initialize_rtsp_file(err_str);
    default:
      err_str = "Unhandled source_type";
      return ErrorCode::ERROR;
  }
  return ErrorCode::ERROR;
}

AUP_AVAF_REGISTER_CALCULATOR_EXT("Aupera", "video_source", VideoSourceCalculator,
                                 VideoSourceOptions, false, "Aupera's video source calculator.", {})
