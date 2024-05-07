CALCULATOR = notification_web
VENDOR = aupera
include VERSION.mk

OBJS = ../apl_common/utils.o ../apl_common/metadata_map.o
INCLUDE_SEARCH_DIRS += -I../apl_common -I../../avas/dependencies/cpp-httplib/
LDLIBS += -lssl -lcrypto

include $(STAGING_DIR)/opt/aupera/make/Calculator.mk