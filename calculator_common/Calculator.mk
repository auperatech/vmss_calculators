VERSION = $(AUP_AVAF_CALC_VERSION_MAJOR).$(AUP_AVAF_CALC_VERSION_MINOR).$(AUP_AVAF_CALC_VERSION_PATCH)

LDLIBS += -lavaf
include ../../CommonVars.mk
CXXFLAGS += -I../box_common -I../../dependencies/include
LIB_SEARCH_DIRS += -L../box_common -L../../dependencies/lib

OBJS += $(CALCULATOR).o

all: $(CALCULATOR)

$(CALCULATOR):
	$(MAKE) $(OBJS)
	$(CXX) $(CXXFLAGS) $(WARNFLAGS) $(INCLUDE_SEARCH_DIRS) -shared $(OBJS) \
	       -o lib$(VENDOR).$(CALCULATOR).calculator.$(VERSION) $(LIB_SEARCH_DIRS) $(LDLIBS)

%.o: %.cc
	$(CXX) -c $(CXXFLAGS) $(WARNFLAGS) $(INCLUDE_SEARCH_DIRS) -o $@ $^

install: 
	cp lib$(VENDOR).$(CALCULATOR).calculator.$(VERSION) $(STAGING_DIR)$(INSTALL_PREFIX)/lib
	ln -sfr $(STAGING_DIR)$(INSTALL_PREFIX)/lib/lib$(VENDOR).$(CALCULATOR).calculator.$(VERSION) \
	        $(STAGING_DIR)$(INSTALL_PREFIX)/lib/lib$(VENDOR).$(CALCULATOR).calculator

clean:
	rm -f lib$(VENDOR).$(CALCULATOR).calculator* $(OBJS)

uninstall:
	rm -f $(STAGING_DIR)$(INSTALL_PREFIX)/lib/lib$(VENDOR).$(CALCULATOR).calculator \
	      $(STAGING_DIR)$(INSTALL_PREFIX)/lib/lib$(VENDOR).$(CALCULATOR).calculator.$(VERSION)
