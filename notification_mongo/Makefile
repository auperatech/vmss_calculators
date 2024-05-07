CALCULATOR = notification_mongo
VENDOR = aupera
include VERSION.mk

INCLUDE_SEARCH_DIRS += -I/usr/local/include/libmongoc-1.0/ -I/usr/local/include/libbson-1.0
LDLIBS = -lmongoc-1.0 -lbson-1.0

include $(STAGING_DIR)/opt/aupera/make/Calculator.mk