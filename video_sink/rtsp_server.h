#pragma once

#include <gst/gst.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <gst/rtsp-server/rtsp-server.h>
#pragma GCC diagnostic pop

#include <gst/rtsp-server/rtsp-server.h>

std::string exec(const char* cmd);
std::vector<std::string> GetIp();

struct UDPToRTSPServerOptions
{
  std::string out_type;
  guint udp_port; // multiple servers cannot have the same udp port
  guint out_port;
  std::string stream_name;
};

class UDPToRTSPServer
{
private:
  typedef struct _RTSPServerData
  {
    GMainContext* contxt;
    GMainLoop* main_loop;
    UDPToRTSPServerOptions* options;
  } RTSPServerData;

  RTSPServerData data;
  std::thread worker_thread;

  static void clientConnected(GstRTSPServer* server, GstRTSPClient* client, gpointer* ptr);
  static void worker(RTSPServerData* data);

public:
  // TODO validate the rtsp sever options inside the constructor
  UDPToRTSPServer(UDPToRTSPServerOptions* rtsp_server_options)
  {
    data.options = rtsp_server_options;
  }
  ~UDPToRTSPServer();
  int initialize(std::string& err_str);
};