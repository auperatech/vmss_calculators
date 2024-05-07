CALCULATOR = video_sink
VENDOR = aupera
include VERSION.mk

OBJS += rtsp_server.o

CXXFLAGS += $(shell pkg-config --cflags-only-other --libs-only-other opencv4 gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0)
LDLIBS += $(shell pkg-config --libs-only-l opencv4 gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0)
LIB_SEARCH_DIRS += $(shell pkg-config --libs-only-L opencv4 gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0)
INCLUDE_SEARCH_DIRS += $(shell pkg-config --cflags-only-I opencv4 gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0)

include $(STAGING_DIR)/opt/aupera/make/Calculator.mk