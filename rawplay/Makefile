# the host side of the livi stack. weston-touch.so is the weston module that adds a touch
# device (and injects the panel keys) the raw sender can drive, so the app gets real touch
# instead of an x11 mouse. needs weston-devel (fedora: dnf install weston-devel,
# postmarketos: apk add weston-dev). the newest installed libweston is picked; the major is
# passed to the source because weston 16 changed the touch and keyboard apis (see the
# comments in weston-touch.c).
WESTON_PC := $(shell pkg-config --list-all 2>/dev/null | awk '/^libweston-[0-9]+ /{print $$1}' | sort | tail -1)
ifeq ($(WESTON_PC),)
WESTON_PC := libweston-14
endif
WESTON_MAJOR := $(patsubst libweston-%,%,$(WESTON_PC))

CFLAGS := -O2 -Wall -Wextra -fPIC -DLIVI_WESTON_MAJOR=$(WESTON_MAJOR) \
          $(shell pkg-config --cflags $(WESTON_PC) wayland-server)
LDLIBS := $(shell pkg-config --libs $(WESTON_PC) wayland-server)

all: weston-touch.so

weston-touch.so: weston-touch.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

# rawlink is the host: the functionfs gadget and the raw sender in one c binary
# (stream/gadget/run); the python hosts (rawstream.py, rawgadget.py, usbgadget.py) were
# removed. the sender captures the headless weston session over the wayland socket
# (weston_capture_v1), so it needs libwayland-client and the generated protocol glue
# below; the vendored xml is weston's (MIT). tests: python3 rawplay/test_rawlink.py
WAYLAND_PC := wayland-client
WAYLAND_CFLAGS := $(shell pkg-config --cflags $(WAYLAND_PC))
WAYLAND_LIBS := $(shell pkg-config --libs $(WAYLAND_PC))

CAPTURE_XML := protocol/weston-output-capture.xml
CAPTURE_H := out/weston-output-capture-client-protocol.h
CAPTURE_C := out/weston-output-capture-protocol.c

out:
	mkdir -p out

$(CAPTURE_H): $(CAPTURE_XML) | out
	wayland-scanner client-header $< $@

$(CAPTURE_C): $(CAPTURE_XML) | out
	wayland-scanner private-code $< $@

CFLAGS_RAWLINK := -O2 -Wall -Wextra -std=gnu11 -pthread -Iout $(WAYLAND_CFLAGS)

rawlink: rawlink.c $(CAPTURE_H) $(CAPTURE_C)
	mkdir -p out
	$(CC) $(CFLAGS_RAWLINK) -o out/rawlink rawlink.c $(CAPTURE_C) $(WAYLAND_LIBS)

# the functionfs byte bridge against fifos: no usb device controller needed
test-gadget: test_gadget.c rawlink.c $(CAPTURE_H) $(CAPTURE_C)
	mkdir -p out
	$(CC) $(CFLAGS_RAWLINK) -o out/test_gadget test_gadget.c $(CAPTURE_C) $(WAYLAND_LIBS)

# one-line client for LIVI's helper control socket: manual toggling of LIVI's wireless
# Android Auto ("set-aa 1"/"set-aa 0"). the deployment parks AA on and leaves it alone,
# so this is an admin/debug tool. tests: python3 test_livi_link.py
livi-cmd: deploy/livi-cmd.c
	mkdir -p out
	$(CC) -O2 -Wall -Wextra -o out/livi-cmd deploy/livi-cmd.c

clean:
	rm -f weston-touch.so out/rawlink out/test_gadget out/livi-cmd \
	      out/weston-output-capture-client-protocol.h out/weston-output-capture-protocol.c

.PHONY: all clean rawlink test-gadget livi-cmd
