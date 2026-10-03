################################################################################
#
# unit-sim
#
################################################################################

UNIT_SIM_LICENSE = MIT

# the source lives in the tests/ directory of this br2-external tree's parent,
# next to smoke.sh which expects the binary in the guest
define UNIT_SIM_EXTRACT_CMDS
    cp -a $(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/../tests/unit-sim/* $(@D)/
endef

define UNIT_SIM_BUILD_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
        CC="$(TARGET_CC)" \
        PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

define UNIT_SIM_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/unit-sim $(TARGET_DIR)/usr/bin/unit-sim
    $(INSTALL) -D -m 0644 $(UNIT_SIM_PKGDIR)/unit-sim.service \
        $(TARGET_DIR)/usr/lib/systemd/system/unit-sim.service
    mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants
    ln -sf ../../../../usr/lib/systemd/system/unit-sim.service \
        $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/unit-sim.service
endef

UNIT_SIM_DEPENDENCIES = libusb host-pkgconf

$(eval $(generic-package))
