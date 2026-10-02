# the host side of the livi stack. weston-touch.so is the weston module that adds a touch
# device the raw sender can drive, so the app gets real touch instead of an x11 mouse.
# needs weston-devel (fedora: dnf install weston-devel, postmarketos: apk add weston-dev).
# the newest installed libweston is picked; the major is passed to the source because
# weston 16 changed the touch api (see the comments in weston-touch.c).
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
# removed. it needs no x11 or weston headers: input injection dlopens libX11/libXtst at
# runtime. tests: python3 rawplay/test_rawlink.py
CFLAGS_RAWLINK := -O2 -Wall -Wextra -std=gnu11 -pthread

rawlink: rawlink.c
	mkdir -p out
	$(CC) $(CFLAGS_RAWLINK) -o out/rawlink rawlink.c -ldl

# the functionfs byte bridge against fifos: no usb device controller needed
test-gadget: test_gadget.c rawlink.c
	mkdir -p out
	$(CC) $(CFLAGS_RAWLINK) -o out/test_gadget test_gadget.c -ldl

# one-line client for LIVI's helper control socket: manual toggling of LIVI's wireless
# Android Auto ("set-aa 1"/"set-aa 0"). the deployment parks AA on and leaves it alone,
# so this is an admin/debug tool. tests: python3 test_livi_link.py
livi-cmd: deploy/livi-cmd.c
	mkdir -p out
	$(CC) -O2 -Wall -Wextra -o out/livi-cmd deploy/livi-cmd.c

clean:
	rm -f weston-touch.so out/rawlink out/test_gadget out/livi-cmd

.PHONY: all clean rawlink test-gadget livi-cmd
