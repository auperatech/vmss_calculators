#pragma once

#include <thread>
#include <unistd.h>

// gst headers
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <opencv2/opencv.hpp>

template <class T>
class GSTThreadSafeQueue
{
private:
  std::queue<T> q;
  mutable std::mutex m;

public:
  GSTThreadSafeQueue(void) : q(), m() {}
  ~GSTThreadSafeQueue(void) {}

  void enqueue(T t)
  {
    std::lock_guard<std::mutex> lock(m);
    q.push(t);
  }

  bool dequeue(T& val)
  {
    std::unique_lock<std::mutex> lock(m);
    if (q.empty()) {
      return false;
    }
    val = q.front();
    q.pop();
    return true;
  }

  int get_size(void)
  {
    std::unique_lock<std::mutex> lock(m);
    auto size_ = q.size();
    return int(size_);
  }
};

// struct RawFrameData
// {
//   guchar *raw_data = NULL;
//   GstClockTime pts;
//   GstClockTime duration;
//   gsize buff_size;
// };

struct VideoSinkGSTOptions
{
  std::string out_type;
  std::string rate_control_mode;
  guint target_bitrate;
  guint gop_length;
  std::string gop_mode;
  guint bframes;
  guint img_width;
  guint img_height;
  guint udp_port;
};

class VideoSinkGST
{
private:
  typedef struct _GSTData
  {
    GstElement* pipeline;
    GstElement* appsrc;
    GSTThreadSafeQueue<GstBuffer*>* thread_in_q = NULL;
    VideoSinkGSTOptions* options;
  } GSTData;

  GSTData data;

  static void push_data(GstElement* appsrc, guint size, GSTData* data);

public:
  VideoSinkGST(VideoSinkGSTOptions* videosinkoptions, GSTThreadSafeQueue<GstBuffer*>* in_queue) { data.thread_in_q = in_queue; data.options = videosinkoptions; }
  ~VideoSinkGST();
  void initialize(std::string& err_str);
};

class UDPRTSP {
private:
  guint udp_port = 5000;
  std::string out_type = "h264";
  GstElement* pipeline = NULL;
public:
  ~UDPRTSP();
  int initialize(std::string& err_str);
};