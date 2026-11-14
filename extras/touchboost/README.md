# touchboost (optional)

Ubuntu Touch has no Android power-HAL input boost, so the Adreno 619 idles at 266 MHz (msm-adreno-tz)
and scrolling tops out around 60 fps on the 120 Hz panel. `larry-touchboost` watches the `touchpanel`
input device (and scrcpy's virtual devices) and, for 2 s after the last event, sets the GPU devfreq
floor to max and the CPU floors to 60 % of max. While a desktop-mode session is connected, the boost
stays on for the whole session.

* Files: `overlay/system/usr/bin/larry-touchboost`, `larry-touchboost.service` (+ its `multi-user.target.wants` link)
* Leave out of a build: remove `touchboost` from `extras.conf`
* Add to / remove from a flashed phone: `sudo sh install.sh` / `sudo sh install.sh --uninstall`
* Options: `larry-touchboost [-g gpu_hz] [-c cpu_percent] [-t hold_ms] [-v]`
* Rebuild: `aarch64-linux-gnu-gcc -O2 -o overlay/system/usr/bin/larry-touchboost src/larry-touchboost.c`
