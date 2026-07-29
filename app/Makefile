PROG      = $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
SRCS      = object_dwell_timer.c zone.c tracker.c events.c httpd.c
OBJS      = $(SRCS:.c=.o)
DEBUG_DIR = debug

PKGS = device-data-hub-client-c jansson glib-2.0 gio-2.0 axevent

CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS)) -lm

CFLAGS += -Wall \
          -Wextra \
          -Wpedantic \
          -Wmissing-prototypes \
          -Wstrict-prototypes \
          -Wfloat-equal \
          -Wbad-function-cast \
          -Wstack-protector \
          -Wpointer-arith \
          -Werror \
          -std=c17

all: $(PROG)

%.o: %.c
	$(CC) -c $< $(CFLAGS) -o $@

$(PROG): $(OBJS)
	install -d $(DEBUG_DIR)
	$(CC) $^ $(CFLAGS) $(LDFLAGS) $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

clean:
	rm -rf $(PROG) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
