#!/bin/sh
# Install / remove the touch boost on a phone that was flashed without it.
# Run ON THE PHONE as root from this folder:  sudo sh install.sh   |   sudo sh install.sh --uninstall
set -e
cd "$(dirname "$0")"
mount -o remount,rw /
if [ "$1" = "--uninstall" ]; then
    systemctl disable --now larry-touchboost 2>/dev/null || true
    rm -f /usr/bin/larry-touchboost /etc/systemd/system/larry-touchboost.service
    mount -o remount,ro / 2>/dev/null || true; echo "touch boost removed"; exit 0
fi
install -m 755 overlay/system/usr/bin/larry-touchboost /usr/bin/larry-touchboost
install -m 644 overlay/system/etc/systemd/system/larry-touchboost.service /etc/systemd/system/
sync; mount -o remount,ro / 2>/dev/null || true
systemctl daemon-reload; systemctl enable --now larry-touchboost
echo "touch boost installed and running"
