################################################################################
#
# rawlink
#
################################################################################

# the working tree's rawplay/ directory is the site, so the package builds
# exactly the sources checked out next to this br2-external tree. the local
# site method rsyncs the site into $(@D), so the site must be the directory
# that contains the Makefile (three levels up from br2-external/:
# br2-external -> buildroot -> deploy -> rawplay).
RAWLINK_SITE = $(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/../../..
RAWLINK_SITE_METHOD = local
RAWLINK_LICENSE = MIT (repo convention, see rawplay/README.md)
# rawplay/ contains deploy/, and deploy/ contains this Buildroot tree's
# own build directory; the local method rsyncs the whole site, so keep the
# (possibly multi-GB) build tree out of the package build dir.
RAWLINK_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude /deploy --exclude /out --exclude /__pycache__

RAWLINK_DEPENDENCIES = host-pkgconf host-wayland wayland

# the Makefile generates the weston_capture_v1 client glue with wayland-scanner
# (host-wayland puts it in $(HOST_DIR)/bin, which TARGET_MAKE_ENV puts on PATH).
# out/ comes in from the working tree and can hold a newer host binary or host
# generated glue; drop it so make always rebuilds for the target.
define RAWLINK_BUILD_CMDS
    rm -rf $(@D)/out
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
        CC="$(TARGET_CC)" \
        PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
        rawlink
endef

define RAWLINK_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/out/rawlink $(TARGET_DIR)/opt/livi/rawlink
endef

$(eval $(generic-package))
