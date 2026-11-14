#!/bin/sh
# EXPERIMENTAL / UNTESTED: flat (1:1, no acceleration) movement for the scrcpy mouse only.
# Loads libflat-scrcpy.so into lomiri-system-compositor (started by lightdm) through a wrapper.
# Run ON THE PHONE as root:  sudo sh install.sh   then reboot.   Undo: sudo sh install.sh --uninstall
# If the screen stays black after the reboot, adb still works: run the uninstall and reboot.
set -e
cd "$(dirname "$0")"
mount -o remount,rw /
if [ "$1" = "--uninstall" ]; then
    rm -f /etc/lightdm/lightdm.conf.d/99-flat-scrcpy.conf /usr/local/bin/lsc-flat-wrapper /usr/local/lib/libflat-scrcpy.so
    mount -o remount,ro / 2>/dev/null || true; echo "removed - reboot"; exit 0
fi
KEYLINE=$(grep -rh "^ *unity-compositor-command *=\|^ *lomiri-compositor-command *=" /etc/lightdm/ /usr/share/lightdm/ 2>/dev/null \
          | grep -v lsc-flat-wrapper | tail -1)
[ -n "$KEYLINE" ] || { echo "lightdm compositor-command not found - not installed"; mount -o remount,ro / || true; exit 1; }
KEY=$(echo "$KEYLINE" | sed 's/ *=.*//; s/^ *//')
CMD=$(echo "$KEYLINE" | sed 's/^[^=]*= *//')
BIN=$(echo "$CMD" | awk '{print $1}'); ARGS=$(echo "$CMD" | cut -s -d' ' -f2-)
[ -x "$BIN" ] || BIN=$(command -v "$BIN")
install -m 644 libflat-scrcpy.so /usr/local/lib/libflat-scrcpy.so
cat > /usr/local/bin/lsc-flat-wrapper <<WRAP
#!/bin/sh
[ -r /usr/local/lib/libflat-scrcpy.so ] && export LD_PRELOAD="\${LD_PRELOAD:+\$LD_PRELOAD }/usr/local/lib/libflat-scrcpy.so"
exec $BIN "\$@"
WRAP
chmod 755 /usr/local/bin/lsc-flat-wrapper
SEAT=$(grep -rl "$KEY" /etc/lightdm/ /usr/share/lightdm/ 2>/dev/null | grep -v 99-flat-scrcpy | xargs grep -h "^\[" | tail -1)
mkdir -p /etc/lightdm/lightdm.conf.d
printf '%s\n%s=/usr/local/bin/lsc-flat-wrapper %s\n' "${SEAT:-[Seat:*]}" "$KEY" "$ARGS" > /etc/lightdm/lightdm.conf.d/99-flat-scrcpy.conf
sync; mount -o remount,ro / 2>/dev/null || true
echo "installed - reboot the phone"
