# libertine-gpu (optional, experimental)

Desktop (X11) apps in a Libertine container normally render in software (llvmpipe), because Ubuntu's
Mesa needs the upstream `msm` kernel driver and this phone has Qualcomm's `kgsl`.
[lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) builds Mesa
with kgsl support. This extra installs it into the container.

| | Result on the Adreno 619 |
|---|---|
| Vulkan | turnip on kgsl |
| OpenGL 4.6 | zink on turnip. Reports `zink Vulkan 1.3(Turnip Adreno (TM) 619)`. |
| glmark2 (800x600, 4 scenes) | GPU 95-111 vs software 22-23. On the real desktop (Xwayland): 231. |

How it fits together:

* The X servers here (Xwayland, Xvfb) have no **DRI3**, so finished frames are copied to X by the
  CPU (`MESA_VK_WSI_DEBUG=sw`, `LIBGL_KOPPER_DRI2=1`). Fine for real rendering work. Trivial demos
  like `glxgears` are limited by that copy (~300 fps either way).
* Libertine doesn't pass `/etc/environment` to apps, so `libmesa-env.so` (in the container's
  `/etc/ld.so.preload`) sets the Mesa variables for every program. Anything set explicitly wins.
  `LARRY_GPU=0 <app>` opts one app out.
* The Mesa packages are put on hold inside the container so `apt upgrade` doesn't swap in stock (software) Mesa again.
* **Known gap:** EGL window apps (e.g. `es2gears_x11`) hang. GLX apps work.

Install (phone online): `sudo sh install.sh [container]`. The default container name is `noble`.
Uninstall: `sudo sh install.sh [container] --uninstall`.
