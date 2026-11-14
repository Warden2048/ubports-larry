#!/bin/sh
# EXPERIMENTAL: GPU-accelerated OpenGL/Vulkan for desktop apps in a Libertine container.
# Installs Mesa built for Android's kgsl GPU driver (lfdevs/mesa-for-android-container) INTO THE
# CONTAINER ONLY - the Ubuntu Touch system itself is not changed.
#   Vulkan: turnip on kgsl.  OpenGL: zink on turnip.  Frames reach X via the CPU (no DRI3 here).
# Run ON THE PHONE as root, phone online:  sudo sh install.sh [container]     (default container: noble)
# Undo:                                    sudo sh install.sh [container] --uninstall
# Per app opt-out:  LARRY_GPU=0 <app>
set -e
cd "$(dirname "$0")"
CT=${1:-noble}; [ "$CT" = "--uninstall" ] && { CT=noble; set -- noble --uninstall; }
R=/home/phablet/.cache/libertine-container/$CT/rootfs
[ -d "$R/usr" ] || { echo "no Libertine container '$CT' (create one in the Libertine app first)"; exit 1; }

cleanup() {
    umount -R "$R/dev" 2>/dev/null || true; umount "$R/proc" "$R/sys" 2>/dev/null || true
    [ -e "$R/etc/resolv.conf.larry" ] && { rm -f "$R/etc/resolv.conf"; mv "$R/etc/resolv.conf.larry" "$R/etc/resolv.conf"; } || true
}
trap cleanup EXIT
mkdir -p "$R/proc" "$R/sys" "$R/dev"
mount -t proc proc "$R/proc"; mount --bind /sys "$R/sys"
mount --rbind /dev "$R/dev"; mount --make-rslave "$R/dev"
mv "$R/etc/resolv.conf" "$R/etc/resolv.conf.larry" 2>/dev/null || true; cp -L /etc/resolv.conf "$R/etc/resolv.conf"
IN="chroot $R env DEBIAN_FRONTEND=noninteractive PATH=/usr/sbin:/usr/bin:/sbin:/bin"
MESA_PKGS="libgl1-mesa-dri libegl-mesa0 libglx-mesa0 libgbm1 mesa-vulkan-drivers mesa-libgallium libglapi-mesa"

if [ "$2" = "--uninstall" ]; then
    rm -f "$R/etc/ld.so.preload" "$R/usr/local/lib/libmesa-env.so"
    for p in $MESA_PKGS; do $IN apt-mark unhold $p >/dev/null 2>&1 || true; done
    $IN sh -c "apt-get update -qq && apt-get install -y --reinstall $(echo $MESA_PKGS)" | tail -3 || true
    find "$R" -xdev ! -user phablet -exec chown -h phablet:phablet {} +
    echo "removed - container is back to Ubuntu's stock (software) Mesa"; exit 0
fi

TAG=mesa-26.2.0-devel-20260511
F=mesa-for-android-container_26.2.0-devel-20260511_ubuntu_noble_arm64.tar.gz
SUM=2482a9ff6e6ef578b6a4fcc6e61ee3f08968bc4a4ac3cb941851983af80a7729
URL=https://github.com/lfdevs/mesa-for-android-container/releases/download/$TAG/$F
if ! echo "$SUM  /tmp/$F" | sha256sum -c --status 2>/dev/null; then
    echo "downloading $F"
    if command -v curl >/dev/null; then curl -fL -o /tmp/$F "$URL"
    elif command -v wget >/dev/null; then wget -O /tmp/$F "$URL"
    else python3 -c "import urllib.request,sys; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])" "$URL" /tmp/$F; fi
fi
echo "$SUM  /tmp/$F" | sha256sum -c --status || { echo "download failed / checksum mismatch"; exit 1; }

echo "== container packages (libllvm20 is what this Mesa build links against)"
$IN sh -c "apt-get update -qq && apt-get install -y --no-install-recommends libllvm20 libvulkan1 libgl1 libegl1 mesa-utils vulkan-tools" | tail -3
echo "== Mesa for kgsl"
tar -xzf /tmp/$F -C "$R" --no-same-owner --keep-directory-symlink --no-overwrite-dir
$IN ldconfig
for p in $MESA_PKGS; do $IN apt-mark hold $p >/dev/null 2>&1 || true; done   # keep apt from swapping stock Mesa back in
install -m 755 libmesa-env.so "$R/usr/local/lib/libmesa-env.so"
echo /usr/local/lib/libmesa-env.so > "$R/etc/ld.so.preload"   # sets the Mesa env for every app (Libertine ignores /etc/environment)
# everything in a Libertine container belongs to phablet; -xdev keeps this out of the mounted /dev, /proc, /sys
find "$R" -xdev ! -user phablet -exec chown -h phablet:phablet {} +
echo "== check"
chroot --userspec=32011:32011 "$R" env HOME=/tmp vulkaninfo --summary 2>/dev/null | grep -m1 deviceName || true
echo "installed - OpenGL apps in '$CT' now render on the Adreno 619 (LARRY_GPU=0 <app> to opt out)"
