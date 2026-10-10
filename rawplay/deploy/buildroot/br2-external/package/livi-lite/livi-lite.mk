################################################################################
#
# livi-lite
#
################################################################################

# The source is the pukikiko/LIVI-Lite checkout that lives next to this
# ICC2-jailbreak repo (the local site method copies the working tree, so a
# `git pull` plus a rebuild is an update). Override on the command line for a
# checkout somewhere else:  make LIVI_LITE_SRC=/path/to/LIVI-Lite
LIVI_LITE_SRC ?= $(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/../../../../../LIVI-Lite
LIVI_LITE_SITE = $(LIVI_LITE_SRC)
LIVI_LITE_SITE_METHOD = local
LIVI_LITE_LICENSE = GPL-3.0-or-later
LIVI_LITE_LICENSE_FILES = LICENSE

# host-rustc is the prebuilt Rust from Buildroot's rust-bin package by default
# (BR2_PACKAGE_HOST_RUST is the from-source alternative). The C libraries are
# what the Rust build scripts link against in the target sysroot; gst1-libav
# is the software decoder fallback (avdec_h264/h265), which is what a
# headless appliance without a V4L2/VA-API decoder uses.
LIVI_LITE_DEPENDENCIES = host-rustc host-pkgconf host-cmake \
	gstreamer1 gst1-plugins-base gst1-plugins-good gst1-plugins-bad \
	gst1-libav wayland libxkbcommon

# Build all four workspaces with the target cargo from the Rust distribution
# and stage the installed layout. CARGO_HOME is the same shared dir the cargo
# infrastructure uses, so the crates.io downloads are cached in DL_DIR
# instead of being fetched for every rebuild. The build is not `--offline`:
# the crates are given by each workspace's Cargo.lock, fetched on first use.
# The shared build script exports AWS_LC_SYS_NO_JITTER_ENTROPY=1 itself (see
# rawplay/deploy/livi-lite-build.sh): Buildroot's -O2 CFLAGS would otherwise
# fight aws-lc-sys's -O0 requirement for its optional jitterentropy source.
define LIVI_LITE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(PKG_CARGO_ENV) \
		CARGO_TARGET_DIR=$(@D)/target \
		$(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/../../livi-lite-build.sh \
			$(@D) $(@D)/stage
endef

define LIVI_LITE_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/opt/livi
	cp -a $(@D)/stage/. $(TARGET_DIR)/opt/livi/
	$(INSTALL) -D -m 0644 $(LIVI_LITE_PKGDIR)/livi-config.json \
		$(TARGET_DIR)/usr/share/livi/config.json
	# livi-core installs this udev rule at first run when it is missing, then
	# restarts itself to pick it up. The appliance rootfs is a read-only
	# squashfs, so install the rule and its helper up front; if it did not,
	# every boot would log the failed install and lose the rule.
	$(INSTALL) -D -m 0644 $(@D)/assets/linux/99-LIVI.rules.template \
		$(TARGET_DIR)/etc/udev/rules.d/99-LIVI.rules
	$(INSTALL) -D -m 0755 $(@D)/assets/linux/livi-touch-filter \
		$(TARGET_DIR)/usr/local/lib/livi/livi-touch-filter
endef

$(eval $(generic-package))
