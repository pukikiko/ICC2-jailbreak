################################################################################
#
# weston-touch
#
################################################################################

# rawplay/ itself is the site (the local method rsyncs it into $(@D), and the
# Makefile there is what builds the module)
WESTON_TOUCH_SITE = $(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/../../..
WESTON_TOUCH_SITE_METHOD = local
WESTON_TOUCH_LICENSE = MIT
# rawplay/ contains deploy/, and deploy/ contains this Buildroot tree's
# own build directory; the local method rsyncs the whole site, so keep the
# (possibly multi-GB) build tree out of the package build dir.
WESTON_TOUCH_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude /deploy --exclude /out --exclude /__pycache__

WESTON_TOUCH_DEPENDENCIES = host-pkgconf weston wayland

# the module includes libweston's private headers for the seat/touch entry
# points, so it must be built against the same libweston the image ships.
# the major comes from pkg-config at build time instead of a hardcoded number,
# and is what the source switches on (see weston-touch.c).
define WESTON_TOUCH_BUILD_CMDS
    WESTON_PC=$$($(PKG_CONFIG_HOST_BINARY) --list-all | \
        awk '/^libweston-[0-9]+ /{print $$1}' | sort | tail -1); \
    if [ -z "$$WESTON_PC" ]; then \
        echo "weston-touch: no libweston pkg-config file in the sysroot" >&2; \
        exit 1; \
    fi; \
    WESTON_MAJOR=$${WESTON_PC#libweston-}; \
    $(TARGET_CC) -O2 -Wall -Wextra -fPIC -DLIVI_WESTON_MAJOR=$$WESTON_MAJOR \
        $$($(PKG_CONFIG_HOST_BINARY) --cflags $$WESTON_PC wayland-server) \
        -shared -o $(@D)/weston-touch.so $(@D)/weston-touch.c \
        $$($(PKG_CONFIG_HOST_BINARY) --libs $$WESTON_PC wayland-server)
endef

define WESTON_TOUCH_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0644 $(@D)/weston-touch.so \
        $(TARGET_DIR)/opt/livi/weston-touch.so
endef

$(eval $(generic-package))
