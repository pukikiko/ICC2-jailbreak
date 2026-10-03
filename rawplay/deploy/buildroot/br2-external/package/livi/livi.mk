################################################################################
#
# livi
#
################################################################################

LIVI_VERSION = 8.3.0
LIVI_SITE = https://github.com/f-io/LIVI/releases/download/v$(LIVI_VERSION)
LIVI_SOURCE = LIVI-$(LIVI_VERSION)-linux-arm64.AppImage
LIVI_LICENSE = Proprietary (LIVI release; see LICENSES in the AppImage)
LIVI_LICENSE_FILES =

# host-squashfs provides unsquashfs, host-python3 computes the squashfs
# offset from the AppImage's ELF headers (the aarch64 runtime cannot run on
# the build host). gstreamer1/gst1-plugins-base are the headers the
# livi-stride-fix.so shim compiles against.
LIVI_DEPENDENCIES = host-python3 host-squashfs host-pkgconf \
	gstreamer1 gst1-plugins-base

# The release AppImage carries helper trees for every platform next to the
# aarch64 ones: resources/gstreamer/linux-x64 is 223 MB of x86-64 GStreamer
# and @node-usb has darwin/x64 addons. Buildroot's check-bin-arch rightly
# rejects the foreign ELF files, and none of it can run on the target, so the
# install prunes them and the checker skips the rest of the prebuilt tree
# (it is a proprietary prebuilt release, not something this tree built).
LIVI_BIN_ARCH_EXCLUDE = /opt/livi/app

# Buildroot does not know the .AppImage extension; extract it ourselves.
# the downloaded file is a self-mounting aarch64 runtime followed by a
# squashfs. never run --appimage-extract at boot: FUSE plus a squashfs mount
# is slow, and the runtime cannot execute on the x86 build host.
define LIVI_EXTRACT_CMDS
    $(HOST_DIR)/bin/python3 $(LIVI_PKGDIR)/extract-appimage.py \
        --appimage $($(PKG)_DL_DIR)/$($(PKG)_SOURCE) \
        --unsquashfs $(HOST_DIR)/bin/unsquashfs \
        --dest $(@D)/app
    touch $(@D)/.stamp_extracted
endef
LIVI_POST_EXTRACT_HOOKS = LIVI_EXTRACTED_CHECK
define LIVI_EXTRACTED_CHECK
    test -x $(@D)/app/livi || { \
        echo "livi: AppImage extraction produced no runnable livi binary" >&2; \
        exit 1; \
    }
endef

# the stride shim is the same source as the phone deployment's (repo
# rawplay/deploy/livistride.c). it interposes
# gst_wl_shm_memory_construct_wl_buffer() in the bundled GStreamer so a
# row-padded frame is handed to waylandsink with its real stride, or the
# wireless video arrives diagonally sheared.
define LIVI_BUILD_CMDS
    $(TARGET_CC) -O2 -fPIC -shared \
        $$($(PKG_CONFIG_HOST_BINARY) --cflags gstreamer-1.0 gstreamer-video-1.0) \
        -o $(@D)/livi-stride-fix.so \
        $(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/../../livistride.c
endef

define LIVI_INSTALL_TARGET_CMDS
    mkdir -p $(TARGET_DIR)/opt/livi
    rm -rf $(TARGET_DIR)/opt/livi/app
    cp -a $(@D)/app $(TARGET_DIR)/opt/livi/app
    rm -rf $(TARGET_DIR)/opt/livi/app/resources/gstreamer/linux-x64
    rm -rf $(TARGET_DIR)/opt/livi/app/resources/app.asar.unpacked/node_modules/@node-usb/usb-darwin-* \
           $(TARGET_DIR)/opt/livi/app/resources/app.asar.unpacked/node_modules/@node-usb/usb-linux-x64-gnu
    $(INSTALL) -D -m 0755 $(@D)/livi-stride-fix.so \
        $(TARGET_DIR)/opt/livi/livi-stride-fix.so
    $(INSTALL) -D -m 0755 $(LIVI_PKGDIR)/livi-supervisor \
        $(TARGET_DIR)/opt/livi/livi-supervisor
    $(INSTALL) -D -m 0644 $(LIVI_PKGDIR)/livi-config.json \
        $(TARGET_DIR)/usr/share/livi/config.json
endef

# python 3.14 removed the implicit event loop creation asyncio.get_event_loop()
# relied on, and the wireless helper calls it at import time. the .pth shim is
# installed only on a python this new.
LIVI_PYTHON_MIN = $(shell printf '%s\n3.14\n' $(PYTHON3_VERSION_MAJOR) | \
    sort -V | head -n 1)
ifeq ($(LIVI_PYTHON_MIN),3.14)
define LIVI_INSTALL_ASYNCIO_COMPAT
    $(INSTALL) -D -m 0644 $(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/../../livi_asyncio_compat.py \
        $(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages/livi_asyncio_compat.py
    $(INSTALL) -D -m 0644 $(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/../../livi-asyncio-compat.pth \
        $(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages/livi-asyncio-compat.pth
endef
LIVI_POST_INSTALL_TARGET_HOOKS += LIVI_INSTALL_ASYNCIO_COMPAT
endif

$(eval $(generic-package))
