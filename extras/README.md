# Optional extras

Nothing in this folder is needed for Ubuntu Touch to boot or for any hardware to work. These are
quality-of-life additions on top of the port. Each one can be left out.

| Extra | What it does | How it's installed | Default | Status |
|---|---|---|---|---|
| [`touchboost`](touchboost/) | Raises the GPU clock floor to max and the CPU floors to 60 % for 2 s after each touch (and for the whole of a desktop-mode session), so scrolling reaches ~119 fps on the 120 Hz panel instead of ~60. Costs some battery while you touch the screen. | Baked into the image by `build.sh` (or `install.sh` on a flashed phone) | **on** | stable |
| [`android-gestures`](android-gestures/) | Android-style back gesture: swipe in from the left edge - upper half = Back, lower half = app launcher. Replaces two stock QML files (Lomiri launcher, toolkit MainView). | Baked into the image by `build.sh` | **on** | stable |
| [`desktop-mode`](desktop-mode/) | Use the phone from a PC over USB with [scrcpy](https://github.com/Genymobile/scrcpy): a 1920x1080 Lomiri desktop on a virtual second screen (or a mirror of the phone screen), PC keyboard and mouse, hardware H.265/H.264 encoding at 60 fps. | `install.sh` on the phone + `.bat` files on the PC | off | works; see notes |
| [`libertine-gpu`](libertine-gpu/) | GPU-accelerated OpenGL/Vulkan for desktop apps in a Libertine container (Mesa turnip + zink on the kgsl driver). ~4x software rendering in glmark2. Changes only the container. | `install.sh` on the phone | off | experimental |

## Choosing extras for a build

`build.sh` bakes in the extras listed in [`../extras.conf`](../extras.conf). Edit that file, or override it:

```bash
EXTRAS="" ./build.sh                 # plain port, no extras
EXTRAS="touchboost" ./build.sh       # only the touch boost
```

`build.sh` adds them to `out/device_larry.tar.xz` straight into the rootfs (not into `/opt/halium-overlay`,
which can only shadow files that already exist).
`desktop-mode` and `libertine-gpu` are never baked in. Install them on a running phone as described in their READMEs.
