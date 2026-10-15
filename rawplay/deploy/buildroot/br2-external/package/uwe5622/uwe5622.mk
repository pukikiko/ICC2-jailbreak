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
endef

# The firmware the marlin bootloader loads from /lib/firmware/uwe5622/.
# It is the raw Marlin3Lite image Armbian ships for this chip (the same
# file as armbian/firmware uwe5622/wcnmodem.bin, sha256
# 119b87ce30875734a67462f7293fb8fe85acf3270fe8b78c978ae24be7715a80),
# plus the per-chip NVM (RF calibration) config. Do not use the
# fw/wcnmodem.bin.hex from the driver tree: that pack only carries images
# for the Marlin3E/3 chips (tags 3EAA/30AC), and this build's chip is a
# Marlin3Lite AB (0x2355b001) whose tag search cannot match them.
# The wifi_2355b001_1ant.ini lookup uses the un-suffixed path
# /lib/firmware/wifi_<chipid>_<ant>ant.ini, so it ships both next to the
# firmware and as a /lib/firmware symlink, like Armbian does.
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
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/wcnmodem.bin \
		$(TARGET_DIR)/lib/firmware/uwe5622/wcnmodem.bin
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/wifi_2355b001_1ant.ini \
		$(TARGET_DIR)/lib/firmware/uwe5622/wifi_2355b001_1ant.ini
	ln -sf uwe5622/wcnmodem.bin $(TARGET_DIR)/lib/firmware/wcnmodem.bin
	ln -sf uwe5622/wifi_2355b001_1ant.ini \
		$(TARGET_DIR)/lib/firmware/wifi_2355b001_1ant.ini
	# The modules are loaded by livi-wifi-modules.service, not by
	# modules-load.d: the firmware download takes ~2 s and used to hold
	# sysinit (and with it D-Bus, bluetooth and livi) behind it. See the
	# unit's comments.
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/livi-wifi-modules.service \
		$(TARGET_DIR)/usr/lib/systemd/system/livi-wifi-modules.service
	mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants
	ln -sf ../../../../usr/lib/systemd/system/livi-wifi-modules.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/livi-wifi-modules.service
	# wpa_supplicant and the helper's AP unit must not race the module
	# load: wpa_supplicant picks its interface at start and hostapd needs
	# wlan0, both of which only exist once the modules are in.
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/wpa_supplicant.service.d-order.conf \
		$(TARGET_DIR)/etc/systemd/system/wpa_supplicant.service.d/uwe5622-order.conf
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/livi-wifi-ap.service.d-order.conf \
		$(TARGET_DIR)/etc/systemd/system/livi-wifi-ap.service.d/uwe5622-order.conf
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/uwe5622-bluetooth.service \
		$(TARGET_DIR)/usr/lib/systemd/system/uwe5622-bluetooth.service
	# hciattach_opi: the Spreadtrum-patched BlueZ hciattach with the "sprd"
	# vendor attach protocol this chip needs to start its Bluetooth radio.
	# Plain btattach -P h4 registers the controller but leaves the radio
	# unresponsive to inquiry. This is Armbian's prebuilt aarch64 build,
	# not Orange Pi's: Orange Pi's build hardcodes the placeholder BD_ADDR
	# 10:11:12:13:14:15, while Armbian's calls its set_mac_address() and
	# reads a real one. That address path (Android
	# /sys/class/addr_mgt/addr_bt, normally filled by Armbian's sunxi_addr
	# module) is patched in place to /run/livi/bt-addr, which the unit
	# fills from the Wi-Fi MAC. Same byte length, NUL padded. Links only
	# glibc, >= 2.17. See files/uwe5622-bluetooth.service.
	$(INSTALL) -D -m 0755 $(UWE5622_PKGDIR)/files/hciattach_opi \
		$(TARGET_DIR)/usr/bin/hciattach_opi
	$(INSTALL) -D -m 0755 $(UWE5622_PKGDIR)/files/livi-bt-addr \
		$(TARGET_DIR)/usr/lib/livi/livi-bt-addr
	# The pskey and RF front-end tables the sprd init programs into the
	# chip (its zeroed values otherwise leave BT without a feature set and
	# with RF_switch_mode 0 - the radio never comes up). Armbian's blob
	# reads them from /lib/firmware, Orange Pi's from /vendor/etc; ship
	# both paths. Files are the official ones from armbian/firmware.
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/bt_configure_pskey.ini \
		$(TARGET_DIR)/lib/firmware/bt_configure_pskey.ini
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/bt_configure_rf.ini \
		$(TARGET_DIR)/lib/firmware/bt_configure_rf.ini
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/bt_configure_pskey.ini \
		$(TARGET_DIR)/vendor/etc/bt_configure_pskey.ini
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/bt_configure_rf.ini \
		$(TARGET_DIR)/vendor/etc/bt_configure_rf.ini
	# livi-helperd connects to org.bluez once when livi-core starts and
	# gives up for the rest of the session if that fails, so on this board
	# livi must wait until hciattach_opi has registered hci0 (the attach unit
	# waits for the node before its start job completes). The drop-in
	# keeps the ordering out of the shared livi.service, which the qemu
	# image uses without any Bluetooth hardware.
	$(INSTALL) -D -m 0644 $(UWE5622_PKGDIR)/files/livi.service.d-uwe5622.conf \
		$(TARGET_DIR)/etc/systemd/system/livi.service.d/uwe5622.conf
	mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants
	ln -sf ../../../../usr/lib/systemd/system/uwe5622-bluetooth.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/uwe5622-bluetooth.service
	ln -sf ../../../../usr/lib/systemd/system/bluetooth.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/bluetooth.service
endef

$(eval $(generic-package))
