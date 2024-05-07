## Encoder test

Build with:

cd calculators/video_sink/test/
g++ -o enc_test enc_test.cc video_sink_gst.cc `pkg-config --cflags --libs opencv4 gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0`

Run with:
GST_DEBUG=2 ./enc_test

## Decoder test

Build with:

cd calculators/video_sink/test/
g++ -o dec_test dec_test.cc `pkg-config --cflags --libs opencv4 gstreamer-1.0 gstreamer-app-1.0`

Run with:
GST_DEBUG=2 ./dec_test
