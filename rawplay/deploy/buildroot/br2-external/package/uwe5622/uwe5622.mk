################################################################################
#
# uwe5622
#
# Out-of-tree modules for the Orange Pi Zero 2W's Unisoc UWE5622/AW859A
# Wi-Fi/BT combo (no mainline driver exists). Built against the image's own
# kernel build tree, so the kernel package itself is never re-extracted or
# recompiled: only these three modules plus the firmware land in the rootfs.
################################################################################

# Pinned armbian/uwe5622 commit: the unified tree Armbian build-tested on
# 6.12.y (among 6.1/6.18/7.0/7.1), so no extra version patches are needed.
UWE5622_VERSION = cc2835a3f935d5297e03cdce464c1785381a7b4d
UWE5622_SITE = $(call github,armbian,uwe5622,$(UWE5622_VERSION))
UWE5622_LICENSE = GPL-2.0
UWE5622_LICENSE_FILES = README.md

UWE5622_DEPENDENCIES = linux

# The driver's own Makefiles already know how to build out of tree
# (M=$(PWD)); the CONFIG_* overrides below select the Allwinner/SDIO
# profile the in-tree integration would get from Kconfig. The Wi-Fi and BT
# modules link against the BSP core's symbols, hence KBUILD_EXTRA_SYMBOLS.
UWE5622_FW_PATH = "/lib/firmware/uwe5622/"
define UWE5622_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(LINUX_MAKE_FLAGS) -C $(LINUX_DIR) \
		M=$(@D)/unisocwcn modules \
		CONFIG_AW_WIFI_DEVICE_UWE5622=y \
		UNISOC_FW_PATH_CONFIG=$(UWE5622_FW_PATH)
	$(TARGET_MAKE_ENV) $(MAKE) $(LINUX_MAKE_FLAGS) -C $(LINUX_DIR) \
		M=$(@D)/unisocwifi modules \
		CONFIG_WLAN_UWE5622=m \
		KBUILD_EXTRA_SYMBOLS=$(@D)/unisocwcn/Module.symvers
	$(TARGET_MAKE_ENV) $(MAKE) $(LINUX_MAKE_FLAGS) -C $(LINUX_DIR) \
		M=$(@D)/tty-sdio modules \
		CONFIG_TTY_OVERY_SDIO=m \
		UNISOC_BSP_INCLUDE=$(@D)/unisocwcn/include \
		KBUILD_EXTRA_SYMBOLS=$(@D)/unisocwcn/Module.symvers
	# wcnmodem.bin is the only firmware the integrate (marlin) path loads
	# via request_firmware ("wcnmodem.bin" on the firmware path); the
	# driver's compiled-in arrays are 8-byte stubs, so without this file
	# the chip never boots. Decode it from the .hex the driver ships.
	python3 -c "import re,sys; \
		tok = re.findall(r'0x([0-9A-Fa-f]{2})', \
			open('$(@D)/unisocwcn/fw/wcnmodem.bin.hex').read()); \
		open('$(@D)/wcnmodem.bin','wb').write(bytes(int(b,16) for b in tok))"
endef

define UWE5622_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/lib/modules/$(LINUX_VERSION)/extra \
		$(TARGET_DIR)/lib/firmware/uwe5622 $(TARGET_DIR)/lib/firmware \
		$(TARGET_DIR)/etc/modules-load.d
	$(INSTALL) -D -m 0644 $(@D)/unisocwcn/uwe5622_bsp_sdio.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION)/extra/uwe5622_bsp_sdio.ko
	$(INSTALL) -D -m 0644 $(@D)/unisocwifi/sprdwl_ng.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION)/extra/sprdwl_ng.ko
	$(INSTALL) -D -m 0644 $(@D)/tty-sdio/sprdbt_tty.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION)/extra/sprdbt_tty.ko
	$(INSTALL) -D -m 0644 $(@D)/wcnmodem.bin \
		$(TARGET_DIR)/lib/firmware/uwe5622/wcnmodem.bin
	ln -sf uwe5622/wcnmodem.bin $(TARGET_DIR)/lib/firmware/wcnmodem.bin
	printf 'uwe5622_bsp_sdio\nsprdwl_ng\nsprdbt_tty\n' > \
		$(TARGET_DIR)/etc/modules-load.d/uwe5622.conf
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/uwe5622-bluetooth.service \
		$(TARGET_DIR)/usr/lib/systemd/system/uwe5622-bluetooth.service
	mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants
	ln -sf ../../../../usr/lib/systemd/system/uwe5622-bluetooth.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/uwe5622-bluetooth.service
	ln -sf ../../../../usr/lib/systemd/system/bluetooth.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/bluetooth.service
endef

$(eval $(generic-package))
