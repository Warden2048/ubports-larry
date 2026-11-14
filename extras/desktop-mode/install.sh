#!/bin/sh
# Desktop mode / display-out to a PC for the OnePlus Nord N30 port (larry).
# Run ON THE PHONE as root from this folder, e.g.:
#   adb push extras/desktop-mode /tmp/desktop-mode
#   adb shell sudo sh /tmp/desktop-mode/install.sh            (sudo asks for the phone passcode)
# Remove again with:  sudo sh install.sh --uninstall
set -e
cd "$(dirname "$0")"
mount -o remount,rw /
if [ "$1" = "--uninstall" ]; then
    rm -f /usr/local/bin/larry-scrcpy-server /usr/local/bin/app_process /etc/udev/rules.d/90-larry-uhid.rules
    mount -o remount,ro / 2>/dev/null || true
    echo "desktop mode removed"; exit 0
fi
install -m 755 bin/larry-scrcpy-server /usr/local/bin/larry-scrcpy-server
install -m 755 bin/app_process /usr/local/bin/app_process
# scrcpy's virtual keyboard/mouse (uhid) and pointer (uinput) must be usable by the phablet user
printf 'KERNEL=="uhid", MODE="0660", GROUP="plugdev"\nKERNEL=="uinput", MODE="0660", GROUP="plugdev"\n' \
    > /etc/udev/rules.d/90-larry-uhid.rules
sync; mount -o remount,ro / 2>/dev/null || true
for d in /dev/uhid /dev/uinput; do [ -e $d ] && chgrp plugdev $d && chmod 0660 $d; done
# the scrcpy client pushes its (unused) server jar to /data/local/tmp
mkdir -p /android/data/local/tmp && chmod 1777 /android/data/local/tmp
C=/home/phablet/.config/larry-scrcpy.conf
if [ ! -e $C ]; then
    sudo -u phablet mkdir -p /home/phablet/.config
    sudo -u phablet sh -c "cat > $C" <<'CONF'
# larry-scrcpy server settings (read each time scrcpy connects)
# Mouse-wheel speed: notches are multiplied by this (1 = Lomiri default)
scroll_multiplier = 3
# Frames the hardware encoder may work on at once. 2 = 60 fps; 1 halves the frame rate on this SoC
max_in_flight = 2
# How often the compositor is asked for a desktop frame (encoding stays at the scrcpy rate).
# 120 keeps Lomiri's second screen from stalling; 0 = same as the encode rate
capture_fps = 120
# Phone panel refresh while streaming the desktop (0 = leave alone; this panel only offers 120 Hz)
desktop_refresh = 0
# Clock the video encoder as if for this many fps. 0 = off
operating_rate = 0
# Leave at 0: the input-config route to a flat mouse breaks the keyboard until reboot (use flat-mouse/)
flat_mouse = 0
CONF
fi
echo "desktop mode installed - on the PC run pc\\scrcpy-desktop.bat (or pc\\scrcpy-mirror.bat)"
