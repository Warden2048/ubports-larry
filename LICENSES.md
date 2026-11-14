# Licensing

This repository mixes original work with files taken from, or built from, other projects.
Each part keeps the licence it has to have:

| Part | Licence |
|---|---|
| Original work in this repository: build/flash scripts, configs, `overlay/` text files, `extras/*` sources and install scripts | **GPL-2.0-or-later** (see `LICENSE`) |
| `kernel-patches/`, `kernel-config/` (changes to the Linux kernel) | **GPL-2.0-only**, like the kernel they patch |
| `extras/android-gestures/.../Launcher.qml` (modified copy from Lomiri) | **GPL-3.0-only** (upstream licence, header kept) |
| `extras/android-gestures/.../MainView.qml` (modified copy from lomiri-ui-toolkit) | **LGPL-3.0-only** (upstream licence, header kept) |
| `overlay/.../libhidlsensorfw-qt5.so*`, `sensord-qt5/libhidl*.so` (built from sensorfw 0.15.2) | **LGPL-2.1-or-later** (sensorfw) |
| `overlay/usr/bin/biometryd`, `libbiometry.so*` (built from biometryd 0.4.1) | **LGPL-3.0** (biometryd) |
| `extras/libertine-gpu` installs Mesa from [lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) at install time (not redistributed here) | Mesa: MIT and others |

The build downloads further components it doesn't ship, under their own licences: the LineageOS kernel source,
halium-generic-adaptation-build-tools (Apache-2.0), and the Halium and Ubuntu Touch images.
