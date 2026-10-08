#!/bin/sh
# Build LIVI-Lite from source and stage the installed runtime tree.
#
# This is the one build recipe both appliances use; only the environment
# differs (Buildroot's cross cargo + target sysroot, or the phone's native
# musl cargo). It replaces what upstream's scripts/build-native.mjs does, but
# with no Node/pnpm in the loop and staged for the installed layout that
# `Resources::installed()` in livi-core expects:
#
#   $DEST/livi-core
#   $DEST/livi-ui
#   $DEST/resources/driver/livi-helperd
#   $DEST/resources/gst-host/livi-gst-host
#   $DEST/resources/compositor/livi-compositor
#   $DEST/resources/<assets/linux templates>
#
# usage: livi-lite-build.sh SRCDIR DESTDIR
#
# environment:
#   CARGO                 cargo to run (default: cargo)
#   CARGO_TARGET_DIR      shared target dir (default: $SRCDIR/target)
#   CARGO_BUILD_TARGET    target triple (cross builds; e.g. aarch64-...-gnu)
#   CC/CFLAGS/LDFLAGS/PKG_CONFIG_*  forwarded to the C build scripts
#   LIVI_LITE_BUNDLE_GST  optional dir with a GStreamer bundle to ship under
#                         $DEST/resources/gstreamer/<platform> (not used by
#                         either appliance: both build against system gst)
set -eu

SRC=$(cd "$1" && pwd)
DEST=$2
CARGO=${CARGO:-cargo}
: "${CARGO_TARGET_DIR:=$SRC/target}"
export CARGO_TARGET_DIR

# aws-lc-sys (CarPlay's crypto) refuses to compile its jitterentropy fallback
# with -O2, and cross/CI environments export -O2 in CFLAGS. The jitter source
# is optional; the kernel CSPRNG is what an appliance wants. Set here so both
# the Buildroot and the on-phone build get it.
AWS_LC_SYS_NO_JITTER_ENTROPY=1
export AWS_LC_SYS_NO_JITTER_ENTROPY

OUT="$CARGO_TARGET_DIR/release"
[ -n "${CARGO_BUILD_TARGET:-}" ] && OUT="$CARGO_TARGET_DIR/$CARGO_BUILD_TARGET/release"

build() {
    manifest=$1
    shift
    echo "livi-lite: cargo build --release --locked -p $*"
    "$CARGO" build --release --locked --manifest-path "$manifest" "$@"
}

# The four standalone workspaces that make the appliance. Order is only for
# readable logs; cargo builds dependencies on demand either way.
build "$SRC/native/livi-gst-video/rust/Cargo.toml" -p gst-video-host
build "$SRC/native/livi-compositor/rust/Cargo.toml" -p livi-compositor
build "$SRC/native/livi-helperd/Cargo.toml" -p livi-helperd -p livi-core
build "$SRC/native/livi-ui/Cargo.toml" -p livi-ui

for bin in livi-core livi-ui livi-helperd livi-gst-host livi-compositor; do
    test -x "$OUT/$bin" || {
        echo "livi-lite: $OUT/$bin missing after the build" >&2
        exit 1
    }
done

# installed layout, exactly what Resources::installed() resolves
mkdir -p "$DEST/resources/driver" "$DEST/resources/gst-host" "$DEST/resources/compositor"
install -D -m 0755 "$OUT/livi-core" "$DEST/livi-core"
install -D -m 0755 "$OUT/livi-ui" "$DEST/livi-ui"
install -D -m 0755 "$OUT/livi-helperd" "$DEST/resources/driver/livi-helperd"
install -D -m 0755 "$OUT/livi-gst-host" "$DEST/resources/gst-host/livi-gst-host"
install -D -m 0755 "$OUT/livi-compositor" "$DEST/resources/compositor/livi-compositor"

# the root templates livi-core renders (udev rules, sudoers, the wifi-ap unit)
# and the helper's touch filter; templates dir == resources dir
cp -a "$SRC/assets/linux/." "$DEST/resources/"

# both appliances use the system GStreamer; a bundle is opt-in for other
# environments (the layout finds it at resources/gstreamer/<platform>/)
if [ -n "${LIVI_LITE_BUNDLE_GST:-}" ] && [ -d "$LIVI_LITE_BUNDLE_GST" ]; then
    mkdir -p "$DEST/resources/gstreamer"
    cp -a "$LIVI_LITE_BUNDLE_GST" "$DEST/resources/gstreamer/"
fi

echo "livi-lite: staged $DEST"
