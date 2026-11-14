/*
 * Fan Control for Linux (Lenovo IdeaPad 3 14ALC6 and its 15ALC6 sibling)
 *
 * Native GTK 3 app. At runtime it needs only libraries every Linux desktop
 * already ships (glibc + GTK 3) and the in-kernel ideapad_laptop driver.
 *
 * How the fan is controlled (found by reverse engineering, see tools/)
 * ----------------------------------------------------------------------
 * The embedded controller (EC) is an ITE IT8227. Three things matter:
 *
 * 1. PWM duty register DCR4, at 0x1806 in the EC's internal register space,
 *    drives the fan: 0 = stopped, 200 = full speed (about 5200 rpm). The
 *    firmware only nudges this register by one step per second towards its
 *    own target, so rewriting it a few times a second holds any speed
 *    steadily. It is reached through the chip's "I2EC" window on the Super
 *    I/O ports 0x4E/0x4F, which needs /dev/port, and the kernel only allows
 *    that with Secure Boot OFF.
 *
 * 2. Byte 0x06 of the EC's RAM ("FANS" in the DSDT) is the measured speed in
 *    units of 100 rpm. It is readable through the in-kernel ec_sys module,
 *    also with Secure Boot on.
 *
 * 3. /sys/bus/platform/drivers/ideapad_acpi/<dev>/fan_mode has just two
 *    states on this firmware, whatever the kernel documentation says about
 *    "silent / standard / performance":
 *      write 0          automatic control by the firmware  (reads back 133)
 *      write 1, 2 or 4  dust cleaning: about 10 s at full speed, a 1 s
 *                       stop, repeating
 *
 * Manual speed runs as a small root service and uses the best method there is:
 *   1. The ideapad_fan_pwm kernel driver (kmod/), if loaded. It writes the
 *      PWM duty from inside the kernel, which works with Secure Boot ON once
 *      the module is signed with the owner's own key. Steady speed.
 *   2. Writing the PWM duty through /dev/port. Steady speed, but the kernel
 *      only allows it with Secure Boot OFF.
 *   3. Switching fan_mode between full speed and automatic so that the
 *      average measured speed matches the percentage. Works everywhere, but
 *      the speed swings and never averages below automatic control.
 *
 * Safety: with direct PWM the fan can be set slower than the firmware
 * wants, so the service forces full speed while the CPU is at 85 C or more.
 *
 *   FanControl                       open the app
 *   FanControl status|auto|dust|N    command line (N = 30..100 percent)
 */
#include <gtk/gtk.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP_ID       "io.github.ideapadfancontrol"
#define APP_VERSION  "2.1.0"

/* Paths can be overridden at build time to test the speed loop off-hardware. */
#ifndef SYSFS_GLOB
#define SYSFS_GLOB   "/sys/bus/platform/drivers/ideapad_acpi/*/fan_mode"
#endif
#ifndef CONF_FILE
#define CONF_FILE    "/etc/ideapad-fan-control.conf"   /* the wanted percentage */
#endif
#ifndef RUN_FILE
#define RUN_FILE     "/run/ideapad-fan-control"        /* "<percent> <pid> <rpm> <direct>" while the service runs */
#endif
#ifndef EC_IO
#define EC_IO        "/sys/kernel/debug/ec/ec0/io"     /* EC RAM, from the ec_sys module (root only) */
#endif
#define EC_FANS      0x06  /* fan speed in units of 100 rpm */
#define FULL_UNITS   52    /* speed at full blast: about 5200 rpm */

/* The ideapad_fan_pwm kernel driver (kmod/): works with Secure Boot on when signed. */
#define DRIVER_MODULE "ideapad_fan_pwm"
#define DRIVER_HWMON  "ideapad_fan"

/* Direct PWM on the ITE IT8227 embedded controller from user space (needs Secure Boot off). */
#ifndef PORT_DEV
#define PORT_DEV     "/dev/port"
#endif
#define SIO_BASE     0x4E    /* Super I/O index port; data port is +1 */
#define EC_CHIP      0x8227  /* expected chip id at 0x2000/0x2001 */
#define EC_DCR4      0x1806  /* PWM duty of the fan */
#define DUTY_MAX     200
#define TEMP_HOT     85      /* force full speed at or above this CPU temperature */
#define TEMP_COOL    75      /* ... until it is back below this */
#define HELPER       "/usr/local/libexec/ideapad-fan-control"
#define SERVICE_NAME "ideapad-fan-control.service"
#define SERVICE      "/etc/systemd/system/" SERVICE_NAME

/* Left behind by version 1.0, which believed the kernel's mode table. */
#define OLD_SERVICE_NAME "ideapad-fan-mode.service"
#define OLD_SERVICE      "/etc/systemd/system/" OLD_SERVICE_NAME
#define OLD_STATE        "/run/ideapad-fan-mode"

#define FAN_AUTO    0     /* written to fan_mode */
#define FAN_FULL    2
#define RAW_AUTO    133   /* read back from fan_mode while in automatic control */

#define PCT_MIN     30
#define PCT_MAX     100
#define TICK        0.25  /* seconds between speed readings */
#define DWELL       1.0   /* minimum seconds between fan_mode switches */
#define PERIOD      6.0   /* fallback without a speed reading: seconds per on/off cycle */

enum { ST_NA, ST_AUTO, ST_MANUAL, ST_DUST };

/* ------------------------------------------------------------- backend -- */

static gchar *read_file(const char *path)
{
    gchar *s = NULL;
    if (!g_file_get_contents(path, &s, NULL, NULL))
        return NULL;
    return g_strstrip(s);
}

/* Raw value the firmware reports, or -1 when fan_mode is not available. */
static int read_raw(void)
{
    glob_t g;
    int r = -1;
    if (glob(SYSFS_GLOB, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        gchar *v = read_file(g.gl_pathv[0]);
        if (v && *v)
            r = (int)strtol(v, NULL, 0);
        g_free(v);
    }
    globfree(&g);
    return r;
}

/* Percentage of the running manual-speed service, or -1 when it is not running.
 * *rpm gets the fan speed it measured, or -1. */
static int read_driver_rpm(void);
static int manual_direct;   /* set by read_manual: 1 = direct PWM, 0 = fan_mode switching */

static int read_manual(int *rpm)
{
    int pct = -1, pid = 0;
    *rpm = -1;
    manual_direct = 0;
    gchar *v = read_file(RUN_FILE);
    if (v && sscanf(v, "%d %d %d %d", &pct, &pid, rpm, &manual_direct) >= 2 && pid > 0) {
        gchar *p = g_strdup_printf("/proc/%d", pid);
        if (access(p, F_OK) != 0)
            pct = -1; /* stale file */
        g_free(p);
    } else {
        pct = -1;
    }
    g_free(v);
    if (pct < PCT_MIN || pct > PCT_MAX)
        pct = *rpm = -1;
    return pct;
}

static int read_state(int *raw, int *pct, int *rpm)
{
    *raw = read_raw();
    *pct = read_manual(rpm);
    if (*rpm < 0)
        *rpm = read_driver_rpm();
    if (*raw < 0)
        return ST_NA;
    if (*pct > 0)
        return ST_MANUAL;
    return *raw == RAW_AUTO ? ST_AUTO : ST_DUST;
}

/* Temperature of the first hwmon chip with this name, in degrees C, or -1. */
static int read_temp(const char *chip)
{
    glob_t g;
    int t = -1;
    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && t < 0; i++) {
            gchar *n = read_file(g.gl_pathv[i]);
            if (n && !strcmp(n, chip)) {
                gchar *dir = g_path_get_dirname(g.gl_pathv[i]);
                gchar *p = g_build_filename(dir, "temp1_input", NULL);
                gchar *v = read_file(p);
                if (v && *v)
                    t = (int)lround(atof(v) / 1000.0);
                g_free(v); g_free(p); g_free(dir);
            }
            g_free(n);
        }
    }
    globfree(&g);
    return t;
}

/* hwmon directory of the ideapad_fan_pwm kernel driver, or NULL. Caller frees. */
static gchar *driver_dir(void)
{
    glob_t g;
    gchar *dir = NULL;
    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && !dir; i++) {
            gchar *n = read_file(g.gl_pathv[i]);
            if (n && !strcmp(n, DRIVER_HWMON))
                dir = g_path_get_dirname(g.gl_pathv[i]);
            g_free(n);
        }
    }
    globfree(&g);
    return dir;
}

/* Fan speed in rpm from the kernel driver (readable without root), or -1. */
static int read_driver_rpm(void)
{
    int rpm = -1;
    gchar *dir = driver_dir();
    if (dir) {
        gchar *p = g_build_filename(dir, "fan1_input", NULL);
        gchar *v = read_file(p);
        if (v && *v)
            rpm = atoi(v);
        g_free(v); g_free(p); g_free(dir);
    }
    return rpm;
}

static gboolean write_text(const char *dir, const char *name, int value)
{
    gchar *p = g_build_filename(dir, name, NULL);
    FILE *f = fopen(p, "w");
    g_free(p);
    if (!f)
        return FALSE;
    gboolean ok = fprintf(f, "%d", value) > 0;
    return fclose(f) == 0 && ok;
}

static int read_cpu_temp(void)
{
    const char *chips[] = { "k10temp", "zenpower", "coretemp" };
    for (size_t i = 0; i < G_N_ELEMENTS(chips); i++) {
        int t = read_temp(chips[i]);
        if (t >= 0)
            return t;
    }
    return -1;
}

/* Writes a value to fan_mode. Returns how many devices accepted it. */
static int write_fan(int value)
{
    char val[8];
    snprintf(val, sizeof val, "%d", value);
    glob_t g;
    int written = 0;
    if (glob(SYSFS_GLOB, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "w");
            if (!f)
                continue;
            if (fputs(val, f) >= 0 && fclose(f) == 0)
                written++;
        }
    }
    globfree(&g);
    return written;
}

/* "auto", "dust" or a percentage. Returns 0, -1 (dust) or the percentage; -2 if invalid. */
static int parse_target(const char *arg)
{
    if (!strcmp(arg, "auto") || !strcmp(arg, "default"))
        return 0;
    if (!strcmp(arg, "dust"))
        return -1;
    char *end;
    long p = strtol(arg, &end, 10);
    if (end == arg || (*end && strcmp(end, "%")) || p < PCT_MIN || p > PCT_MAX)
        return -2;
    return (int)p;
}

/* -------------------------------------------------- manual-speed service -- */

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void nap(double seconds)
{
    struct timespec ts = { (time_t)seconds, (long)((seconds - floor(seconds)) * 1e9) };
    while (!stop_requested && nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Fan speed from the EC in units of 100 rpm, or -1. */
static int read_fan_units(FILE *ec)
{
    unsigned char b;
    if (!ec || fseek(ec, EC_FANS, SEEK_SET) != 0 || fread(&b, 1, 1, ec) != 1)
        return -1;
    return b;
}

/* --- ITE "I2EC": access to the EC's internal registers through the Super I/O ports --- */

static int port_fd = -1;

static int inb_(int port)
{
    unsigned char b = 0xFF;
    if (pread(port_fd, &b, 1, port) != 1)
        return -1;
    return b;
}

static void outb_(int value, int port)
{
    unsigned char b = (unsigned char)value;
    if (pwrite(port_fd, &b, 1, port) != 1) { /* nothing to do about it here */ }
}

static void i2ec_select(int addr)
{
    outb_(0x2E, SIO_BASE); outb_(0x11, SIO_BASE + 1);
    outb_(0x2F, SIO_BASE); outb_(addr >> 8, SIO_BASE + 1);
    outb_(0x2E, SIO_BASE); outb_(0x10, SIO_BASE + 1);
    outb_(0x2F, SIO_BASE); outb_(addr & 0xFF, SIO_BASE + 1);
    outb_(0x2E, SIO_BASE); outb_(0x12, SIO_BASE + 1);
    outb_(0x2F, SIO_BASE);
}

static int i2ec_read(int addr)
{
    i2ec_select(addr);
    return inb_(SIO_BASE + 1);
}

static void i2ec_write(int addr, int value)
{
    i2ec_select(addr);
    outb_(value, SIO_BASE + 1);
}

/* TRUE when direct PWM is usable: /dev/port opens (Secure Boot off) and the
 * EC is the chip this register map was found on. */
static gboolean pwm_open(void)
{
    port_fd = open(PORT_DEV, O_RDWR);
    if (port_fd < 0)
        return FALSE;
    int chip = (i2ec_read(0x2000) << 8) | (i2ec_read(0x2001) & 0xFF);
    if (chip != EC_CHIP) {
        fprintf(stderr, "EC chip id 0x%04X is not the expected 0x%04X; not using direct PWM\n",
                chip & 0xFFFF, EC_CHIP);
        close(port_fd);
        port_fd = -1;
        return FALSE;
    }
    return TRUE;
}

static void publish(int pct, int rpm, int direct)
{
    FILE *f = fopen(RUN_FILE ".tmp", "w");
    if (!f)
        return;
    fprintf(f, "%d %d %d %d\n", pct, (int)getpid(), rpm, direct);
    fclose(f);
    rename(RUN_FILE ".tmp", RUN_FILE);
}

/* Runs as root from the systemd service until it is stopped. */
static int run_daemon(void)
{
    gchar *v = read_file(CONF_FILE);
    int pct = v ? parse_target(v) : -2;
    g_free(v);
    if (pct < PCT_MIN) {
        fprintf(stderr, "No valid percentage in %s\n", CONF_FILE);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (read_raw() < 0) {
        fprintf(stderr, "fan_mode not found. Is the ideapad_laptop module loaded?\n");
        return 1;
    }
    if (access(EC_IO, R_OK) != 0 && system("modprobe ec_sys >/dev/null 2>&1") != 0)
        fprintf(stderr, "Warning: could not load ec_sys\n");
    FILE *ec = fopen(EC_IO, "rb");
    if (ec)
        setvbuf(ec, NULL, _IONBF, 0);
    int units = read_fan_units(ec);
    publish(pct, units < 0 ? -1 : units * 100, 0);

    gchar *drv = driver_dir();
    if (!drv && system("modprobe " DRIVER_MODULE " >/dev/null 2>&1") == 0)
        drv = driver_dir();

    if (drv && write_text(drv, "pwm1_enable", 1)) {
        /* Kernel driver: it holds the duty; we refresh it (the driver returns
         * the fan to the firmware if we stop) and apply the overheat guard. */
        write_fan(FAN_AUTO);
        gboolean hot = FALSE;
        for (int k = 0; !stop_requested; k++) {
            if (k % 4 == 0) {
                int t = read_cpu_temp();
                if (t >= TEMP_HOT)
                    hot = TRUE;
                else if (t >= 0 && t < TEMP_COOL)
                    hot = FALSE;
            }
            write_text(drv, "pwm1", hot ? 255 : (pct * 255 + 50) / 100);
            nap(TICK);
            publish(pct, read_driver_rpm(), 1);
        }
        write_text(drv, "pwm1_enable", 2);
    } else if (pwm_open()) {
        /* Direct PWM: the firmware moves the duty one step per second towards
         * its own target, so rewriting it every tick keeps it where we want. */
        write_fan(FAN_AUTO);
        gboolean hot = FALSE;
        for (int k = 0; !stop_requested; k++) {
            if (k % 4 == 0) {
                int t = read_cpu_temp();
                if (t >= TEMP_HOT)
                    hot = TRUE;
                else if (t >= 0 && t < TEMP_COOL)
                    hot = FALSE;
            }
            i2ec_write(EC_DCR4, hot ? DUTY_MAX : pct * DUTY_MAX / 100);
            nap(TICK);
            int u = read_fan_units(ec);
            publish(pct, u < 0 ? -1 : u * 100, 1);
        }
        close(port_fd); /* the firmware now walks the duty back to its own target */
    } else if (units >= 0) {
        /* Feedback loop on the measured speed.
         *
         * What the probes in tools/ showed about this firmware: it acts on a
         * fan_mode write within about a second, then ramps the fan at roughly
         * 3000 rpm per second, so the speed cannot be held still between
         * automatic and full. What can be controlled is the average. The loop
         * integrates the error (target minus measured speed) and runs the fan
         * at full speed while the integral is positive, which makes the
         * long-run average equal the target. Switching faster than once a
         * second is pointless: the firmware misses shorter pulses. */
        double target = FULL_UNITS * pct / 100.0;
        double limit = FULL_UNITS * 0.75;   /* anti-windup, in (100 rpm) x seconds */
        double integral = 0, since = -DWELL, checked = 0;
        gboolean full = FALSE;
        while (!stop_requested) {
            double t = now();
            gboolean want = pct >= 100 || integral > 0;
            if (want != full && t - since >= DWELL) {
                write_fan(want ? FAN_FULL : FAN_AUTO);
                full = want;
                since = t;
            } else if (full && t - checked >= 2.0) {
                checked = t;
                if (read_raw() == RAW_AUTO)  /* the firmware ended the full-speed run */
                    write_fan(FAN_FULL);
            }
            nap(TICK);
            int u = read_fan_units(ec);
            if (u >= 0)
                units = u;
            integral += (target - units) * TICK;
            if (integral > limit)  integral = limit;
            if (integral < -limit) integral = -limit;
            publish(pct, units * 100, 0);
        }
    } else {
        /* No speed reading: full speed for pct% of each cycle instead. */
        fprintf(stderr, "No fan speed reading from %s; using a timed on/off cycle\n", EC_IO);
        double on = PERIOD * pct / 100.0;
        write_fan(FAN_FULL);
        while (!stop_requested) {
            nap(on);
            if (stop_requested || pct >= 100)
                continue;
            write_fan(FAN_AUTO);
            nap(PERIOD - on);
            if (!stop_requested)
                write_fan(FAN_FULL);
        }
    }

    g_free(drv);
    if (ec)
        fclose(ec);
    write_fan(FAN_AUTO); /* always hand the fan back to the firmware */
    unlink(RUN_FILE);
    return 0;
}

static void quiet(const char *cmd)
{
    gchar *c = g_strdup_printf("%s >/dev/null 2>&1", cmd);
    if (system(c) != 0) { /* best effort */ }
    g_free(c);
}

/* Runs as root. target: 0 = automatic, -1 = dust cleaning, else percentage. */
static int apply_as_root(int target)
{
    gboolean systemd = access("/run/systemd/system", F_OK) == 0;

    if (access(OLD_SERVICE, F_OK) == 0) { /* clean up after version 1.0 */
        if (systemd)
            quiet("systemctl disable --now " OLD_SERVICE_NAME);
        unlink(OLD_SERVICE);
    }
    unlink(OLD_STATE);

    if (target <= 0) {
        if (systemd && access(SERVICE, F_OK) == 0)
            quiet("systemctl disable --now " SERVICE_NAME);
        if (!write_fan(FAN_AUTO) || (target < 0 && !write_fan(FAN_FULL))) {
            fprintf(stderr, "Could not write fan_mode. "
                            "Is the ideapad_laptop module loaded? (sudo modprobe ideapad_laptop)\n");
            return 1;
        }
        return 0;
    }

    if (!systemd) {
        fprintf(stderr, "Manual speed needs systemd to run its background service.\n");
        return 1;
    }
    if (read_raw() < 0) {
        fprintf(stderr, "fan_mode not found. Is the ideapad_laptop module loaded?\n");
        return 1;
    }

    /* The service must not run a binary the user can modify: keep a root-owned copy. */
    gchar *self = NULL;
    gsize len = 0;
    GError *err = NULL;
    g_mkdir_with_parents("/usr/local/libexec", 0755);
    if (!g_file_get_contents("/proc/self/exe", &self, &len, &err) ||
        !g_file_set_contents(HELPER, self, len, &err) ||
        chmod(HELPER, 0755) != 0 || chown(HELPER, 0, 0) != 0) {
        fprintf(stderr, "Could not install %s: %s\n", HELPER, err ? err->message : strerror(errno));
        g_clear_error(&err);
        g_free(self);
        return 1;
    }
    g_free(self);

    FILE *f = fopen(CONF_FILE, "w");
    if (!f) {
        fprintf(stderr, "Could not write %s\n", CONF_FILE);
        return 1;
    }
    fprintf(f, "%d\n", target);
    fclose(f);

    f = fopen(SERVICE, "w");
    if (!f) {
        fprintf(stderr, "Could not write %s\n", SERVICE);
        return 1;
    }
    fputs("[Unit]\n"
          "Description=Lenovo IdeaPad manual fan speed\n"
          "After=multi-user.target\n\n"
          "[Service]\n"
          "ExecStart=" HELPER " --daemon\n"
          "ExecStopPost=/bin/sh -c 'for f in " SYSFS_GLOB "; do echo 0 > \"$f\"; done'\n"
          "Restart=on-failure\n"
          "RestartSec=5\n\n"
          "[Install]\n"
          "WantedBy=multi-user.target\n", f);
    fclose(f);

    quiet("systemctl daemon-reload");
    quiet("systemctl enable " SERVICE_NAME);
    if (system("systemctl restart " SERVICE_NAME) != 0) {
        fprintf(stderr, "Could not start " SERVICE_NAME "\n");
        return 1;
    }
    return 0;
}

/* argv that changes the setting: runs this binary as root through pkexec. */
static gchar **apply_argv(const char *target)
{
    gchar **argv = g_new0(gchar *, 5);
    int i = 0;
    if (geteuid() != 0)
        argv[i++] = g_strdup("pkexec");
    argv[i++] = g_file_read_link("/proc/self/exe", NULL);
    argv[i++] = g_strdup("--apply");
    argv[i++] = g_strdup(target);
    return argv;
}

static gboolean pkexec_cancelled(int status)
{
    return WIFEXITED(status) && (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127);
}

/* ------------------------------------------------------------- drawing -- */

/* Fan picture in a 1024-unit square. */
static void draw_fan(cairo_t *cr, double angle, const GdkRGBA *fg, double r, double g, double b)
{
    cairo_save(cr);
    cairo_translate(cr, 512, 512);

    gdk_cairo_set_source_rgba(cr, fg);
    cairo_set_line_width(cr, 44);
    cairo_arc(cr, 0, 0, 468, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, r, g, b);
    for (int i = 0; i < 5; i++) {
        cairo_save(cr);
        cairo_rotate(cr, angle + i * 2 * M_PI / 5);
        cairo_move_to(cr, 80, -50);
        cairo_curve_to(cr, 190, -250, 370, -230, 400, -70);
        cairo_curve_to(cr, 350, 30, 210, 60, 80, 50);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    gdk_cairo_set_source_rgba(cr, fg);
    cairo_arc(cr, 0, 0, 105, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* Writes the app icon (used by build.sh). */
static int make_icon(const char *path, int size)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(s);
    cairo_scale(cr, size / 1024.0, size / 1024.0);

    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, 1024);
    cairo_pattern_add_color_stop_rgb(bg, 0, 0x3d / 255.0, 0x38 / 255.0, 0x46 / 255.0);
    cairo_pattern_add_color_stop_rgb(bg, 1, 0x24 / 255.0, 0x1f / 255.0, 0x31 / 255.0);
    rounded_rect(cr, 40, 40, 944, 944, 210);
    cairo_set_source(cr, bg);
    cairo_fill(cr);
    cairo_pattern_destroy(bg);

    GdkRGBA white = { 1, 1, 1, 1 };
    cairo_translate(cr, 512 * 0.22, 512 * 0.22);
    cairo_scale(cr, 0.78, 0.78);
    draw_fan(cr, 0.3, &white, 0x62 / 255.0, 0xa0 / 255.0, 0xea / 255.0);

    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(s, path);
    cairo_surface_destroy(s);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Could not write %s: %s\n", path, cairo_status_to_string(st));
        return 1;
    }
    return 0;
}

/* ----------------------------------------------------------------- CLI -- */

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: FanControl [status|auto|dust|PERCENT|--version]\n"
        "  (no argument)  open the app\n"
        "  status         show the fan setting and temperatures\n"
        "  auto           automatic control by the firmware (the default)\n"
        "  PERCENT        manual fan power, %d to %d (for example: FanControl 60)\n"
        "  dust           dust cleaning: the firmware's full-speed bursts\n",
        PCT_MIN, PCT_MAX);
}

static const char *NOT_FOUND =
    "Error: fan_mode not found (ideapad_laptop module not loaded, "
    "or this model has no fan control).\n";

static int run_cli(const char *arg)
{
    if (!strcmp(arg, "--version")) {
        printf("FanControl %s\n", APP_VERSION);
        return 0;
    }
    if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
        usage(stdout);
        return 0;
    }
    if (strcmp(arg, "status") != 0) {
        if (parse_target(arg) == -2) {
            usage(stderr);
            return 2;
        }
        if (read_raw() < 0) {
            fputs(NOT_FOUND, stderr);
            return 1;
        }
        gchar **argv = apply_argv(arg);
        gint status = 0;
        GError *err = NULL;
        gboolean ok = g_spawn_sync(NULL, argv, NULL,
                                   G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                                   NULL, NULL, NULL, NULL, &status, &err);
        g_strfreev(argv);
        if (!ok) {
            fprintf(stderr, "Error: %s\n", err->message);
            g_error_free(err);
            return 1;
        }
        if (pkexec_cancelled(status)) {
            fprintf(stderr, "Cancelled.\n");
            return 1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return 1;
        g_usleep(300000); /* let the service write its state */
    }

    int raw, pct, rpm;
    switch (read_state(&raw, &pct, &rpm)) {
    case ST_NA:     fputs(NOT_FOUND, stderr); return 1;
    case ST_AUTO:
        printf("Fan: Automatic (firmware value %d)\n", raw);
        if (rpm >= 0) printf("Fan speed: about %d rpm\n", rpm);
        break;
    case ST_MANUAL:
        printf("Fan: Manual %d%% (firmware value %d)\n", pct, raw);
        if (rpm >= 0 && manual_direct)
            printf("Fan speed: about %d rpm (steady)\n", rpm);
        else if (rpm >= 0)
            printf("Fan speed: about %d rpm (swings around an average of %d; install "
                   "the kernel driver for a steady speed)\n", rpm, FULL_UNITS * pct);
        break;
    default:        printf("Fan: Dust Cleaning is running (firmware value %d)\n", raw); break;
    }
    int t = read_cpu_temp();
    if (t >= 0) printf("CPU: %d °C\n", t);
    t = read_temp("amdgpu");
    if (t >= 0) printf("GPU: %d °C\n", t);
    t = read_temp("nvme");
    if (t >= 0) printf("SSD: %d °C\n", t);
    return 0;
}

/* ----------------------------------------------------------------- GUI -- */

typedef struct {
    GtkWidget *window, *art, *title, *spinner, *msg;
    GtkWidget *auto_btn, *scale, *apply_btn, *dust_btn;
    GtkWidget *rpm, *cpu, *gpu, *ssd, *raw, *model;
    gboolean working;
    int state, pct;      /* current */
    int wanted;          /* target of the change in progress */
    double angle;
} Ui;

static gboolean draw_art(GtkWidget *w, cairo_t *cr, gpointer data)
{
    Ui *ui = data;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double s = 84.0 / 1024.0;
    cairo_translate(cr, (W - 1024 * s) / 2, (H - 1024 * s) / 2);
    cairo_scale(cr, s, s);

    GdkRGBA fg;
    gtk_style_context_get_color(gtk_widget_get_style_context(w), GTK_STATE_FLAG_NORMAL, &fg);
    if (ui->state == ST_MANUAL)    draw_fan(cr, ui->angle, &fg, 0xe6 / 255.0, 0x61 / 255.0, 0x00 / 255.0);
    else if (ui->state == ST_DUST) draw_fan(cr, ui->angle, &fg, 0x91 / 255.0, 0x41 / 255.0, 0xac / 255.0);
    else                           draw_fan(cr, ui->angle, &fg, 0x2e / 255.0, 0xc2 / 255.0, 0x7e / 255.0);
    return FALSE;
}

static gboolean tick(gpointer data)
{
    Ui *ui = data;
    if (!gtk_widget_get_mapped(ui->art))
        return G_SOURCE_CONTINUE;
    double spin = ui->state == ST_MANUAL ? 0.06 + 0.26 * ui->pct / 100.0
                : ui->state == ST_DUST   ? 0.32 : 0.06;
    ui->angle = fmod(ui->angle + spin, 2 * M_PI);
    gtk_widget_queue_draw(ui->art);
    return G_SOURCE_CONTINUE;
}

static void set_temp(GtkWidget *l, int t)
{
    if (t < 0) {
        gtk_label_set_text(GTK_LABEL(l), "-");
        return;
    }
    gchar *s = g_strdup_printf("%d °C", t);
    gtk_label_set_text(GTK_LABEL(l), s);
    g_free(s);
}

static gboolean refresh(gpointer data)
{
    Ui *ui = data;
    int raw, rpm;
    ui->state = read_state(&raw, &ui->pct, &rpm);

    if (!ui->working) {
        gboolean ok = ui->state != ST_NA;
        gtk_widget_set_sensitive(ui->auto_btn, ok && ui->state != ST_AUTO);
        gtk_widget_set_sensitive(ui->scale, ok);
        gtk_widget_set_sensitive(ui->apply_btn, ok);
        gtk_widget_set_sensitive(ui->dust_btn, ok && ui->state != ST_DUST);
    }

    gchar *s;
    switch (ui->state) {
    case ST_NA:     s = g_strdup("Not available"); break;
    case ST_AUTO:   s = g_strdup("Fan: Automatic"); break;
    case ST_MANUAL: s = g_strdup_printf("Fan: Manual %d%%", ui->pct); break;
    default:        s = g_strdup("Dust Cleaning is running"); break;
    }
    gtk_label_set_text(GTK_LABEL(ui->title), s);
    g_free(s);

    if (rpm < 0)
        s = g_strdup(ui->state == ST_MANUAL ? "-" : "shown while a manual speed is set");
    else if (ui->state == ST_MANUAL && !manual_direct)
        s = g_strdup_printf("about %d rpm (average target %d)", rpm, FULL_UNITS * ui->pct);
    else
        s = g_strdup_printf("about %d rpm", rpm);
    gtk_label_set_text(GTK_LABEL(ui->rpm), s);
    g_free(s);
    set_temp(ui->cpu, read_cpu_temp());
    set_temp(ui->gpu, read_temp("amdgpu"));
    set_temp(ui->ssd, read_temp("nvme"));
    if (raw < 0)
        s = g_strdup("fan_mode not found (ideapad_laptop driver not loaded?)");
    else
        s = g_strdup_printf("%d (0x%02X)", raw, raw);
    gtk_label_set_text(GTK_LABEL(ui->raw), s);
    g_free(s);
    return G_SOURCE_CONTINUE;
}

static void show_msg(Ui *ui, const char *text, gboolean error)
{
    GtkStyleContext *c = gtk_widget_get_style_context(ui->msg);
    if (error) gtk_style_context_add_class(c, "error");
    else       gtk_style_context_remove_class(c, "error");
    gtk_label_set_text(GTK_LABEL(ui->msg), text);
}

static gboolean refresh_once(gpointer data)
{
    refresh(data);
    return G_SOURCE_REMOVE;
}

static void on_apply_done(GPid pid, gint status, gpointer data)
{
    Ui *ui = data;
    g_spawn_close_pid(pid);
    ui->working = FALSE;
    gtk_spinner_stop(GTK_SPINNER(ui->spinner));
    gtk_widget_hide(ui->spinner);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        gchar *s = ui->wanted == 0 ? g_strdup("Fan returned to automatic control.")
                 : ui->wanted < 0  ? g_strdup("Dust Cleaning started. Choose Automatic to stop it.")
                 : g_strdup_printf("Manual speed set to %d%%. It is kept after a restart.", ui->wanted);
        show_msg(ui, s, FALSE);
        g_free(s);
    } else if (pkexec_cancelled(status)) {
        show_msg(ui, "Cancelled. Nothing was changed.", FALSE);
    } else {
        show_msg(ui, "Could not change the fan setting.", TRUE);
    }
    refresh(ui);
    g_timeout_add(500, refresh_once, ui); /* the service needs a moment to report */
}

static void start_apply(Ui *ui, int target)
{
    if (ui->working)
        return;
    gchar *arg = target == 0 ? g_strdup("auto") : target < 0 ? g_strdup("dust")
               : g_strdup_printf("%d", target);
    gchar **argv = apply_argv(arg);
    g_free(arg);
    GPid pid;
    GError *err = NULL;
    gboolean ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, &pid, &err);
    g_strfreev(argv);
    if (!ok) {
        show_msg(ui, err->message, TRUE);
        g_error_free(err);
        return;
    }
    ui->working = TRUE;
    ui->wanted = target;
    show_msg(ui, "", FALSE);
    gtk_widget_set_sensitive(ui->auto_btn, FALSE);
    gtk_widget_set_sensitive(ui->scale, FALSE);
    gtk_widget_set_sensitive(ui->apply_btn, FALSE);
    gtk_widget_set_sensitive(ui->dust_btn, FALSE);
    gtk_widget_show(ui->spinner);
    gtk_spinner_start(GTK_SPINNER(ui->spinner));
    g_child_watch_add(pid, on_apply_done, ui);
}

static void on_auto(GtkButton *b, gpointer data) { (void)b; start_apply(data, 0); }
static void on_dust(GtkButton *b, gpointer data) { (void)b; start_apply(data, -1); }

static void on_apply(GtkButton *b, gpointer data)
{
    (void)b;
    Ui *ui = data;
    start_apply(ui, (int)lround(gtk_range_get_value(GTK_RANGE(ui->scale))));
}

static gchar *format_pct(GtkScale *scale, gdouble value, gpointer data)
{
    (void)scale; (void)data;
    return g_strdup_printf("%d%%", (int)lround(value));
}

static GtkWidget *label(const char *text, const char *css_class, float xalign)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    if (css_class)
        gtk_style_context_add_class(gtk_widget_get_style_context(l), css_class);
    return l;
}

static GtkWidget *info_row(GtkWidget *grid, int row, const char *name)
{
    GtkWidget *v = label("", NULL, 0);
    gtk_grid_attach(GTK_GRID(grid), label(name, "dim-label", 0), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
    return v;
}

static GtkWidget *texts(const char *name, const char *desc)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *d = label(desc, "dim-label", 0);
    gtk_label_set_line_wrap(GTK_LABEL(d), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(d), 40);
    gtk_box_pack_start(GTK_BOX(box), label(name, "section", 0), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), d, FALSE, FALSE, 0);
    return box;
}

/* A card with a name, a description and a button on the right. */
static GtkWidget *action_card(const char *name, const char *desc, const char *button,
                              GCallback cb, Ui *ui, GtkWidget **btn)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
    gtk_box_pack_start(GTK_BOX(card), texts(name, desc), TRUE, TRUE, 0);
    *btn = gtk_button_new_with_label(button);
    gtk_widget_set_valign(*btn, GTK_ALIGN_CENTER);
    g_signal_connect(*btn, "clicked", cb, ui);
    gtk_box_pack_start(GTK_BOX(card), *btn, FALSE, FALSE, 0);
    return card;
}

static const char *CSS =
    ".hero-title { font-size: 17pt; font-weight: bold; }\n"
    ".section { font-weight: bold; }\n"
    ".card { background-color: @theme_base_color; border: 1px solid alpha(@borders, 0.8);"
    "        border-radius: 12px; padding: 14px 16px; }\n"
    ".error { color: #c01c28; }\n";

static void on_activate(GtkApplication *app, gpointer data)
{
    Ui *ui = data;
    GList *wins = gtk_application_get_windows(app);
    if (wins) {
        gtk_window_present(GTK_WINDOW(wins->data));
        return;
    }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    ui->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui->window), "Fan Control");
    gtk_window_set_icon_name(GTK_WINDOW(ui->window), "ideapad-fan-control");
    gtk_window_set_resizable(GTK_WINDOW(ui->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 440, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_set(box, "margin", 22, NULL);
    gtk_container_add(GTK_CONTAINER(ui->window), box);

    /* hero: fan picture and title */
    ui->art = gtk_drawing_area_new();
    gtk_widget_set_size_request(ui->art, 84, 84);
    g_signal_connect(ui->art, "draw", G_CALLBACK(draw_art), ui);
    gtk_box_pack_start(GTK_BOX(box), ui->art, FALSE, FALSE, 0);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(head, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(head, 10);
    ui->title = label("", "hero-title", 0.5);
    ui->spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(head), ui->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), ui->spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);

    /* automatic (default) */
    GtkWidget *card = action_card("Automatic (default)",
        "The laptop's firmware sets the fan speed from the temperature.",
        "Use", G_CALLBACK(on_auto), ui, &ui->auto_btn);
    gtk_widget_set_margin_top(card, 20);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    /* manual speed */
    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
    gtk_widget_set_margin_top(card, 12);
    gtk_box_pack_start(GTK_BOX(card), texts("Manual speed",
        "Fan power in percent. 100% is about 5200 rpm. Steady with the kernel "
        "driver installed; without it the fan pulses around the average."),
        FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    ui->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, PCT_MIN, PCT_MAX, 5);
    gtk_scale_set_value_pos(GTK_SCALE(ui->scale), GTK_POS_RIGHT);
    gtk_range_set_round_digits(GTK_RANGE(ui->scale), 0);
    for (int p = PCT_MIN; p <= PCT_MAX; p += 10)
        gtk_scale_add_mark(GTK_SCALE(ui->scale), p, GTK_POS_BOTTOM, NULL);
    g_signal_connect(ui->scale, "format-value", G_CALLBACK(format_pct), NULL);
    gtk_box_pack_start(GTK_BOX(row), ui->scale, TRUE, TRUE, 0);
    ui->apply_btn = gtk_button_new_with_label("Apply");
    gtk_widget_set_valign(ui->apply_btn, GTK_ALIGN_CENTER);
    g_signal_connect(ui->apply_btn, "clicked", G_CALLBACK(on_apply), ui);
    gtk_box_pack_start(GTK_BOX(row), ui->apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    /* dust cleaning */
    card = action_card("Dust Cleaning",
        "The firmware's full-speed bursts. Choose Automatic to stop.",
        "Start", G_CALLBACK(on_dust), ui, &ui->dust_btn);
    gtk_widget_set_margin_top(card, 12);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    /* details */
    GtkWidget *st = label("Sensors", "section", 0);
    gtk_widget_set_margin_top(st, 20);
    gtk_box_pack_start(GTK_BOX(box), st, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_widget_set_margin_top(grid, 8);
    ui->rpm   = info_row(grid, 0, "Fan speed");
    ui->cpu   = info_row(grid, 1, "CPU");
    ui->gpu   = info_row(grid, 2, "GPU");
    ui->ssd   = info_row(grid, 3, "SSD");
    ui->raw   = info_row(grid, 4, "Firmware value");
    ui->model = info_row(grid, 5, "Model");
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    gchar *model = read_file("/sys/class/dmi/id/product_version");
    gtk_label_set_text(GTK_LABEL(ui->model), model && *model ? model : "Unknown");
    g_free(model);

    ui->msg = label("", NULL, 0);
    gtk_label_set_line_wrap(GTK_LABEL(ui->msg), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui->msg), 50);
    gtk_widget_set_margin_top(ui->msg, 14);
    gtk_box_pack_start(GTK_BOX(box), ui->msg, FALSE, FALSE, 0);

    gtk_widget_show_all(ui->window);
    gtk_widget_hide(ui->spinner);
    refresh(ui);
    gtk_range_set_value(GTK_RANGE(ui->scale), ui->state == ST_MANUAL ? ui->pct : 50);
    g_timeout_add_seconds(1, refresh, ui);
    g_timeout_add(40, tick, ui);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--apply")) {
        if (geteuid() != 0) {
            fprintf(stderr, "--apply must run as root\n");
            return 1;
        }
        int target = parse_target(argv[2]);
        return target == -2 ? 2 : apply_as_root(target);
    }
    if (argc == 2 && !strcmp(argv[1], "--daemon"))
        return run_daemon();
    if (argc == 3 && !strcmp(argv[1], "--make-icon"))
        return make_icon(argv[2], 256);
    if (argc == 2)
        return run_cli(argv[1]);

    Ui ui = { .state = ST_NA };
#if GLIB_CHECK_VERSION(2, 74, 0)
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
#else
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_FLAGS_NONE);
#endif
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &ui);
    int r = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return r;
}
