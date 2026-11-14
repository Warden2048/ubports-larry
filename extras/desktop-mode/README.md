# desktop-mode (optional)

The SoC has no DisplayPort output, so this streams the screen to a PC over USB instead.
`larry-scrcpy-server` is a [scrcpy](https://github.com/Genymobile/scrcpy) (v5 protocol) compatible
server for Ubuntu Touch, so the stock scrcpy client for Windows/Linux/macOS works unchanged.

* **Desktop:** `scrcpy --new-display=1920x1080` makes Mir create a virtual second screen that Lomiri
  uses as a desktop monitor; that screen is what gets streamed.
* **Mirror:** without `--new-display`, it streams the phone screen.
* **Video:** Mir screencast, then the Qualcomm hardware encoder through libhybris `MediaCodecSource`,
  zero-copy. H.265 or H.264, 60 fps. The encoder can't do 1080p at 120 fps.
* **Input:** scrcpy UHID keyboard and mouse (`--keyboard=uhid --mouse=uhid`, mouse captured, Left-Alt
  releases it). Wheel speed is set in the config.

## Install

1. Enable Developer Mode on the phone (Settings > About > Developer Mode) so `adb` works.
2. Install the server on the phone:
   ```bash
   adb push extras/desktop-mode /tmp/desktop-mode
   adb shell sudo sh /tmp/desktop-mode/install.sh        # asks for the phone passcode
   ```
3. On the PC, unzip scrcpy (5.x) into `scrcpy\` next to the `pc\` folder (or put it on PATH) and run
   `pc\scrcpy-desktop.bat` (desktop) or `pc\scrcpy-mirror.bat` (mirror).
   If your PC can't decode H.265, add `--video-codec=h264`.

Settings: `~/.config/larry-scrcpy.conf` on the phone, read at each connect. The defaults are documented in the file.
Uninstall: `sudo sh install.sh --uninstall`. The [`touchboost`](../touchboost/) extra keeps the GPU clocked up
during a session; without it, expect ~45 fps instead of 60.

## Notes / limits

* Latency is about 30 ms from capture to encoded frame, plus decode and display on the PC.
* Mouse movement goes through Lomiri's normal pointer acceleration. [`flat-mouse/`](flat-mouse/)
  (**experimental, untested**) loads a small libinput shim into the system compositor to make the
  scrcpy mouse 1:1. Don't use the server's `flat_mouse = 1` option: that route replaces the input
  configuration and leaves the keyboard dead until the next reboot.
* Turning the phone screen off with the power button also turns off the virtual desktop screen.
* Build: see the header of `src/server.cpp`. It links against Mir 1.x client (`libmir1client`) and libhybris `libmedia`.
