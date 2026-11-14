#!/bin/bash
# Build wrapper for the OnePlus Nord N30 5G (larry) Ubuntu Touch port.
# Usage: ./build.sh [-b workdir] [-o out] [-k]   (same flags as halium-generic-adaptation-build-tools)
#        EXTRAS="touchboost" ./build.sh           (override extras.conf; EXTRAS="" = no extras)
set -xe

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

[ -d build ] || git clone https://gitlab.com/ubports/porting/community-ports/halium-generic-adaptation-build-tools build

BUILD_DIR=workdir
args=("$@")
while [ $# -gt 0 ]; do
    case "$1" in (-b) BUILD_DIR="$2"; shift;; esac
    shift
done

# 1) Fetch toolchains, ramdisks and the kernel without building.
./build/build.sh -c "${args[@]}"

# 2) Inject the Halium config fragment into the LineageOS kernel tree.
source ./deviceinfo
KDIR="$BUILD_DIR/downloads/$(basename "${deviceinfo_kernel_source%.git}")"
cp "$HERE/kernel-config/halium.config" "$KDIR/arch/arm64/configs/halium.config"

# Apply port-specific kernel patches (idempotent: skipped if already applied).
for p in "$HERE"/kernel-patches/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$KDIR" apply --reverse --check "$p" 2>/dev/null; then
        echo "kernel patch already applied: $(basename "$p")"
    else
        git -C "$KDIR" apply "$p"
        echo "kernel patch applied: $(basename "$p")"
    fi
done

# Modern distros have no python2; the tools' mkdtboimg.py (pie-gsi) is py2-only.
# Swap in AOSP main's python3 version and shim python2 -> python3.
if ! command -v python2 >/dev/null; then
    MKDTBO="$BUILD_DIR/downloads/libufdt/utils/src/mkdtboimg.py"
    if [ -f "$MKDTBO" ] && ! head -1 "$MKDTBO" | grep -q python3; then
        curl -sSfL "https://android.googlesource.com/platform/system/libufdt/+/refs/heads/main/utils/src/mkdtboimg.py?format=TEXT" | base64 -d > "$MKDTBO.new"
        mv "$MKDTBO.new" "$MKDTBO"
    fi
    mkdir -p "$BUILD_DIR/shim"
    ln -sf "$(command -v python3)" "$BUILD_DIR/shim/python2"
    export PATH="$(realpath "$BUILD_DIR/shim"):$PATH"
fi

# 3) Real build.
# Empty-but-set LOCALVERSION stops setlocalversion appending "+" for a modified tree.
export LOCALVERSION=

# Vendor drivers (qcacld-3.0, camera) hard-code -Werror; newer clang trips on
# intentional truncations. Specific -Wno-error=X beats a blanket -Werror.
export KCFLAGS="${KCFLAGS} -Wno-error=fortify-source -Wno-error=unused-but-set-variable \
-Wno-error=single-bit-bitfield-constant-conversion -Wno-error=deprecated-non-prototype \
-Wno-error=enum-conversion -Wno-error=array-parameter -Wno-error=unused-const-variable"
./build/build.sh "${args[@]}"

# 4) Optional extras (see extras/README.md): the ones listed in extras.conf - or in $EXTRAS,
# e.g. EXTRAS="" ./build.sh for a plain image - are added to the device tarball straight into the
# rootfs (system/...), not into /opt/halium-overlay: the overlay store can only shadow files that
# already exist, and extras add new ones (binaries, systemd units).
source ./deviceinfo
OUT_DIR=out
set -- "${args[@]}"
while [ $# -gt 0 ]; do case "$1" in (-o) OUT_DIR="$2"; shift;; esac; shift; done
EXTRAS="${EXTRAS-$(grep -v '^[[:space:]]*#' "$HERE/extras.conf" 2>/dev/null | tr '\n' ' ')}"
TARBALL="$OUT_DIR/device_${deviceinfo_codename}.tar.xz"
if [ -n "${EXTRAS// /}" ] && [ -f "$TARBALL" ]; then
    STAGE=$(mktemp -d)
    tar -xJf "$TARBALL" -C "$STAGE"
    for e in $EXTRAS; do
        if [ -d "$HERE/extras/$e/overlay/system" ]; then
            cp -a "$HERE/extras/$e/overlay/system/." "$STAGE/system/"
            echo "extra baked into the image: $e"
        elif [ -d "$HERE/extras/$e" ]; then
            echo "extra '$e' is installed on the phone after flashing - see extras/$e/README.md"
        else
            echo "extras.conf: unknown extra '$e'" >&2; exit 1
        fi
    done
    tar -cJf "$TARBALL" -C "$STAGE" --owner=root --group=root partitions/ system/
    rm -rf "$STAGE"
fi
