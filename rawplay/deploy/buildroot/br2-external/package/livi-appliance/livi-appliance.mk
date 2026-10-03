################################################################################
#
# livi-appliance
#
################################################################################

LIVI_APPLIANCE_LICENSE = MIT

LIVI_APPLIANCE_DEPENDENCIES = livi rawlink weston-touch weston \
    host-mtools host-dosfstools

# the repo root, for the homebrew stick image: a clean checkout has no
# usb.img, so mkstick.sh builds a minimal one that still gives the unit a
# mountable /fs/usb0 with homebrew/apps/rawplay.sh in it.
LIVI_APPLIANCE_REPO = $(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/../../../..

define LIVI_APPLIANCE_INSTALL_TARGET_CMDS
    mkdir -p $(TARGET_DIR)/opt/livi $(TARGET_DIR)/usr/lib/livi \
        $(TARGET_DIR)/usr/share/livi $(TARGET_DIR)/home/user
    $(INSTALL) -D -m 0755 $(LIVI_APPLIANCE_PKGDIR)/files/rawlink-wait \
        $(TARGET_DIR)/opt/livi/rawlink-wait
    $(INSTALL) -D -m 0755 $(LIVI_APPLIANCE_PKGDIR)/files/livi-persist \
        $(TARGET_DIR)/usr/lib/livi/livi-persist
    $(INSTALL) -D -m 0755 $(LIVI_APPLIANCE_PKGDIR)/files/livi-markers \
        $(TARGET_DIR)/usr/lib/livi/livi-markers
    $(INSTALL) -D -m 0755 $(LIVI_APPLIANCE_PKGDIR)/files/livi-inner \
        $(TARGET_DIR)/opt/livi/livi-inner
    $(INSTALL) -D -m 0755 $(LIVI_APPLIANCE_PKGDIR)/files/livi-pulse \
        $(TARGET_DIR)/usr/lib/livi/livi-pulse
    $(INSTALL) -D -m 0755 $(LIVI_APPLIANCE_PKGDIR)/files/livi-default-sink \
        $(TARGET_DIR)/usr/lib/livi/livi-default-sink
    $(INSTALL) -D -m 0755 $(LIVI_APPLIANCE_PKGDIR)/files/sudo-shim \
        $(TARGET_DIR)/usr/bin/sudo
    $(INSTALL) -D -m 0755 $(LIVI_APPLIANCE_PKGDIR)/files/nmcli-shim \
        $(TARGET_DIR)/usr/bin/nmcli
    $(INSTALL) -D -m 0644 $(LIVI_APPLIANCE_PKGDIR)/files/system.pa \
        $(TARGET_DIR)/etc/pulse/system.pa
    $(INSTALL) -D -m 0644 $(LIVI_APPLIANCE_PKGDIR)/files/journald.conf \
        $(TARGET_DIR)/etc/systemd/journald.conf
    $(INSTALL) -D -m 0644 $(LIVI_APPLIANCE_PKGDIR)/files/fstab \
        $(TARGET_DIR)/etc/fstab
    $(INSTALL) -D -m 0644 $(LIVI_APPLIANCE_PKGDIR)/files/livi-weston.service \
        $(TARGET_DIR)/usr/lib/systemd/system/livi-weston.service
    $(INSTALL) -D -m 0644 $(LIVI_APPLIANCE_PKGDIR)/files/livi.service \
        $(TARGET_DIR)/usr/lib/systemd/system/livi.service
    $(INSTALL) -D -m 0644 $(LIVI_APPLIANCE_PKGDIR)/files/rawlink.service \
        $(TARGET_DIR)/usr/lib/systemd/system/rawlink.service
    $(INSTALL) -D -m 0644 $(LIVI_APPLIANCE_PKGDIR)/files/livi-persist.service \
        $(TARGET_DIR)/usr/lib/systemd/system/livi-persist.service
    $(INSTALL) -D -m 0644 $(LIVI_APPLIANCE_PKGDIR)/files/livi-pulse.service \
        $(TARGET_DIR)/usr/lib/systemd/system/livi-pulse.service
    $(INSTALL) -D -m 0644 $(LIVI_APPLIANCE_PKGDIR)/files/livi-markers.service \
        $(TARGET_DIR)/usr/lib/systemd/system/livi-markers.service
    mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants \
        $(TARGET_DIR)/etc/systemd/system/sysinit.target.wants
    ln -sf ../../../../usr/lib/systemd/system/livi-persist.service \
        $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/livi-persist.service
    ln -sf ../../../../usr/lib/systemd/system/livi-weston.service \
        $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/livi-weston.service
    ln -sf ../../../../usr/lib/systemd/system/livi.service \
        $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/livi.service
    ln -sf ../../../../usr/lib/systemd/system/rawlink.service \
        $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/rawlink.service
    ln -sf ../../../../usr/lib/systemd/system/livi-pulse.service \
        $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/livi-pulse.service
    ln -sf ../../../../usr/lib/systemd/system/livi-markers.service \
        $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/livi-markers.service
    $(LIVI_APPLIANCE_PKGDIR)/mkstick.sh \
        $(LIVI_APPLIANCE_REPO) $(TARGET_DIR)/opt/livi/usb.img \
        $(HOST_DIR) || true
    test -f $(TARGET_DIR)/opt/livi/usb.img || { \
        echo "livi-appliance: no usb.img (need mtools/dosfstools host tools)" >&2; \
        exit 1; \
    }
endef

$(eval $(generic-package))
