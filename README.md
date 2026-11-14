# Ubuntu Touch (Halium 16) for OnePlus Nord N30 5G / Nord CE 3 Lite 5G — `larry`

Community port, **work in progress / first-boot stage**. Expect things to be broken.

| | |
|---|---|
| SoC | Qualcomm SM6375 Snapdragon 695 (holi / blair) |
| Kernel | LineageOS `android_kernel_oneplus_sm6375` @ `lineage-23.2` (5.4.302 QGKI) + `kernel-config/halium.config` |
| Android base (vendor) | Stock OxygenOS 16 (Android 16) |
| Halium | 16 (GSI-style, `halium-generic-adaptation-build-tools`) |
| Ubuntu Touch | 24.04-2.x |
| Boot | header v3: `boot` + `vendor_boot` + `dtbo`, A/B, no recovery partition |

## Layout

```
deviceinfo                         device + boot image description
build.sh                           wrapper: fetches tools, injects halium.config, applies kernel patches, builds
kernel-config/halium.config        kernel fragment merged on top of vendor/holi-qgki_defconfig
kernel-patches/                    port kernel patches (required)
vendor-ramdisk-overlay/            modules loaded by the Halium initramfs (audio, BT, camera, rmnet)
overlay/system/                    files dropped into the Ubuntu Touch rootfs (required)
extras/                            OPTIONAL add-ons - see "Optional extras" below
extras.conf                        which extras build.sh bakes into the image
flash-larry.bat                    Windows flashing script
out/                               build output (boot.img, vendor_boot.img, dtbo.img, ubuntu.img) - not in git
```

## Optional extras

The port itself (kernel patches + `overlay/`) is everything needed for the hardware to work.
Extras live in [`extras/`](extras/), clearly separated, and can be left out:

| Extra | Summary | Default |
|---|---|---|
| [`touchboost`](extras/touchboost/) | GPU/CPU clock floors raised for 2 s after each touch (~119 fps scrolling instead of ~60) | baked in |
| [`android-gestures`](extras/android-gestures/) | left-edge swipe: upper half = Back, lower half = launcher | baked in |
| [`desktop-mode`](extras/desktop-mode/) | use the phone from a PC over USB with scrcpy (virtual 1080p desktop or mirror, keyboard/mouse) | install on phone |
| [`libertine-gpu`](extras/libertine-gpu/) | experimental GPU acceleration for desktop apps in a Libertine container | install on phone |

Build without any: `EXTRAS="" ./build.sh`, or edit [`extras.conf`](extras.conf). Details in [`extras/README.md`](extras/README.md).

## Building (Linux x86_64 or WSL2, ~30 GB free, Ubuntu 22.04+ packages)

```bash
sudo apt install bc bison build-essential ca-certificates cpio curl fakeroot flex git kmod \
  libelf-dev libssl-dev lz4 python3 python-is-python3 unzip wget xz-utils img2simg jq zstd
./build.sh                                   # kernel + boot/vendor_boot/dtbo -> out/
./build/prepare-fake-ota.sh out/device_larry.tar.xz ota
./build/system-image-from-ota.sh ota/ubuntu_command out   # -> out/rootfs.img + out/system.img (sparse)
simg2simg out/system.img out/ubuntu.img 398458880         # optional: split into <400 MB flashable chunks
```

Build notes (handled by `build.sh`): LTO/CFI are disabled to keep RAM use low; `CONFIG_CC_WERROR`
and a few `-Wno-error=` KCFLAGS work around vendor code that clang 18 (r510928) is stricter about;
`CONFIG_SECTION_MISMATCH_WARN_ONLY` covers an Oplus `__initdata` reference that LTO normally hides;
a python3 `mkdtboimg.py` + `python2` shim replace the python2-only one on modern distros.

## Flashing

**This wipes the phone.** You need an unlocked bootloader and up-to-date Android platform-tools (`fastboot` 35+).

### 0. Back up the stock partitions first (with root, while still in OxygenOS)

```bash
adb shell getprop ro.boot.slot_suffix        # e.g. _a  -> use that suffix below
adb shell su -c "dd if=/dev/block/by-name/vendor_boot_a of=/sdcard/vendor_boot.img"
adb shell su -c "dd if=/dev/block/by-name/dtbo_a        of=/sdcard/dtbo.img"
adb shell su -c "dd if=/dev/block/by-name/vbmeta_a      of=/sdcard/vbmeta.img"
adb pull /sdcard/vendor_boot.img ; adb pull /sdcard/dtbo.img ; adb pull /sdcard/vbmeta.img
```
Keep these with the stock `boot.img` — they are how you get back to Android.

Prebuilt images are attached to the GitHub Releases (or build them yourself, see above). The 4.5 GB rootfs is split into 9 sparse chunks
(`ubuntu.img.0` … `ubuntu.img.8`) that are flashed one after another onto the same partition —
no extra tools needed. On Windows, copy your stock `vbmeta.img` next to the images and run
`flash-larry.bat a` (or `b`) from that folder with the phone in bootloader mode. The manual steps:

### 1. Bootloader mode (`adb reboot bootloader`)

```bash
fastboot --disable-verity --disable-verification flash vbmeta_a vbmeta.img   # the stock vbmeta you dumped
fastboot flash boot_a        boot.img
fastboot flash vendor_boot_a vendor_boot.img
fastboot flash dtbo_a        dtbo.img
fastboot format:ext4 userdata
```

### 2. fastbootd (userspace fastboot) to make room for the rootfs

```bash
fastboot reboot fastboot
fastboot delete-logical-partition product_a
fastboot delete-logical-partition system_ext_a
fastboot resize-logical-partition system_a 4718592000
fastboot flash system_a ubuntu.img.0     # ...repeat for .1 through .8, in order
fastboot reboot
```
Replace `_a` with your active slot if it is `_b`. If fastbootd complains about a snapshot,
run `fastboot snapshot-update cancel` first.

### Going back to Android
Flash the stock `boot.img`, `vendor_boot.img`, `dtbo.img`, `vbmeta.img` and reflash a full OxygenOS
package (or LineageOS 23.2) — `product`/`system_ext` were deleted, so a full ROM install is required.

## Debugging the first boot

If the screen stays black, plug USB into a Linux PC:
* `dmesg -w` on the PC — look for a new `RNDIS` / `usb0` network interface.
* `telnet 192.168.2.15` when the initramfs fails (Halium debug shell),
  `ssh phablet@10.15.19.82` (password `0000`) once Ubuntu Touch is up.
* On device: `cat /proc/last_kmsg` or `/sys/fs/pstore/console-ramoops*` after a reboot loop.

## Status (release r5)

| Works | Notes |
|---|---|
| Boot, display (Lomiri), GPU | Adreno 619, OpenGL ES 3.2 via hwcomposer |
| Touchscreen | needs kernel patch 0002 |
| Wi-Fi | wlan.ko loaded + switched on by `device-hacks` |
| Audio (speaker) | PulseAudio loads the vendor audio HAL in-process |
| USB networking + ADB (Developer Mode) | `dummy_hcd` disabled, vendor USB rc stubbed |
| Modems (ofono, 2 slots) | online with IMEIs; calls/SMS untested (no SIM yet) |
| Camera (photo 12 MP, video up to 4K) | needs kernel patch 0003 + `media.settings.xml` prop; 108 MP mode is vendor-private |
| 120 Hz | panel already runs 120 Hz; scrolling ~60 fps, ~119 fps with the optional `touchboost` extra |
| Sensors (light, proximity, compass, gyro, barometer, steps) | sensorfw 0.15.2 binder plugin (AIDL sensors HAL) - see below |
| GPS | works through the halium shim (HIDL GNSS 2.1); apps get it via the location-service permission prompt |
| Fingerprint (side sensor) | biometryd 0.4.1 with libgbinder (no libbiometry_fp_api.so in the GSI) |
| MTP | kernel patch 0004 (Android f_mtp, Still Image class) |
| Back gesture | optional `android-gestures` extra: left edge upper half = Back, lower half = launcher |
| Display out | no DisplayPort on this SoC; optional `desktop-mode` extra streams a desktop to a PC over USB |

## What it took (for other ports on Android 16 / 5.4 QGKI)

* `kernel-patches/0001` - binder: when the LSM (AppArmor) has no secctx, hand context managers a dummy
  context instead of failing the transaction. Android 16 `vndservicemanager` requires one; without it
  `display.qservice` can't register and the display composer exits (status 234).
* `kernel-patches/0002` - Oplus touch driver enables ABS_MT_WIDTH_MAJOR/PRESSURE without ranges;
  libinput rejects the device ("min == max").
* `vendor/etc/init/vndservicemanager.rc` - LD_PRELOAD libselinux_stubs.so (SELinux check aborts otherwise).
* `vendor/etc/init/android.hardware.usb.gadget-service.qti.rc`, `hw/init.qcom.usb.rc` - stop Android
  from owning the USB gadget so usb-moded can.
* `lib/udev/rules.d/70-android.rules` - generated from ueventd.rc (converted from ueventd.rc), numeric AIDs.
* `kernel-patches/0003` - AppArmor never records the peer label of AF_UNIX sockets on this kernel
  (that's Ubuntu's af_unix mediation, not upstream), so dbus reports an empty AppArmor context and
  media-hub aborts ("apparmor::Context cannot be created for empty name") - which kills the Camera app.
  Fall back to the label of the socket's peer credentials.
* `vendor/etc/init/vndservicemanager.rc` also carries `setprop media.settings.xml
  /vendor/etc/media_profiles_vendor.xml`: MediaProfiles only finds the vendor's file through that
  property; without it camera API1 caps video at 352x288.
* Ubuntu Touch has no power-HAL input boost, so the Adreno 619 sits at 266 MHz (msm-adreno-tz) and
  scrolling tops out around 60 fps on the 120 Hz panel - see the optional `extras/touchboost`.
* `kernel-patches/0004` - Android's f_mtp from 4.19 (mtp-server needs /dev/mtp_usb). Two changes:
  link resets are reported as ECANCELED (mtp-server exits on any other error and Windows then
  resets again, in a loop), and the interface is class 6/1/1 instead of vendor 0xff, because
  usb-moded sets up no Microsoft OS descriptors and Windows would load no driver.
* Sensors: this vendor exposes only the AIDL `android.hardware.sensors.ISensors`; the 24.04-2.x
  sensorfw 0.15.1 only speaks HIDL. Overlay carries `libhidlsensorfw-qt5.so.1.0.0` + `sensord-qt5/libhidl*`
  built from UBports' packaging (ubports/latest = 0.15.2, upstream AIDL backend) with CONFIG+=hybris
  CONFIG+=binder; only the hybris backend changed vs 0.15.1, so the installed core libs are reused.
* Fingerprint: overlay carries `biometryd` + `libbiometry.so.2.0.0` from biometryd 0.4.1 built with
  -DWITH_GBINDER=ON (talks to fingerprint@2.1 + gatekeeper@1.0 over hwbinder). The 24.04-2.x 0.4.0
  needs libbiometry_fp_api.so, which the halium-16 GSI doesn't ship. Drop both overlays once the
  24.04-2.x archive gets these versions.
* `device-hacks` - `/dev/ashmem<boot_id>` (Android 16), load our wlan.ko, `echo ON > /dev/wlan`.
* `halium.config` - AppArmor, no LTO/CFI, `CONFIG_LOCALVERSION_AUTO=n` (modules always match),
  no `CONFIG_USB_DUMMY_HCD`, `CONFIG_CC_WERROR=n`, `CONFIG_SECTION_MISMATCH_WARN_ONLY=y`.

## Known TODO

- [ ] Calls / SMS / mobile data with a SIM (ofono binder config is in place)
- [ ] Bluetooth (bluebinder fails to start)
- [ ] AppArmor: apparmor.service fails - 5.4 lacks Ubuntu's AppArmor patches (0003 covers dbus peer labels only)
- [ ] Audio calibration libs (libacdbloader) absent on this vendor; headset/mic not tested
- [ ] Display cutout + grid unit tuning in halium.yaml; recovery; UBports Installer config

## Licence

GPL-2.0-or-later for the original work in this repository. Kernel patches are GPL-2.0-only, and files
copied from Lomiri or built from sensorfw and biometryd keep their upstream licences.
See [`LICENSES.md`](LICENSES.md).
