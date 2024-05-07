// stl headers
#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <filesystem>
#include <iostream>
#include <thread>
#include <tuple>
#include <vector>

// sdk headers
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/opencv.hpp>

// avaf headers
#include "aup/avaf/calculator.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/image_packet.h"
#include "aup/avaf/packets/video_stream_info_packet.h"
#include "aup/avaf/thread_name.h"
#include "aup/avaf/utils.h"
#include "aup/avap/image_stream.pb.h"

// #define AUP_AVAF_DBG_ENABLE (1)
#include "aup/avaf/avaf_dbg.h"

using namespace std;
using namespace cv;
using namespace aup::avaf;
namespace fs = std::filesystem;

class ImageStreamCalculator : public CalculatorBase<ImageStreamOptions>
{
  PacketPtr<VideoStreamInfoPacket> video_stream_info;
  thread io_service_thread;
  void io_service_worker()
  {
    AUP_AVAF_HANDLE_THREAD_NAME_EXT(node->get_calculator_name());
    io_service.run();
  };
  boost::asio::io_service io_service;
  int64_t interval_int;
  boost::posix_time::microseconds interval;
  unique_ptr<boost::asio::deadline_timer> timer;
  bool running = false;
  vector<tuple<fs::path, Mat>> image_paths;
  decltype(image_paths)::const_iterator image_path_itr;
  uint64_t next_pts = 12345678;
  void send_frame();
  int overall_count = 0;
  float fps;
  uint32_t width;
  uint32_t height;
  shared_ptr<ImagePacket::Allocator> allocator;

protected:
  ErrorCode fill_contract(std::shared_ptr<Contract>& contract, std::string& err_str) override;

public:
  ImageStreamCalculator(const ::aup::avaf::Node* node) : CalculatorBase(node), interval(0) {}
  ~ImageStreamCalculator();
  ErrorCode initialize(std::string& err_str) override;
};

ImageStreamCalculator::~ImageStreamCalculator()
{
  io_service.stop();
  running = false;
  AUP_AVAF_THREAD_JOIN_NOTERM(io_service_thread);
}

ErrorCode ImageStreamCalculator::fill_contract(shared_ptr<Contract>& contract, string& err_str)
{
  if (contract->input_stream_names.size() != 0) {
    err_str = "node cannot have any inputs";
    return ErrorCode::INVALID_CONTRACT;
  }
  uint32_t sz_output = (uint32_t)contract->output_stream_names.size();
  if (sz_output != 2) {
    err_str = "node has exactly two outputs";
    return ErrorCode::INVALID_CONTRACT;
  }
  contract->sample_output_packets[0] = make_packet<ImagePacket>();
  contract->sample_output_packets[1] = make_packet<VideoStreamInfoPacket>();
  return ErrorCode::OK;
}

ErrorCode ImageStreamCalculator::initialize(std::string& err_str)
{
  this->node = node;
  options    = make_unique<ImageStreamOptions>();
  for (const ::google::protobuf::Any& object : node->options) {
    if (object.Is<ImageStreamOptions>()) {
      if (!object.UnpackTo(options.get())) {
        AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                          "\033[33m" << __func__
                                     << "unable to unpack options of image stream.\033[0m");
        return ErrorCode::ERROR;
      }
    } else {
      AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                        "\033[33m" << __func__ << "options not match with image stream.\033[0m");
      return ErrorCode::ERROR;
    }
  }
  if (!options->width() || !options->height() || !options->playback_fps()) {
    AUP_AVAF_LOG_NODE(
      node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
      "\033[33m" << __func__ << "image stream node should define positive width, height and fps."
                 << "\033[0m");
  }

  width        = options->width();
  height       = options->height();
  fps          = options->playback_fps();
  interval_int = (int64_t)(1'000'000.0 / fps);
  interval     = boost::posix_time::microseconds((boost::uint64_t)(interval_int));
  ErrorCode ec;
  allocator = ImagePacket::Allocator::new_normal_allocator(width, height, PIXFMT_BGR24,
                                                           options->pool_size() ?: 12, ec);
  if (ec != ErrorCode::OK) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "\033[33m" << __func__
                                 << "issue instatiating allocator for video stream.\033[0m");
    return ec;
  }
  if (options->vooya_input()) {
    options->set_preload_images(false);
  }
  for (const auto& file : fs::directory_iterator(options->directory())) {
    if (!file.is_regular_file()) {
      continue;
    }
    if (options->vooya_input()) {
      ErrorCode ec;
      auto img_pkt = make_packet<ImagePacket>(0, false, allocator, ec);
      if ((uint32_t)img_pkt->get_raw_data_sz() != (uint32_t)file.file_size()) {
        continue;
      }
    } else {
      if (boost::algorithm::to_lower_copy(file.path().extension().string()) != ".jpg" &&
          boost::algorithm::to_lower_copy(file.path().extension().string()) != ".jpeg" &&
          boost::algorithm::to_lower_copy(file.path().extension().string()) != ".png") {
        continue;
      }
    }
    Mat cvmat;
    if (options->preload_images()) {
      auto cvmat_read = imread(file.path().string(), IMREAD_COLOR);
      AUP_AVAF_DBG("is cvmat empty:" << cvmat_read.empty() << " h:" << cvmat_read.size().height
                                     << " w:" << cvmat_read.size().width
                                     << " data:" << (void*)cvmat_read.data);
      resize(cvmat_read, cvmat, Size(width, height), 0, 0, INTER_CUBIC);
    }
    image_paths.push_back(make_tuple(file.path(), cvmat));
  }
  if (image_paths.empty()) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "\033[33m" << __func__
                                 << "image stream node didnt find any images in the directory."
                                 << "\033[0m");
    return ErrorCode::ERROR;
  }
  sort(image_paths.begin(), image_paths.end(),
       [](const tuple<fs::path, Mat>& lhs, const tuple<fs::path, Mat>& rhs) {
         return get<0>(lhs).string() < get<0>(rhs).string();
       });

  allocator = ImagePacket::Allocator::new_normal_allocator(
    options->width(), options->height(), PIXFMT_BGR24, options->pool_size() ?: 12, ec);
  if (ec != ErrorCode::OK) {
    AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_ERROR,
                      "\033[33m" << __func__
                                 << "issue instatiating allocator for video stream.\033[0m");
    return ec;
  }
  video_stream_info                 = make_packet<VideoStreamInfoPacket>();
  video_stream_info->iframe_extract = false;
  video_stream_info->w              = options->width();
  video_stream_info->h              = options->height();
  video_stream_info->max_bframes    = true;
  video_stream_info->codec_type     = CODEC_TYPE_NONE;
  video_stream_info->fps            = fps;
  video_stream_info->pixfmt         = PIXFMT_BGR24;

  if ((ec = node->enqueue(1, video_stream_info)) != ErrorCode::OK) {
    err_str = "image_stream_initialize did not receive input stream info. code: " + to_string(ec);
    return ec;
  }

  timer          = make_unique<boost::asio::deadline_timer>(io_service, interval);
  image_path_itr = image_paths.begin();
  timer->async_wait(boost::bind(&ImageStreamCalculator::send_frame, this));
  running           = true;
  io_service_thread = thread([this] { io_service_worker(); });
  return ErrorCode::OK;
}

void ImageStreamCalculator::send_frame()
{
  auto graph_status = node->get_graph_status();
  if (graph_status == GraphStatus::FAILED) {
    return;
  }
  if (graph_status == GraphStatus::FINISHED) {
    return;
  }
  auto reset_timer = [&]() {
    if (!running) {
      return;
    }
    timer->expires_at(timer->expires_at() + interval);
    timer->async_wait(boost::bind(&ImageStreamCalculator::send_frame, this));
  };
  if (graph_status != GraphStatus::RUNNING) {
    reset_timer();
    return;
  }
  next_pts += interval_int;
  AUP_AVAF_LOG_NODE(node, GraphConfig::LoggingFilter::SEVERITY_INFO,
                    "\033[33m" << __func__ << "image stream sending frame "
                               << to_string(overall_count) << "\033[0m");
  PacketPtr<ImagePacket> image_packet = nullptr;
  //
  auto fill_img_packet = [&](const Mat& cvmat) {
    AUP_AVAF_TRACE();
    AUP_AVAF_DBG("is cvmat empty:" << cvmat.empty() << " h:" << cvmat.size().height
                                   << " w:" << cvmat.size().width << " data:" << (void*)cvmat.data);
    if ((int)cvmat.rows == (int)height && (int)cvmat.cols == (int)width) {
      AUP_AVAF_TRACE();
      memcpy(image_packet->get_raw_data(), cvmat.data, image_packet->get_raw_data_sz());
      AUP_AVAF_TRACE();
    } else {
      Mat cvmat_resized;
      resize(cvmat, cvmat_resized, Size(width, height), INTER_LINEAR);
      memcpy(image_packet->get_raw_data(), cvmat_resized.data, image_packet->get_raw_data_sz());
    }
  };
  do {
    ErrorCode ec;
    image_packet = make_packet<ImagePacket>(next_pts, false, allocator, ec);
    if (ec == ErrorCode::OK) {
      image_packet->set_fps(fps);
      break;
    }
    usleep(100);
    graph_status = node->get_graph_status();
  } while (graph_status != GraphStatus::FAILED && graph_status != GraphStatus::FINISHED);
  image_packet->set_pres_timestamp(next_pts);
  if (options->vooya_input()) {
    ifstream image_file_ss(get<0>(*image_path_itr).string(), ios::binary);
    image_file_ss.read((char*)image_packet->get_raw_data(), image_packet->get_raw_data_sz());
  } else if (!options->preload_images()) { // if jpeg or png
    auto cvmat = imread(get<0>(*image_path_itr).string(), IMREAD_COLOR);
    fill_img_packet(cvmat);
  } else {
    AUP_AVAF_DBG("file:" << get<0>(*image_path_itr).string());
    fill_img_packet(get<1>(*image_path_itr));
  }
  AUP_AVAF_DBG_NODE(node, "Enqueue packet with sts:" << image_packet->get_sync_timestamp());
  node->enqueue(0, image_packet);
  overall_count++;
  if (++image_path_itr == image_paths.end()) {
    AUP_AVAF_TRACE();
    if (options->loop_over()) {
      AUP_AVAF_TRACE();
      image_path_itr = image_paths.begin();
    } else {
      return;
    }
  }
  reset_timer();
}

AUP_AVAF_REGISTER_CALCULATOR_EXT("Aupera", "image_stream", ImageStreamCalculator,
                                 ImageStreamOptions, false, "Aupera's image stream calculator.", {})
