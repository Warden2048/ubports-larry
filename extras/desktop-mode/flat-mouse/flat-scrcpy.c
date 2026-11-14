/*
 * flat-scrcpy: preloaded into the system compositor (lomiri-system-compositor).
 * The compositor gives every mouse libinput's adaptive (accelerated) pointer profile and only lets
 * Settings change the acceleration *speed*. For the scrcpy mouse (the PC's mouse, which the PC has
 * already accelerated - or not - to the user's taste) this forces the flat profile at speed 0:
 * one PC mouse count = one pixel, no acceleration. Every other device is left untouched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
/* minimal libinput declarations (ABI-stable since libinput 1.1) */
struct libinput_device;
enum libinput_config_status { LIBINPUT_CONFIG_STATUS_SUCCESS = 0 };
enum libinput_config_accel_profile { LIBINPUT_CONFIG_ACCEL_PROFILE_NONE = 0,
    LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT = (1 << 0), LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE = (1 << 1) };

static int is_scrcpy(struct libinput_device *d) {
    static const char *(*get_name)(struct libinput_device *);
    if (!get_name) get_name = (const char *(*)(struct libinput_device *))dlsym(RTLD_DEFAULT, "libinput_device_get_name");
    const char *n = get_name ? get_name(d) : 0;
    return n && !strncmp(n, "scrcpy", 6);
}

enum libinput_config_status libinput_device_config_accel_set_profile(struct libinput_device *d,
                                                                   enum libinput_config_accel_profile p) {
    static enum libinput_config_status (*real)(struct libinput_device *, enum libinput_config_accel_profile);
    if (!real) real = dlsym(RTLD_NEXT, "libinput_device_config_accel_set_profile");
    if (is_scrcpy(d)) p = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
    return real(d, p);
}

enum libinput_config_status libinput_device_config_accel_set_speed(struct libinput_device *d, double speed) {
    static enum libinput_config_status (*real)(struct libinput_device *, double);
    static enum libinput_config_status (*setp)(struct libinput_device *, enum libinput_config_accel_profile);
    if (!real) real = dlsym(RTLD_NEXT, "libinput_device_config_accel_set_speed");
    if (!setp) setp = dlsym(RTLD_NEXT, "libinput_device_config_accel_set_profile");
    if (is_scrcpy(d)) { setp(d, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT); speed = 0.0; }
    return real(d, speed);
}
