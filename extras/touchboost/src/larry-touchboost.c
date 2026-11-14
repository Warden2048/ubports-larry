/*
 * larry-touchboost - Android-style input boost for Ubuntu Touch on the OnePlus Nord N30 (larry).
 *
 * Android's power HAL raises GPU/CPU floors while the user touches the screen. Ubuntu Touch has
 * no equivalent, so the Adreno 619 idles at 266 MHz and scrolling only reaches ~60 fps on the
 * 120 Hz panel. With the floor raised it holds ~119 fps. This daemon watches the touchscreen and
 * raises the floors for HOLD_MS after the last touch event, then puts the previous values back.
 *
 * Usage: larry-touchboost [-g gpu_hz] [-c cpu_percent] [-t hold_ms] [-v]
 *   -g  GPU devfreq floor while boosted (default: the GPU's max frequency)
 *   -c  CPU floor as % of each policy's max frequency (default 60)
 *   -t  how long to stay boosted after the last touch event (default 2000 ms)
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GPU_DEVFREQ "/sys/class/kgsl/kgsl-3d0/devfreq"
#define CPUFREQ     "/sys/devices/system/cpu/cpufreq"
#define MAX_DEVS    8
#define MAX_POLICIES 8

static int verbose;
static volatile sig_atomic_t stop;

static void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "larry-touchboost: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static long read_long(const char *path) {
    char buf[64]; long v = -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) { buf[n] = 0; v = strtol(buf, NULL, 10); }
    return v;
}

static int write_long(const char *path, long v) {
    char buf[32]; int len = snprintf(buf, sizeof(buf), "%ld", v);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int r = write(fd, buf, len) == len ? 0 : -1;
    close(fd);
    return r;
}

struct knob { char path[320]; long boost; long saved; int changed; };
static struct knob knobs[1 + MAX_POLICIES];
static int nknobs, boosted;

static void boost_on(void) {
    for (int i = 0; i < nknobs; i++) {
        knobs[i].saved = read_long(knobs[i].path);
        knobs[i].changed = knobs[i].saved >= 0 && knobs[i].saved < knobs[i].boost
                           && write_long(knobs[i].path, knobs[i].boost) == 0;
    }
    boosted = 1;
    if (verbose) logmsg("boost on");
}

static void boost_off(void) {
    for (int i = 0; i < nknobs; i++)
        if (knobs[i].changed) {   /* only undo what we changed (the kernel may round CPU values) */
            write_long(knobs[i].path, knobs[i].saved);
            knobs[i].changed = 0;
        }
    boosted = 0;
    if (verbose) logmsg("boost off");
}

static long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void on_signal(int s) { (void)s; stop = 1; }

static char open_names[MAX_DEVS][32];
static char open_names_dev[MAX_DEVS][128];

/* Interactive input: the touchscreen, and scrcpy's virtual (UHID) mouse/keyboard. */
static int wanted(const char *name) { return !strcmp(name, "touchpanel") || !strncmp(name, "scrcpy", 6); }

static int open_touch_devices(struct pollfd *pfds, int n) {
    DIR *d = opendir("/sys/class/input");
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d)) && n < MAX_DEVS) {
        char p[320], name[128] = "";
        if (strncmp(e->d_name, "event", 5)) continue;
        snprintf(p, sizeof(p), "/sys/class/input/%s/device/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(name, sizeof(name), f)) name[0] = 0;
        fclose(f);
        name[strcspn(name, "\n")] = 0;
        if (!wanted(name)) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp(open_names[i], e->d_name)) dup = 1;
        if (dup) continue;
        snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { logmsg("open %s: %s", p, strerror(errno)); continue; }
        pfds[n].fd = fd; pfds[n].events = POLLIN;
        snprintf(open_names[n], sizeof(open_names[0]), "%s", e->d_name);
        snprintf(open_names_dev[n], sizeof(open_names_dev[0]), "%s", name); n++;
        logmsg("watching %s (%s)", p, name);
    }
    closedir(d);
    return n;
}

int main(int argc, char **argv) {
    long gpu_hz = -1, hold_ms = 2000; int cpu_pct = 60, opt;
    while ((opt = getopt(argc, argv, "g:c:t:v")) != -1) {
        switch (opt) {
        case 'g': gpu_hz = atol(optarg); break;
        case 'c': cpu_pct = atoi(optarg); break;
        case 't': hold_ms = atol(optarg); break;
        case 'v': verbose = 1; break;
        default: fprintf(stderr, "usage: %s [-g gpu_hz] [-c cpu_percent] [-t hold_ms] [-v]\n", argv[0]); return 2;
        }
    }

    /* GPU floor */
    if (gpu_hz < 0) gpu_hz = read_long(GPU_DEVFREQ "/max_freq");
    if (gpu_hz > 0) {
        snprintf(knobs[nknobs].path, sizeof(knobs[0].path), GPU_DEVFREQ "/min_freq");
        knobs[nknobs++].boost = gpu_hz;
    }
    /* CPU floors */
    DIR *d = opendir(CPUFREQ);
    struct dirent *e;
    while (d && (e = readdir(d)) && nknobs < 1 + MAX_POLICIES) {
        char p[320];
        if (strncmp(e->d_name, "policy", 6)) continue;
        snprintf(p, sizeof(p), CPUFREQ "/%s/cpuinfo_max_freq", e->d_name);
        long max = read_long(p);
        if (max <= 0 || cpu_pct <= 0) continue;
        snprintf(knobs[nknobs].path, sizeof(knobs[0].path), CPUFREQ "/%s/scaling_min_freq", e->d_name);
        knobs[nknobs++].boost = max * cpu_pct / 100;
    }
    if (d) closedir(d);
    for (int i = 0; i < nknobs; i++) logmsg("knob %s -> %ld", knobs[i].path, knobs[i].boost);

    struct pollfd pfds[MAX_DEVS];
    int ndev = 0;
    while (!stop && (ndev = open_touch_devices(pfds, 0)) == 0) sleep(2);   /* wait for udev */

    signal(SIGTERM, on_signal); signal(SIGINT, on_signal);
    long deadline = 0, next_scan = now_ms() + 2000;
    while (!stop) {
        /* A scrcpy session (its virtual input devices exist) keeps the boost on for its whole
         * duration: screen capture + hardware encode need the GPU clock even when nobody types. */
        int session = 0;
        for (int i = 0; i < ndev; i++) if (!strncmp(open_names_dev[i], "scrcpy", 6)) session = 1;
        if (session && !boosted) boost_on();
        long now = now_ms();
        long wake = next_scan;
        if (boosted && !session && deadline < wake) wake = deadline;
        int timeout = (int)(wake - now); if (timeout < 0) timeout = 0;
        int r = poll(pfds, ndev, timeout);
        if (r < 0) { if (errno == EINTR) continue; logmsg("poll: %s", strerror(errno)); break; }
        now = now_ms();
        if (now >= next_scan) { ndev = open_touch_devices(pfds, ndev); next_scan = now + 2000; }
        int got = 0;
        for (int i = 0; r > 0 && i < ndev; i++) {
            if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                logmsg("input %s went away", open_names[i]);
                close(pfds[i].fd);
                pfds[i] = pfds[ndev - 1]; memcpy(open_names[i], open_names[ndev - 1], sizeof(open_names[0]));
                memcpy(open_names_dev[i], open_names_dev[ndev - 1], sizeof(open_names_dev[0]));
                pfds[ndev - 1].revents = 0;
                ndev--; i--; continue;
            }
            if (!(pfds[i].revents & POLLIN)) continue;
            struct input_event ev[64];
            while (read(pfds[i].fd, ev, sizeof(ev)) > 0) got = 1;   /* drain */
        }
        if (got) {
            deadline = now + hold_ms;
            if (!boosted) boost_on();
        }
        int still = 0;
        for (int i = 0; i < ndev; i++) if (!strncmp(open_names_dev[i], "scrcpy", 6)) still = 1;
        if (boosted && !still && now >= deadline) boost_off();
    }
    if (boosted) boost_off();
    return 0;
}
