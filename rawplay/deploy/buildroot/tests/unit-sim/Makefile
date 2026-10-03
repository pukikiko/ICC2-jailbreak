# unit-sim is a guest program: Buildroot compiles it with the target
# toolchain and the target libusb (package/unit-sim). It can also be built
# standalone with `make PKG_CONFIG=pkg-config` on any aarch64 host.
CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS := -O2 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LDLIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0)

all: unit-sim

unit-sim: unit-sim.c

clean:
	rm -f unit-sim

.PHONY: all clean
