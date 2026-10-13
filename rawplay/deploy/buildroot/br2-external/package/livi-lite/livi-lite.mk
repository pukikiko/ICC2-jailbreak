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
# The sibling checkout is a developer tree: local cargo target dirs (tens of
# GB), node_modules and the JS-staged out/ and build/ trees are not inputs to
# the package build, so never rsync them into the package build dir.
LIVI_LITE_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--exclude target --exclude node_modules --exclude out --exclude build
LIVI_LITE_LICENSE = GPL-3.0-or-later
LIVI_LITE_LICENSE_FILES = LICENSE

# host-rustc is the prebuilt Rust from Buildroot's rust-bin package by default
# (BR2_PACKAGE_HOST_RUST is the from-source alternative). The C libraries are
# what the Rust build scripts link against in the target sysroot; gst1-libav
# is the software decoder fallback (avdec_h264/h265), which is what a
# headless appliance without a V4L2/VA-API decoder uses. openssl is livi-cp's
# Linux TLS stack (the manifest's probe list only checks the runtime side).
LIVI_LITE_DEPENDENCIES = host-rustc host-pkgconf host-cmake openssl \
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
	# The wireless-projection AP has the same first-run-install problem as
	# the udev rule, with three files instead of one: livi-core checks
	# /etc/systemd/system/livi-wifi-ap.service, its sudoers rule and the
	# root's copy of the helper at /usr/local/sbin/livi-helperd, and
	# installs all three itself through sudo when any is missing or stale.
	# The appliance rootfs is read-only and its sudo is a shim that execs
	# directly (livi-appliance), so seed what livi-core would install. It
	# renders for the user it runs as (root, see livi.service) and with
	# the systemctl path it resolves on the image; the root's helper copy
	# is a link, which satisfies the byte-for-byte comparison.
	mkdir -p $(TARGET_DIR)/etc/sudoers.d $(TARGET_DIR)/usr/local/sbin
	$(INSTALL) -D -m 0644 $(@D)/assets/linux/livi-wifi-ap.service.template \
		$(TARGET_DIR)/etc/systemd/system/livi-wifi-ap.service
	$(SED) 's|__USERNAME__|root|g;s|__SYSTEMCTL__|/usr/bin/systemctl|g' \
		$(TARGET_DIR)/etc/systemd/system/livi-wifi-ap.service
	$(INSTALL) -D -m 0644 $(@D)/assets/linux/99-LIVI-wifi-ap.sudoers.template \
		$(TARGET_DIR)/etc/sudoers.d/99-LIVI-wifi-ap
	$(SED) 's|__USERNAME__|root|g;s|__SYSTEMCTL__|/usr/bin/systemctl|g' \
		$(TARGET_DIR)/etc/sudoers.d/99-LIVI-wifi-ap
	chmod 0440 $(TARGET_DIR)/etc/sudoers.d/99-LIVI-wifi-ap
	ln -sf /opt/livi/resources/driver/livi-helperd \
		$(TARGET_DIR)/usr/local/sbin/livi-helperd
	# The rule that lets LIVI run the helper as root. The phone installer
	# seeds the same file for its user, aimed at the installed helper; the
	# template's /home/__USERNAME__ path is the AppImage layout, so point it
	# at the helper this image installs, for the root the service runs as.
	$(INSTALL) -D -m 0644 $(@D)/assets/linux/99-LIVI-helper.sudoers.template \
		$(TARGET_DIR)/etc/sudoers.d/99-LIVI-helper
	$(SED) 's|/home/__USERNAME__/.config/LIVI/driver/livi-helperd|/opt/livi/resources/driver/livi-helperd|;s|__USERNAME__|root|g' \
		$(TARGET_DIR)/etc/sudoers.d/99-LIVI-helper
	chmod 0440 $(TARGET_DIR)/etc/sudoers.d/99-LIVI-helper
	# AppStream metadata from the same assets dir; keep it in its canonical
	# location even though the appliance has no software centre.
	$(INSTALL) -D -m 0644 $(@D)/assets/linux/dev.f-io.livi.metainfo.xml \
		$(TARGET_DIR)/usr/share/metainfo/dev.f-io.livi.metainfo.xml
endef

$(eval $(generic-package))
