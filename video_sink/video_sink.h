#pragma once

// std headers
#include <atomic>
#include <regex>
#include <thread>
#include <unistd.h>

// gst headers
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

// sdk headers (ffmpeg)
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// avaf headers
#include "aup/avaf/calculator.h"
#include "aup/avaf/node.h"
#include "aup/avaf/packets/image_packet.h"
#include "aup/avaf/packets/video_stream_info_packet.h"
#include "aup/avaf/utils.h"

// avap headers
#include "aup/avap/video_sink.pb.h"

// local headers
#include "rtsp_server.h"

using namespace aup::avaf;
using namespace std;

int parse_rtsp_url(string url, string& domain, int& port, string& extension, string& err_str);

template <class T>
class GSTThreadSafeQueue
{
private:
  queue<T> q;
  mutable mutex m;
  unsigned int size;

public:
  GSTThreadSafeQueue(int queue_size = 10) : q(), m(), size(queue_size) {}
  ~GSTThreadSafeQueue(void) {}

  void enqueue(T t)
  {
    lock_guard<mutex> lock(m);
    if (q.size() >= size) {
      T val = q.front();
      std::cout << __func__ << ": Max queue size " << size << " reached, dropping entry" << std::endl;
      q.pop();
    }
    q.push(t);
  }

  bool dequeue(T& val)
  {
    unique_lock<mutex> lock(m);
    if (q.empty()) {
      return false;
    }
    val = q.front();
    q.pop();
    return true;
  }

  int get_size(void)
  {
    unique_lock<mutex> lock(m);
    auto size_ = q.size();
    return int(size_);
  }
};

struct VideoSinkGSTOptions
{
  string out_type;
  string rate_control_mode;
  guint target_bitrate;
  guint gop_length;
  string gop_mode;
  guint bframes;
  guint img_width;
  guint img_height;
  guint udp_port;
  guint framerate_numerator;
  guint framerate_denominator;
};

class VideoSinkGST
{
private:
  struct GSTData
  {
    GstElement* pipeline;
    GstElement* appsrc;
    GSTThreadSafeQueue<PacketPtr<const ImagePacket>>* thread_in_q = NULL;
    VideoSinkGSTOptions* options;
    const Node* node;
    std::atomic<bool> running = false;
  };

  GSTData data;

  static void push_data(GstElement* appsrc, guint size, GSTData* data);

public:
  VideoSinkGST(VideoSinkGSTOptions* videosinkoptions,
               GSTThreadSafeQueue<PacketPtr<const ImagePacket>>* in_queue, const Node* node)
  {
    data.thread_in_q = in_queue;
    data.options     = videosinkoptions;
    data.node        = node;
  }
  ~VideoSinkGST();
  ErrorCode initialize(string& err_str);
};

class UDPtoRTSP
{
private:
  guint udp_port;
  std::string out_type;
  std::string out_url;
  GstElement* pipeline = NULL;
  const Node* node     = NULL;

public:
  UDPtoRTSP(guint port, std::string type, std::string url, const Node* node)
    : udp_port(port), out_type(type), out_url(url), node(node){};
  ~UDPtoRTSP();
  int initialize(std::string& err_str);
};

class VideoSinkCalculator : public CalculatorBase<VideoSinkOptions>
{
  bool need_i_img_stream_info_side_packet = true;
  PacketPtr<const VideoStreamInfoPacket> i_img_stream_info;
  uint32_t send_img_packet_cnt          = 0;
  VideoSinkGST* gst_instance            = NULL;
  UDPtoRTSP* rtsp_instance              = NULL;
  UDPToRTSPServer* rtsp_server_instance = NULL;
  GSTThreadSafeQueue<PacketPtr<const ImagePacket>> buffer_queue;
  bool use_local_server = true;
  int obj_id;

protected:
  ErrorCode fill_contract(shared_ptr<Contract>& contract, string& err_str) override;
  static std::atomic<int> id_counter; // to give each object instance a unique udp port

public:
  VideoSinkCalculator(const Node* node) : CalculatorBase(node), obj_id(++id_counter) {}
  virtual ~VideoSinkCalculator()
  {
    delete gst_instance;
    gst_instance = NULL;
    delete rtsp_instance;
    rtsp_instance = NULL;
    delete rtsp_server_instance;
    rtsp_server_instance = NULL;
  };
  aup::avaf::ErrorCode initialize(string& err_str) override;
  ErrorCode execute() override;
};
