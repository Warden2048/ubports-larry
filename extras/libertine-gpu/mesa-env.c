/*
 * mesa-env: preloaded into every program in the Libertine container (/etc/ld.so.preload).
 * Libertine doesn't pass /etc/environment to apps, so this sets the Mesa defaults that make
 * OpenGL run on the Adreno GPU: zink (OpenGL on Vulkan) on turnip (Vulkan on kgsl), with the
 * finished frames copied to the X server by the CPU (this phone's X server has no DRI3).
 * Anything already set in the environment wins; LARRY_GPU=0 turns it all off.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void larry_mesa_env(void) {
    const char *off = getenv("LARRY_GPU");
    if ((off && !strcmp(off, "0")) || getenv("LIBGL_ALWAYS_SOFTWARE")) return;
    if (access("/dev/kgsl-3d0", R_OK | W_OK) != 0) return;
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 0);
    setenv("MESA_VK_WSI_DEBUG", "sw", 0);
    setenv("LIBGL_KOPPER_DRI2", "1", 0);
    setenv("TU_DEBUG", "noconform", 0);
}
