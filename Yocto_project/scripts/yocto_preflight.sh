#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
layer_dir="$project_dir/yocto/meta-adaptive-video"

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

command -v bitbake >/dev/null 2>&1 ||
    fail "bitbake not found; first source the BSP setup script (for example oe-init-build-env)"
command -v bitbake-layers >/dev/null 2>&1 || fail "bitbake-layers not found"
[ -f "$layer_dir/conf/layer.conf" ] || fail "invalid layer directory: $layer_dir"
[ -n "${BUILDDIR:-}" ] && [ -f "$BUILDDIR/conf/bblayers.conf" ] ||
    fail "no initialized Yocto build directory; source the BSP build environment first"

layers=$(bitbake-layers show-layers 2>&1) || fail "cannot read active Yocto layers"
case "$layers" in
    *meta-adaptive-video*) ;;
    *)
        echo "Adaptive-video layer is not active. Add it with:" >&2
        echo "  bitbake-layers add-layer $layer_dir" >&2
        exit 1
        ;;
esac
recipes=$(bitbake-layers show-recipes adaptive-video adaptive-video-image x264 2>&1) ||
    fail "cannot resolve adaptive-video/image/x264 recipes"
echo "$recipes" | grep -q '^adaptive-video:' || fail "adaptive-video recipe is not visible"
echo "$recipes" | grep -q '^adaptive-video-image:' || fail "adaptive-video-image recipe is not visible"
echo "$recipes" | grep -q '^x264:' ||
    fail "x264 recipe is not visible in this BSP's OE-Core"

image_env=$(bitbake -e adaptive-video-image 2>/dev/null) ||
    fail "BitBake cannot parse adaptive-video-image"
machine=$(echo "$image_env" | sed -n 's/^MACHINE="\([^"]*\)"/\1/p' | tail -n 1)
distro_features=$(echo "$image_env" |
    sed -n 's/^DISTRO_FEATURES="\([^"]*\)"/\1/p' | tail -n 1)
init_manager=$(echo "$image_env" |
    sed -n 's/^VIRTUAL-RUNTIME_init_manager="\([^"]*\)"/\1/p' | tail -n 1)
[ -n "$machine" ] || fail "MACHINE is empty"
case " $distro_features " in
    *' systemd '*) ;;
    *) fail "DISTRO_FEATURES does not contain systemd" ;;
esac
[ "$init_manager" = "systemd" ] ||
    fail "VIRTUAL-RUNTIME_init_manager is '$init_manager', expected systemd"

ffmpeg_env=$(bitbake -e ffmpeg 2>/dev/null) || fail "BitBake cannot parse ffmpeg"
ffmpeg_packageconfig=$(echo "$ffmpeg_env" |
    sed -n 's/^PACKAGECONFIG="\([^"]*\)"/\1/p' | tail -n 1)
case " $ffmpeg_packageconfig " in
    *' x264 '*) ;;
    *) fail "ffmpeg PACKAGECONFIG does not contain x264; check ffmpeg_%.bbappend" ;;
esac
case " $ffmpeg_packageconfig " in
    *' gpl '*) ;;
    *) fail "ffmpeg PACKAGECONFIG does not contain gpl, which libx264 requires" ;;
esac

echo "Yocto preflight passed"
echo "  BUILDDIR: $BUILDDIR"
echo "  MACHINE:  $machine"
echo "  init:     $init_manager"
echo "  layer:    $layer_dir"
echo "  ffmpeg:   gpl + x264 enabled"
case "$machine" in
    *imx6ull*|*mx6ull*) ;;
    *) echo "WARNING: MACHINE '$machine' does not look like an i.MX6ULL target" >&2 ;;
esac

