/* GStreamer
 * Copyright(C) 2008 Wim Taymans <wim.taymans at gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or(at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Modified by Matthew Wiens <matthew.wiens@auperatech.com>
 */

#include "rtsp_server.h"

std::string exec(const char* cmd)
{
  std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
  if (!pipe)
    return "ERROR";
  char buffer[128];
  std::string result = "";
  while (!feof(pipe.get())) {
    if (fgets(buffer, 128, pipe.get()) != NULL)
      result += buffer;
  }
  return result;
}

std::vector<std::string> GetIp()
{
  std::string s = exec("ifconfig | grep 'inet ' | sed 's/.*inet *\\([^ ]*\\).*/\\1/'");

  std::vector<std::string> rarray;
  std::size_t pos;
  while ((pos = s.find("\n")) != std::string::npos) {
    std::string token = s.substr(0, pos);
    if (token != "127.0.0.1") {
      rarray.push_back(token);
    }
    s.erase(0, pos + std::string("\n").length());
  }

  return rarray;
}

void UDPToRTSPServer::clientConnected(GstRTSPServer*, GstRTSPClient*, gpointer*)
{
  std::cout << "RTSP SERVER: Client connected to stream" << std::endl;
}

int UDPToRTSPServer::initialize(std::string& err_str)
{
  if (!data.options) {
    err_str = "Could not initialize UDPToRTSPServer: options are NULL, are you sure you passed "
              "those to the constructor?";
    return 1;
  }
  GstRTSPServer* server;
  GstRTSPMountPoints* mounts;
  GstRTSPMediaFactory* factory;

  gst_init(NULL, NULL);

  std::stringstream pipeline_ss;
  pipeline_ss << "( ";
  pipeline_ss << "udpsrc port=" << data.options->udp_port << " ";
  pipeline_ss << "caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, ";
  if (data.options->out_type == "h264") {
    pipeline_ss << "encoding-name=(string)H264\" ";
  } else {
    err_str = "unknown encoding type " + data.options->out_type;
    return 1;
  }
  pipeline_ss << "! rtp" << data.options->out_type << "depay ";
  pipeline_ss << "! rtp" << data.options->out_type << "pay name=pay0 ";
  pipeline_ss << ")";
  std::cout << "============ RTSP SERVER PIPELINE ===========\n" << pipeline_ss.str() << std::endl;

  data.contxt    = g_main_context_new();
  data.main_loop = g_main_loop_new(data.contxt, FALSE);

  server = gst_rtsp_server_new();
  g_object_set(server, "service", std::to_string(data.options->out_port).c_str(), NULL);

  mounts  = gst_rtsp_server_get_mount_points(server);
  factory = gst_rtsp_media_factory_new();
  gst_rtsp_media_factory_set_launch(factory, pipeline_ss.str().c_str());
  gst_rtsp_media_factory_set_shared(factory, TRUE);

  std::string extension = "/" + data.options->stream_name;
  gst_rtsp_mount_points_add_factory(mounts, extension.c_str(), factory);
  g_object_unref(mounts);

  if (!gst_rtsp_server_attach(server, data.contxt)) {
    err_str = "Issue attaching RTSP server";
    g_main_loop_unref(data.main_loop);
    return 1;
  }

  g_signal_connect(server, "client-connected", G_CALLBACK(clientConnected), NULL);

  worker_thread = std::thread(UDPToRTSPServer::worker, &data);

  /* start serving */
  std::vector<std::string> ips = GetIp();
  std::ostringstream addr("");
  for (auto& ip : ips) {
    addr << "rtsp://" << ip << ":" << data.options->out_port << "/" << data.options->stream_name << "\n";
  }
  g_print("stream ready at:\n%s", addr.str().c_str());

  return 0;
}

void UDPToRTSPServer::worker(RTSPServerData* data)
{
  g_main_context_push_thread_default(data->contxt);
  std::cout << "Starting main loop" << std::endl;
  g_main_loop_run(data->main_loop);
  std::cout << "Finished main loop" << std::endl;
  g_main_context_pop_thread_default(data->contxt);
}

UDPToRTSPServer::~UDPToRTSPServer()
{
  std::cout << "Stopping main loop" << std::endl;
  g_main_loop_quit(data.main_loop);

  do {
    try {
      (worker_thread).join();
    } catch (const ::std::system_error& e) {
      if (e.code() != std::errc::invalid_argument) {
        std::cerr << "thread:" << __FUNCTION__ << ":" << __LINE__
                  << " - exception would have caused terminate.";
      }
    }
  } while (0);

  std::cout << "Removing references to main loop and context" << std::endl;
  g_main_loop_unref(data.main_loop);
  g_main_context_unref(data.contxt);

  delete data.options;
  data.options = NULL;
}