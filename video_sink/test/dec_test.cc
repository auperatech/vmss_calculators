#include <chrono>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <opencv2/opencv.hpp>

// Function to convert a GstSample to a cv::Mat
cv::Mat gst_sample_to_cvmat(GstSample *sample) {
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map;
  gst_buffer_map(buffer, &map, GST_MAP_READ);

  GstCaps *caps = gst_sample_get_caps(sample);
  GstStructure *capsStruct = gst_caps_get_structure(caps, 0);

  int width, height;
  gst_structure_get_int(capsStruct, "width", &width);
  gst_structure_get_int(capsStruct, "height", &height);

  cv::Mat mat(height + height / 2, width, CV_8UC1, (char *)map.data);
  cv::Mat rgbMat;
  cv::cvtColor(mat, rgbMat, cv::COLOR_YUV2BGR_NV12);

  gst_buffer_unmap(buffer, &map);

  return rgbMat;
}

// The callback function
static GstFlowReturn new_sample(GstAppSink *appsink, gpointer data) {
  GstSample *sample = gst_app_sink_pull_sample(appsink);
  static unsigned int frame_no = 1;
  auto start = std::chrono::high_resolution_clock::now();
  // Path 1: software color conversion from NV12
  //   cv::Mat frame = gst_sample_to_cvmat(sample);
  // Path 2: color conversion from hardware?????
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map;
  gst_buffer_map(buffer, &map, GST_MAP_READ);
  int width = 1920;
  int height = 1080;
  cv::Mat y_plane(cv::Size(width, height), CV_8UC1, map.data);
  cv::Mat uv_plane(cv::Size(width / 2, height / 2), CV_8UC2,
                   map.data + width * height);
  cv::Mat frame;
  cv::cvtColorTwoPlane(y_plane, uv_plane, frame, cv::COLOR_YUV2BGR_NV12);

  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  std::cout << "Time taken for frame" << frame_no << " : " << duration.count()
            << " microseconds" << std::endl;
  if (frame_no >= 100 && frame_no <= 150) {
    std::string filename = "/tmp/frame_" + std::to_string(frame_no) + ".jpg";
    cv::imwrite(filename, frame);
  }
  frame_no++;
  gst_buffer_unmap(buffer, &map);
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);

  // Build the pipeline
  // clang-format off
  GstElement *pipeline = gst_parse_launch(
      "rtspsrc location=\"rtsp://avac.auperatechnologies.com:554/car\" ! queue "
      "! rtph264depay ! queue ! h264parse ! queue ! omxh264dec ! video/x-raw, "
      "width=1920, height=1080, format=NV12 , framerate=30/1 ! appsink "
      "name=nv12_app_sink",
      NULL);
  // clang-format on

  // Get the appsink element
  GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "nv12_app_sink");
  g_object_set(G_OBJECT(appsink), "emit-signals", TRUE, "sync", FALSE, NULL);

  // Connect the callback
  g_signal_connect(appsink, "new-sample", G_CALLBACK(new_sample), NULL);

  // Start playing
  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  // Enter the GStreamer main loop
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);

  // Cleanup
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(pipeline));
  g_main_loop_unref(loop);

  return 0;
}