/*
 * Fan Control for Windows (Lenovo IdeaPad 3 14ALC6)
 *
 * Two ways to drive the fan, the best one available is used:
 *
 * 1. Steady speed, through PawnIO (https://pawnio.eu), a signed kernel driver
 *    that runs small signed modules. Its official LpcIO module reaches the
 *    PWM duty register of the laptop's embedded controller (EC, an ITE IT8227):
 *      DCR4 at 0x1806 in the EC's register space: 0 = stopped, 200 = full speed
 *      tachometer period at 0x181E (low) / 0x181F (high)
 *    through the chip's "I2EC" window on the Super I/O ports 0x4E/0x4F.
 *    Needs PawnIO installed and LpcIO.bin next to FanControl.exe.
 *
 *    The firmware keeps running and moves the duty one step per second towards
 *    its own target, so the app rewrites the duty four times a second. That
 *    also tells us what the firmware wants: if it keeps stepping up, it wants
 *    more cooling than the chosen value, and the app then follows it up.
 *
 *    Returning to automatic: left alone, the firmware walks the duty back one
 *    step per second, which takes minutes from a high setting. The app
 *    therefore searches for the firmware's target instead: it writes a value,
 *    watches which way the firmware nudges it, and halves the interval, which
 *    hands the fan back within about ten seconds.
 *
 * 2. Pulsing speed, without PawnIO, through Lenovo's own Energy Management
 *    driver (\\.\EnergyDrv, the ACPI VPC2004 device that Lenovo Vantage uses).
 *    It switches the firmware's fan mode, which on this model has only two
 *    states: automatic, and dust cleaning (full-speed bursts). Switching
 *    between them so that the average matches the percentage is the same
 *    method the Linux app uses without its kernel driver. The fan audibly
 *    rises and falls, and the average never goes below automatic control.
 *    This driver also provides Dust Cleaning.
 *
 * The speed is held only while this program runs; closing it returns the
 * fan to the firmware.
 *
 *   FanControl.exe              open the app
 *   FanControl.exe status       show the fan state
 *   FanControl.exe auto         return the fan to the firmware
 *   FanControl.exe dust         start dust cleaning
 *   FanControl.exe N            hold N percent (30..100) until Ctrl+C
 *   FanControl.exe --hold N     same, with no window (for Task Scheduler)
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>
#include <commctrl.h>
#include <shellapi.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define APP_NAME    L"Fan Control"
#define APP_VERSION L"2.2.0"

/* --- PawnIO device interface (from PawnIO's pawnio_um.h / PawnIOLib.cpp) --- */
#define PIO_DEVICE_TYPE   41394
#define IOCTL_PIO_LOAD    CTL_CODE(PIO_DEVICE_TYPE, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PIO_EXECUTE CTL_CODE(PIO_DEVICE_TYPE, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define PIO_NAME_LEN      32
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif

/* --- Lenovo Energy Management driver (ACPI VPC2004) --- */
#define IOCTL_VPC_SET     0x831020C0  /* in: DWORD {6, 1, mode}: fan mode 0 = automatic, 1 = full */
#define IOCTL_VPC_GET     0x831020C4  /* in: DWORD 14, out: DWORD fan mode as the firmware reports it */
#define RAW_AUTO          133         /* reported while in automatic control (as on Linux) */

/* --- the EC --- */
#define EC_CHIP      0x8227
#define EC_CHIPID1   0x2000
#define EC_CHIPID2   0x2001
#define EC_DCR4      0x1806
#define EC_TACH_LO   0x181E
#define EC_TACH_HI   0x181F
#define DUTY_MAX     200
#define TACH_K       2130000  /* rpm = TACH_K / tach period; from 5200 rpm = 409, 2500 rpm = 861 */

#define PCT_MIN      30
#define PCT_MAX      100
#define TICK_MS      250

/* Pulsing method: a model of the fan instead of a speed sensor (units of 100 rpm). */
#define FULL_UNITS   52     /* full speed, about 5200 rpm */
#define AUTO_UNITS   22     /* typical automatic speed on a cool laptop */
#define RAMP_UNITS   30.0   /* the fan ramps about 3000 rpm per second */
#define DWELL        1.0    /* minimum seconds between fan mode switches */

enum { ST_NA, ST_AUTO, ST_MANUAL, ST_DUST };

static HANDLE g_pio = INVALID_HANDLE_VALUE;
static HANDLE g_vpc = INVALID_HANDLE_VALUE;
static BOOL g_pwm;           /* PawnIO + LpcIO + the right EC: steady speed */
static HANDLE g_bus_mutex;   /* convention shared by hardware monitors for the Super I/O ports */
static wchar_t g_pwm_why[512];

static void set_why(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(g_pwm_why, 511, fmt, ap);
    g_pwm_why[511] = 0;
    va_end(ap);
}

static double now_s(void) { return GetTickCount64() / 1000.0; }

/* ------------------------------------------------------------ PawnIO -- */

static BOOL pio_execute(const char *name, const ULONG64 *in, DWORD n_in, ULONG64 *out, DWORD n_out)
{
    BYTE buf[PIO_NAME_LEN + 4 * sizeof(ULONG64)];
    DWORD got = 0;
    if (n_in > 4)
        return FALSE;
    ZeroMemory(buf, sizeof buf);
    lstrcpynA((char *)buf, name, PIO_NAME_LEN);
    if (n_in)
        CopyMemory(buf + PIO_NAME_LEN, in, n_in * sizeof(ULONG64));
    return DeviceIoControl(g_pio, IOCTL_PIO_EXECUTE, buf, PIO_NAME_LEN + n_in * sizeof(ULONG64),
                           out, n_out * sizeof(ULONG64), &got, NULL)
           && got == n_out * sizeof(ULONG64);
}

/* Super I/O register write / read through the LpcIO module (index port 0x4E). */
static BOOL sio_out(int reg, int val)
{
    ULONG64 in[2] = { (ULONG64)reg, (ULONG64)val };
    return pio_execute("ioctl_superio_outb", in, 2, NULL, 0);
}

static int sio_in(int reg)
{
    ULONG64 in = (ULONG64)reg, out = 0;
    if (!pio_execute("ioctl_superio_inb", &in, 1, &out, 1))
        return -1;
    return (int)(out & 0xFF);
}

static BOOL bus_lock(void)
{
    DWORD r = WaitForSingleObject(g_bus_mutex, 200);
    return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
}

static void bus_unlock(void) { ReleaseMutex(g_bus_mutex); }

static BOOL i2ec_select(int addr)
{
    return sio_out(0x2E, 0x11) && sio_out(0x2F, addr >> 8) &&
           sio_out(0x2E, 0x10) && sio_out(0x2F, addr & 0xFF) &&
           sio_out(0x2E, 0x12);
}

/* One byte of the EC's register space, or -1. */
static int i2ec_read(int addr)
{
    int v = -1;
    if (!bus_lock())
        return -1;
    if (i2ec_select(addr))
        v = sio_in(0x2F);
    bus_unlock();
    return v;
}

static BOOL i2ec_write(int addr, int val)
{
    BOOL ok = FALSE;
    if (!bus_lock())
        return FALSE;
    if (i2ec_select(addr))
        ok = sio_out(0x2F, val);
    bus_unlock();
    return ok;
}

static BYTE *read_whole_file(const wchar_t *path, DWORD *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD n = GetFileSize(f, NULL), got = 0;
    BYTE *b = (n && n < (4u << 20)) ? HeapAlloc(GetProcessHeap(), 0, n) : NULL;
    if (b && (!ReadFile(f, b, n, &got, NULL) || got != n)) {
        HeapFree(GetProcessHeap(), 0, b);
        b = NULL;
    }
    CloseHandle(f);
    *size = n;
    return b;
}

static void pio_close(void)
{
    if (g_pio != INVALID_HANDLE_VALUE)
        CloseHandle(g_pio);
    g_pio = INVALID_HANDLE_VALUE;
}

static BOOL g_pio_missing;   /* the PawnIO driver is not installed at all */

/* Full path of a file in the program's folder. FALSE if it does not fit. */
static BOOL beside_exe(wchar_t path[MAX_PATH], const wchar_t *file)
{
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (len && path[len - 1] != L'\\')
        len--;
    path[len] = 0;
    if (len + wcslen(file) + 1 >= MAX_PATH)
        return FALSE;
    wcscat(path, file);
    return TRUE;
}

/* Opens PawnIO, loads LpcIO.bin and checks that the EC is the expected chip. */
static BOOL pwm_open(void)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&name, L"\\Device\\PawnIO");
    InitializeObjectAttributes(&attr, &name, 0, NULL, NULL);
    NTSTATUS st = NtOpenFile(&g_pio, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &attr, &iosb,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             FILE_SYNCHRONOUS_IO_NONALERT);
    if (st < 0) {
        g_pio = INVALID_HANDLE_VALUE;
        g_pio_missing = TRUE;
        set_why(L"PawnIO is not installed (status 0x%08lX).", (unsigned long)st);
        return FALSE;
    }
    g_pio_missing = FALSE;

    wchar_t path[MAX_PATH];
    if (!beside_exe(path, L"LpcIO.bin")) {
        set_why(L"The program's folder path is too long.");
        pio_close();
        return FALSE;
    }

    DWORD size = 0;
    BYTE *blob = read_whole_file(path, &size);
    if (!blob) {
        set_why(L"PawnIO is installed, but LpcIO.bin is missing from\n%ls", path);
        pio_close();
        return FALSE;
    }
    DWORD got = 0;
    BOOL ok = DeviceIoControl(g_pio, IOCTL_PIO_LOAD, blob, size, NULL, 0, &got, NULL);
    DWORD e = GetLastError();
    HeapFree(GetProcessHeap(), 0, blob);
    if (!ok) {
        set_why(L"PawnIO refused LpcIO.bin (error %lu). Use the signed LpcIO.bin "
                L"from the official PawnIO.Modules release.", e);
        pio_close();
        return FALSE;
    }

    if (!g_bus_mutex)
        g_bus_mutex = CreateMutexW(NULL, FALSE, L"Global\\Access_ISABUS.HTP.Method");
    if (!g_bus_mutex) {
        set_why(L"Could not create the bus mutex (error %lu).", GetLastError());
        pio_close();
        return FALSE;
    }

    ULONG64 slot = 1; /* Super I/O at 0x4E/0x4F */
    if (!pio_execute("ioctl_select_slot", &slot, 1, NULL, 0)) {
        set_why(L"The LpcIO module did not accept the port selection (error %lu).", GetLastError());
        pio_close();
        return FALSE;
    }
    int hi = i2ec_read(EC_CHIPID1), lo = i2ec_read(EC_CHIPID2);
    if (hi < 0 || lo < 0 || ((hi << 8) | lo) != EC_CHIP) {
        set_why(L"The embedded controller is not the expected ITE IT8227 (chip id %02X%02X), "
                L"so the PWM register is not touched.", hi & 0xFF, lo & 0xFF);
        pio_close();
        return FALSE;
    }
    return TRUE;
}

static int read_duty(void) { return g_pwm ? i2ec_read(EC_DCR4) : -1; }

static int read_rpm(void)
{
    if (!g_pwm)
        return -1;
    int lo = i2ec_read(EC_TACH_LO), hi = i2ec_read(EC_TACH_HI);
    if (lo < 0 || hi < 0)
        return -1;
    int period = (hi << 8) | lo;
    return period ? TACH_K / period : 0;
}

/* ------------------------------------------- Lenovo Energy Management -- */

static BOOL vpc_open(void)
{
    /* The device name differs between driver versions. */
    static const wchar_t *names[] = { L"\\\\.\\EnergyDrv", L"\\\\.\\Lenovo0",
                                      L"\\\\.\\Lenovo1", L"\\\\.\\AcpiVpc" };
    for (int i = 0; i < 4; i++) {
        g_vpc = CreateFileW(names[i], GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (g_vpc != INVALID_HANDLE_VALUE)
            return TRUE;
    }
    return FALSE;
}

static BOOL vpc_ok(void) { return g_vpc != INVALID_HANDLE_VALUE; }

/* Firmware fan mode: 0 = automatic, 1 = full speed (dust cleaning bursts). */
static BOOL vpc_fan(int full)
{
    DWORD in[3] = { 6, 1, full ? 1 : 0 }, got = 0;
    return vpc_ok() && DeviceIoControl(g_vpc, IOCTL_VPC_SET, in, sizeof in, NULL, 0, &got, NULL);
}

/* What the firmware reports (133 = automatic), or -1. */
static int vpc_raw(void)
{
    DWORD in = 14, out = 0, got = 0;
    if (!vpc_ok() || !DeviceIoControl(g_vpc, IOCTL_VPC_GET, &in, sizeof in, &out, sizeof out, &got, NULL)
        || got != sizeof out)
        return -1;
    return (int)(out & 0xFFFF);
}

static BOOL hw_open(void)
{
    g_pwm = pwm_open();
    vpc_open();
    return g_pwm || vpc_ok();
}

/* --------------------------------------------------------- the control -- */

static struct {
    int mode;           /* what the app drives: ST_AUTO, ST_MANUAL or ST_DUST */
    int pct;            /* the user's percentage */
    BOOL direct;        /* manual through the PWM register (steady) */
    int duty;           /* direct: what we hold; above pct*2 while the firmware wants more */
    /* pulsing */
    BOOL full;
    double integral, est, since, checked;
    /* returning to the firmware */
    BOOL rel;
    int lo, hi, mid, rounds;
    double rel_at;
} C = { .mode = ST_AUTO };

static int pct_duty(int pct) { return pct * DUTY_MAX / 100; }

/* Finds the firmware's own target by bisection; see the comment at the top. */
static void release_start(void)
{
    int d = read_duty();
    if (d < 0)
        return;
    C.rel = TRUE;
    C.lo = 0;
    C.hi = DUTY_MAX;
    C.mid = d;
    C.rounds = 0;
    C.rel_at = now_s();
    i2ec_write(EC_DCR4, C.mid);
}

static void release_tick(void)
{
    int d = read_duty();
    if (d < 0) {
        C.rel = FALSE;
        return;
    }
    if (d != C.mid) {
        /* The firmware moved the duty one step towards its target. */
        if (d < C.mid)
            C.hi = C.mid - 1;
        else
            C.lo = C.mid + 1;
        if (C.hi - C.lo < 2 || ++C.rounds > 12) {
            C.rel = FALSE;          /* the firmware takes the last step itself */
            return;
        }
        C.mid = (C.lo + C.hi) / 2;
        i2ec_write(EC_DCR4, C.mid);
        C.rel_at = now_s();
    } else if (now_s() - C.rel_at > 1.6) {
        C.rel = FALSE;              /* no nudge for a while: this is the firmware's target */
    }
}

static BOOL ctl_manual(int pct)
{
    BOOL was_pulsing = C.mode == ST_MANUAL && !C.direct;
    C.rel = FALSE;
    C.pct = pct;
    if (g_pwm) {
        if (C.mode == ST_DUST || !C.direct)
            vpc_fan(0);
        C.mode = ST_MANUAL;
        C.direct = TRUE;
        C.duty = pct_duty(pct);
        return i2ec_write(EC_DCR4, C.duty);
    }
    if (vpc_ok()) {
        if (!was_pulsing) {
            if (C.mode == ST_DUST)
                vpc_fan(0);
            C.full = FALSE;
            C.integral = 0;
            C.est = AUTO_UNITS;
            C.since = now_s() - DWELL;
            C.checked = 0;
        }
        C.mode = ST_MANUAL;
        C.direct = FALSE;
        return TRUE;
    }
    return FALSE;
}

/* force: also search the firmware's target when we were not holding a speed. */
static BOOL ctl_auto(BOOL force)
{
    BOOL was_direct = C.mode == ST_MANUAL && C.direct;
    C.mode = ST_AUTO;
    BOOL ok = vpc_fan(0);
    if (g_pwm && (was_direct || force)) {
        release_start();
        ok = TRUE;
    }
    return ok || g_pwm;
}

static BOOL ctl_dust(void)
{
    C.rel = FALSE;
    if (!vpc_fan(1))
        return FALSE;
    C.mode = ST_DUST;
    return TRUE;
}

/* Direct PWM. Returns FALSE when the EC could not be reached. */
static BOOL hold_tick(void)
{
    int want = pct_duty(C.pct);
    int now = read_duty();
    if (now < 0)
        return FALSE;
    /* The firmware steps the register towards its own target. A step up means
     * it wants more cooling than we hold: follow it. A step down while we are
     * above the user's value means it is satisfied again: follow it back. */
    if (now > C.duty)
        C.duty = now > DUTY_MAX ? DUTY_MAX : now;
    else if (now < C.duty && C.duty > want)
        C.duty = now;
    if (C.duty < want)
        C.duty = want;
    return i2ec_write(EC_DCR4, C.duty);
}

/* Fan mode switching. The integral of (target - modelled speed) decides when
 * the fan runs at full speed, so the long-run average matches the target. */
static void pulse_tick(void)
{
    const double dt = TICK_MS / 1000.0;
    double t = now_s();
    double target = FULL_UNITS * C.pct / 100.0, limit = FULL_UNITS * 0.75;
    BOOL want = C.pct >= 100 || C.integral > 0;
    if (want != C.full && t - C.since >= DWELL) {
        vpc_fan(want);
        C.full = want;
        C.since = t;
    } else if (C.full && t - C.checked >= 2.0) {
        C.checked = t;
        int raw = vpc_raw();
        if (raw < 0 || raw == RAW_AUTO)   /* the firmware ended the full-speed run */
            vpc_fan(1);
    }
    double goal = C.full ? FULL_UNITS : AUTO_UNITS, step = RAMP_UNITS * dt;
    double d = goal - C.est;
    C.est += d > step ? step : d < -step ? -step : d;
    C.integral += (target - C.est) * dt;
    if (C.integral > limit)  C.integral = limit;
    if (C.integral < -limit) C.integral = -limit;
}

/* Called every TICK_MS. Returns FALSE when the hardware could not be reached. */
static BOOL ctl_tick(void)
{
    if (C.rel)
        release_tick();
    if (C.mode != ST_MANUAL)
        return TRUE;
    if (C.direct)
        return hold_tick();
    pulse_tick();
    return TRUE;
}

static int ctl_state(int *raw)
{
    *raw = vpc_raw();
    if (!g_pwm && !vpc_ok())
        return ST_NA;
    if (C.mode == ST_MANUAL)
        return ST_MANUAL;
    if (C.mode == ST_DUST) {
        if (*raw == RAW_AUTO) {
            C.mode = ST_AUTO;
            return ST_AUTO;
        }
        return ST_DUST;
    }
    if (*raw >= 0 && *raw != RAW_AUTO)
        return ST_DUST;   /* started by Lenovo Vantage, a key combination, or our CLI */
    return ST_AUTO;
}

/* -------------------------------------------------------------- sensors -- */

static volatile LONG g_ssd_temp = -1;

/* Structures of IOCTL_STORAGE_QUERY_PROPERTY / StorageDeviceTemperatureProperty
 * (Windows 10), defined here because older MinGW headers lack them. */
typedef struct {
    WORD Index;
    SHORT Temperature;
    SHORT OverThreshold;
    SHORT UnderThreshold;
    BOOLEAN OverThresholdChangable, UnderThresholdChangable, EventGenerated;
    BYTE Reserved0;
    DWORD Reserved1;
} TempInfo;

typedef struct {
    DWORD Version, Size;
    SHORT CriticalTemperature, WarningTemperature;
    WORD InfoCount;
    BYTE Reserved0[2];
    DWORD Reserved1[2];
    TempInfo Info[4];
} TempDesc;

static int read_ssd_temp(void)
{
    HANDLE h = CreateFileW(L"\\\\.\\PhysicalDrive0", 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    STORAGE_PROPERTY_QUERY q;
    ZeroMemory(&q, sizeof q);
    q.PropertyId = (STORAGE_PROPERTY_ID)52; /* StorageDeviceTemperatureProperty */
    q.QueryType = PropertyStandardQuery;
    TempDesc d;
    ZeroMemory(&d, sizeof d);
    DWORD got = 0;
    int t = -1;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &d, sizeof d, &got, NULL)
        && got >= offsetof(TempDesc, Info) + sizeof(TempInfo) && d.InfoCount > 0
        && d.Info[0].Temperature > 0 && d.Info[0].Temperature < 150)
        t = d.Info[0].Temperature;
    CloseHandle(h);
    return t;
}

static DWORD WINAPI sensor_thread(LPVOID p)
{
    (void)p;
    for (;;) {
        InterlockedExchange(&g_ssd_temp, read_ssd_temp());
        Sleep(2000);
    }
    return 0;
}

/* ----------------------------------------------------------- command line -- */

static volatile LONG g_stop;

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

static void console(void)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
}

static int parse_pct(const wchar_t *s)
{
    wchar_t *end;
    long p = wcstol(s, &end, 10);
    if (end == s || (*end && wcscmp(end, L"%")) || p < PCT_MIN || p > PCT_MAX)
        return -1;
    return (int)p;
}

static void not_available(void)
{
    fwprintf(stderr, L"Fan control is not available.\n%ls\n"
                     L"The Lenovo Energy Management driver (\\\\.\\EnergyDrv) was not found either.\n"
                     L"Install PawnIO (https://pawnio.eu) or Lenovo's Energy Management driver, and run as administrator.\n",
             g_pwm_why);
}

/* Lets the firmware's target search finish (about ten seconds at most). */
static void finish_release(BOOL quiet)
{
    if (C.rel && !quiet)
        wprintf(L"Returning the fan to the firmware...\n");
    for (int k = 0; C.rel && k < 120; k++) {
        Sleep(TICK_MS);
        ctl_tick();
    }
}

static int run_hold(int pct, BOOL quiet)
{
    if (!hw_open()) {
        if (!quiet)
            not_available();
        return 1;
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    ctl_manual(pct);
    if (!quiet) {
        if (C.direct)
            wprintf(L"Holding the fan at %d%%. Press Ctrl+C to return to automatic.\n", pct);
        else
            wprintf(L"Holding an average of %d%% by switching the firmware's fan mode "
                    L"(PawnIO not available: %ls)\nPress Ctrl+C to return to automatic.\n", pct, g_pwm_why);
    }
    for (int k = 0; !g_stop; k++) {
        Sleep(TICK_MS);
        ctl_tick();
        if (!quiet && C.direct && k % 8 == 7) {
            wprintf(L"\r%d%% power, about %d rpm   ", C.duty * 100 / DUTY_MAX, read_rpm());
            fflush(stdout);
        }
    }
    if (!quiet)
        wprintf(L"\n");
    ctl_auto(FALSE);
    finish_release(quiet);
    if (!quiet)
        wprintf(L"Returned to automatic control.\n");
    return 0;
}

static int run_cli(int argc, wchar_t **argv)
{
    if (argc == 3 && !wcscmp(argv[1], L"--hold")) {
        int pct = parse_pct(argv[2]);
        return pct < 0 ? 2 : run_hold(pct, TRUE);
    }
    console();
    if (argc == 2 && !wcscmp(argv[1], L"--version")) {
        wprintf(L"FanControl %ls\n", APP_VERSION);
        return 0;
    }
    BOOL is_status = argc == 2 && !wcscmp(argv[1], L"status");
    BOOL is_auto = argc == 2 && (!wcscmp(argv[1], L"auto") || !wcscmp(argv[1], L"default"));
    BOOL is_dust = argc == 2 && !wcscmp(argv[1], L"dust");
    if (is_status || is_auto || is_dust) {
        if (!hw_open()) {
            not_available();
            return 1;
        }
        if (is_auto) {
            if (!ctl_auto(TRUE)) {
                fwprintf(stderr, L"Could not change the fan mode.\n");
                return 1;
            }
            finish_release(FALSE);
            wprintf(L"Fan returned to automatic control.\n");
        } else if (is_dust) {
            if (!vpc_ok()) {
                fwprintf(stderr, L"Dust Cleaning needs the Lenovo Energy Management driver.\n");
                return 1;
            }
            if (!ctl_dust()) {
                fwprintf(stderr, L"Could not start Dust Cleaning.\n");
                return 1;
            }
            wprintf(L"Dust Cleaning started. Run \"FanControl.exe auto\" to stop it.\n");
            Sleep(500);
        }
        int raw, duty = read_duty(), rpm = read_rpm();
        int st = ctl_state(&raw);
        wprintf(L"Fan: %ls\n", st == ST_DUST ? L"Dust Cleaning is running" : L"Automatic");
        if (duty >= 0)
            wprintf(L"Fan power: %d%%\nFan speed: about %d rpm\n", duty * 100 / DUTY_MAX, rpm);
        if (raw >= 0)
            wprintf(L"Firmware value: %d (0x%02X)\n", raw, raw);
        wprintf(L"Control: %ls\n", g_pwm ? L"PawnIO (steady speed)"
                                         : L"Lenovo driver only (manual speed pulses; install PawnIO for a steady speed)");
        int t = read_ssd_temp();
        if (t >= 0)
            wprintf(L"SSD: %d \x00B0" L"C\n", t);
        return 0;
    }
    int pct = argc == 2 ? parse_pct(argv[1]) : -1;
    if (pct < 0) {
        fwprintf(stderr,
            L"Usage: FanControl.exe [status|auto|dust|PERCENT|--hold PERCENT|--version]\n"
            L"  (no argument)   open the app\n"
            L"  status          show the fan state\n"
            L"  auto            return the fan to the firmware (the default)\n"
            L"  dust            dust cleaning: the firmware's full-speed bursts\n"
            L"  PERCENT         hold %d to %d percent until Ctrl+C\n"
            L"  --hold PERCENT  the same with no window, for Task Scheduler\n", PCT_MIN, PCT_MAX);
        return 2;
    }
    return run_hold(pct, FALSE);
}

/* ---------------------------------------------------------------- window -- */

/* Colours of the Linux app (GTK Adwaita). */
#define COL_BG      RGB(0xFA, 0xFA, 0xFA)
#define COL_CARD    RGB(0xFF, 0xFF, 0xFF)
#define COL_BORDER  RGB(0xDE, 0xDD, 0xDA)
#define COL_TEXT    RGB(0x2E, 0x34, 0x36)
#define COL_DIM     RGB(0x86, 0x89, 0x8A)
#define COL_ERROR   RGB(0xC0, 0x1C, 0x28)
#define COL_TROUGH  RGB(0xDE, 0xDD, 0xDA)
#define COL_ACCENT  RGB(0xE6, 0x61, 0x00)
#define COL_AUTO    RGB(0x2E, 0xC2, 0x7E)
#define COL_MANUAL  RGB(0xE6, 0x61, 0x00)
#define COL_DUST    RGB(0x91, 0x41, 0xAC)

/* Layout, in pixels at 96 dpi. */
#define WIN_W       468
#define WIN_H       778
#define MARGIN      22
#define CARD_W      (WIN_W - 2 * MARGIN)
#define ART         84
#define CARD1_Y     166
#define CARD1_H     86
#define CARD2_Y     264
#define CARD2_H     140
#define CARD3_Y     416
#define CARD3_H     86

enum { ID_SLIDER = 101, ID_APPLY, ID_AUTO, ID_DUST, ID_TIMER = 1, ID_ANIM = 2 };
enum { F_CARD = 1, F_DIM = 2, F_ERROR = 4 };

static struct {
    HWND hwnd, title, msg, slider, pct, apply, auto_btn, dust_btn;
    HWND rpm, power, ssd, raw, method, model;
    HFONT f_title, f_text, f_small, f_bold;
    HBRUSH b_bg, b_card;
    int dpi, ticks, state;
    double angle;
    BOOL ready, closing, releasing;
    wchar_t note[400];
    BOOL note_error;
} A;

static int S(int v) { return MulDiv(v, A.dpi, 96); }

/* Font size in tenths of a point. */
static HFONT make_font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, A.dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Segoe UI");
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h,
                int id, HFONT font, int flags)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             A.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowLongPtrW(c, GWLP_USERDATA, flags);
    return c;
}

static HWND text(const wchar_t *t, int x, int y, int w, int h, HFONT f, int flags)
{
    return add(L"STATIC", t, SS_NOPREFIX, x, y, w, h, 0, f, flags);
}

static int slider_pct(void)
{
    return (int)SendMessageW(A.slider, TBM_GETPOS, 0, 0) * 5;
}

static void set_note(const wchar_t *t, BOOL error)
{
    lstrcpynW(A.note, t, 400);
    A.note_error = error;
}

/* Fan picture in a 1024-unit square, as in the Linux app. Drawn four times
 * larger and scaled down, which smooths the edges. */
static void draw_fan(HDC dc, int x0, int y0, int size, double angle, COLORREF blade)
{
    int ss = size * 4;
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, ss, ss);
    HGDIOBJ old_bmp = SelectObject(m, bmp);
    RECT r = { 0, 0, ss, ss };
    FillRect(m, &r, A.b_bg);

    SetGraphicsMode(m, GM_ADVANCED);
    float k = ss / 1024.0f, c = ss / 2.0f;
    XFORM base = { k, 0, 0, k, c, c };
    HBRUSH fg = CreateSolidBrush(COL_TEXT), bl = CreateSolidBrush(blade);
    HGDIOBJ old_pen = SelectObject(m, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(m, fg);

    SetWorldTransform(m, &base);
    Ellipse(m, -490, -490, 491, 491);
    SelectObject(m, A.b_bg);
    Ellipse(m, -446, -446, 447, 447);

    SelectObject(m, bl);
    for (int i = 0; i < 5; i++) {
        double a = angle + i * 2 * M_PI / 5;
        float cs = (float)cos(a) * k, sn = (float)sin(a) * k;
        XFORM rot = { cs, sn, -sn, cs, c, c };
        SetWorldTransform(m, &rot);
        POINT p1[3] = { { 190, -250 }, { 370, -230 }, { 400, -70 } };
        POINT p2[3] = { { 350, 30 }, { 210, 60 }, { 80, 50 } };
        BeginPath(m);
        MoveToEx(m, 80, -50, NULL);
        PolyBezierTo(m, p1, 3);
        PolyBezierTo(m, p2, 3);
        CloseFigure(m);
        EndPath(m);
        FillPath(m);
    }

    SetWorldTransform(m, &base);
    SelectObject(m, fg);
    Ellipse(m, -105, -105, 106, 106);
    ModifyWorldTransform(m, NULL, MWT_IDENTITY);

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, x0, y0, size, size, m, 0, 0, ss, ss, SRCCOPY);

    SelectObject(m, old_brush);
    SelectObject(m, old_pen);
    SelectObject(m, old_bmp);
    DeleteObject(fg);
    DeleteObject(bl);
    DeleteObject(bmp);
    DeleteDC(m);
}

static void draw_card(HDC dc, int y, int h)
{
    HPEN pen = CreatePen(PS_SOLID, 1, COL_BORDER);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, A.b_card);
    RoundRect(dc, S(MARGIN), S(y), S(MARGIN + CARD_W), S(y + h), S(24), S(24));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(m, bmp);
    FillRect(m, &rc, A.b_bg);

    COLORREF blade = A.state == ST_MANUAL ? COL_MANUAL : A.state == ST_DUST ? COL_DUST : COL_AUTO;
    draw_fan(m, (rc.right - S(ART)) / 2, S(22), S(ART), A.angle, blade);
    draw_card(m, CARD1_Y, CARD1_H);
    draw_card(m, CARD2_Y, CARD2_H);
    draw_card(m, CARD3_Y, CARD3_H);

    BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
           ps.rcPaint.bottom - ps.rcPaint.top, m, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(m, ob);
    DeleteObject(bmp);
    DeleteDC(m);
    EndPaint(hwnd, &ps);
}

/* The slider, drawn like the GTK one: a thin trough, filled in orange up to a round knob. */
static LRESULT draw_slider(NMCUSTOMDRAW *cd)
{
    if (cd->dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT)
        return CDRF_DODEFAULT;
    if (cd->dwItemSpec == TBCD_THUMB)
        return CDRF_SKIPDEFAULT;   /* drawn with the channel */
    if (cd->dwItemSpec != TBCD_CHANNEL)
        return CDRF_DODEFAULT;

    HDC dc = cd->hdc;
    RECT ch = cd->rc, th;
    SendMessageW(A.slider, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
    BOOL on = IsWindowEnabled(A.slider);
    int cy = (th.top + th.bottom) / 2, cx = (th.left + th.right) / 2, h = S(2) > 1 ? S(2) : 2;
    RECT trough = { ch.left, cy - h, ch.right, cy + h };
    HBRUSH b = CreateSolidBrush(COL_TROUGH);
    FillRect(dc, &trough, b);
    DeleteObject(b);
    RECT fill = { ch.left, cy - h, cx, cy + h };
    b = CreateSolidBrush(on ? COL_ACCENT : COL_DIM);
    FillRect(dc, &fill, b);
    DeleteObject(b);

    int r = S(10);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xC0, 0xBF, 0xBC));
    HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, A.b_card);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, op);
    SelectObject(dc, obr);
    DeleteObject(pen);
    return CDRF_SKIPDEFAULT;
}

static void set_text_if(HWND w, const wchar_t *t)
{
    wchar_t cur[512];
    GetWindowTextW(w, cur, 512);
    if (wcscmp(cur, t))
        SetWindowTextW(w, t);
}

static void update_ui(void)
{
    wchar_t b[400];
    int raw;
    int st = A.ready ? ctl_state(&raw) : (raw = -1, ST_NA);
    if (st != A.state) {
        A.state = st;
        InvalidateRect(A.hwnd, NULL, FALSE);
    }

    switch (st) {
    case ST_NA:     wcscpy(b, L"Not available"); break;
    case ST_AUTO:   wcscpy(b, L"Fan: Automatic"); break;
    case ST_MANUAL: swprintf(b, 400, L"Fan: Manual %d%%", C.pct); break;
    default:        wcscpy(b, L"Dust Cleaning is running"); break;
    }
    set_text_if(A.title, b);

    BOOL ok = st != ST_NA;
    EnableWindow(A.auto_btn, ok && st != ST_AUTO);
    EnableWindow(A.slider, ok);
    EnableWindow(A.apply, ok);
    EnableWindow(A.dust_btn, ok && vpc_ok() && st != ST_DUST);

    /* sensors */
    int duty = read_duty(), rpm = read_rpm();
    if (rpm >= 0)
        swprintf(b, 400, L"about %d rpm", rpm);
    else if (st == ST_MANUAL && !C.direct)
        swprintf(b, 400, L"not measured (average target %d rpm)", FULL_UNITS * C.pct);
    else
        wcscpy(b, ok ? L"measured with PawnIO only" : L"-");
    set_text_if(A.rpm, b);
    if (duty >= 0)
        swprintf(b, 400, L"%d%%", duty * 100 / DUTY_MAX);
    else
        wcscpy(b, L"-");
    set_text_if(A.power, b);
    int t = (int)g_ssd_temp;
    if (t >= 0)
        swprintf(b, 400, L"%d \x00B0" L"C", t);
    else
        wcscpy(b, L"-");
    set_text_if(A.ssd, b);
    if (raw >= 0)
        swprintf(b, 400, L"%d (0x%02X)", raw, raw);
    else
        wcscpy(b, vpc_ok() ? L"-" : L"Lenovo Energy Management driver not found");
    set_text_if(A.raw, b);
    set_text_if(A.method, g_pwm ? L"PawnIO: steady speed"
                        : vpc_ok() ? L"Lenovo driver: speed pulses (no PawnIO)"
                        : L"none");

    /* message */
    BOOL err = A.note_error;
    if (C.rel) {
        wcscpy(b, L"Returning the fan to the firmware's own speed...");
        err = FALSE;
        A.releasing = TRUE;
    } else if (st == ST_MANUAL && C.direct && C.duty > pct_duty(C.pct)) {
        wcscpy(b, L"The laptop is warm: the firmware asks for more cooling, so the fan "
                  L"is running faster than your setting.");
        err = FALSE;
    } else {
        if (A.releasing) {
            A.releasing = FALSE;
            set_note(L"Fan returned to automatic control.", FALSE);
        }
        wcscpy(b, A.note);
        err = A.note_error;
    }
    LONG_PTR fl = err ? F_ERROR : 0;
    if (GetWindowLongPtrW(A.msg, GWLP_USERDATA) != fl) {
        SetWindowLongPtrW(A.msg, GWLP_USERDATA, fl);
        InvalidateRect(A.msg, NULL, TRUE);
    }
    set_text_if(A.msg, b);
}

static void on_apply(void)
{
    int pct = slider_pct();
    wchar_t b[400];
    if (!ctl_manual(pct)) {
        set_note(L"Could not change the fan setting.", TRUE);
    } else if (C.direct) {
        swprintf(b, 400, L"Manual speed set to %d%%. Keep this window open or minimised; "
                         L"closing it returns the fan to automatic control.", pct);
        set_note(b, FALSE);
    } else {
        swprintf(b, 400, L"Manual speed set to %d%%. Without PawnIO the fan pulses around this "
                         L"average. Keep this window open or minimised.", pct);
        set_note(b, FALSE);
    }
    update_ui();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_HSCROLL:
        if ((HWND)lp == A.slider) {
            wchar_t b[16];
            swprintf(b, 16, L"%d%%", slider_pct());
            SetWindowTextW(A.pct, b);
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (h->hwndFrom == A.slider && h->code == NM_CUSTOMDRAW)
            return draw_slider((NMCUSTOMDRAW *)lp);
        break;
    }
    case WM_COMMAND:
        if (!A.ready)
            return 0;
        switch (LOWORD(wp)) {
        case ID_APPLY:
            on_apply();
            break;
        case ID_AUTO:
            if (ctl_auto(FALSE))
                set_note(L"Fan returned to automatic control.", FALSE);
            else
                set_note(L"Could not change the fan setting.", TRUE);
            update_ui();
            break;
        case ID_DUST:
            if (ctl_dust())
                set_note(L"Dust Cleaning started. Choose Automatic to stop it.", FALSE);
            else
                set_note(L"Could not start Dust Cleaning.", TRUE);
            update_ui();
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == ID_ANIM) {
            double spin = A.state == ST_MANUAL ? 0.06 + 0.26 * C.pct / 100.0
                        : A.state == ST_DUST   ? 0.32 : 0.06;
            if (A.state != ST_NA && !IsIconic(hwnd)) {
                A.angle = fmod(A.angle + spin, 2 * M_PI);
                RECT r = { 0, S(22), S(WIN_W), S(22 + ART) };
                InvalidateRect(hwnd, &r, FALSE);
            }
            return 0;
        }
        if (A.ready) {
            if (!ctl_tick() && C.mode == ST_MANUAL) {
                C.mode = ST_AUTO;
                vpc_fan(0);
                set_note(L"Lost contact with the embedded controller. "
                         L"The fan is back on automatic control.", TRUE);
            }
            if (++A.ticks % 4 == 0)
                update_ui();
        }
        if (A.closing && !C.rel)
            DestroyWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC: {
        LONG_PTR f = GetWindowLongPtrW((HWND)lp, GWLP_USERDATA);
        HDC dc = (HDC)wp;
        SetTextColor(dc, f & F_ERROR ? COL_ERROR : f & F_DIM ? COL_DIM : COL_TEXT);
        SetBkColor(dc, f & F_CARD ? COL_CARD : COL_BG);
        return (LRESULT)(f & F_CARD ? A.b_card : A.b_bg);
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)A.b_card;
    case WM_CLOSE:
        /* Hand the fan back to the firmware before quitting. */
        if (A.ready && (C.mode == ST_MANUAL || C.rel)) {
            ctl_auto(FALSE);
            if (C.rel) {
                A.closing = TRUE;
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_QUERYENDSESSION:
        if (A.ready && C.mode == ST_MANUAL)
            ctl_auto(FALSE);
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* A card's name and description. */
static void card_texts(int y, const wchar_t *name, const wchar_t *desc, int w, int desc_h)
{
    text(name, MARGIN + 16, y + 14, w, 20, A.f_bold, F_CARD);
    text(desc, MARGIN + 16, y + 36, w, desc_h, A.f_small, F_CARD | F_DIM);
}

static HWND card_button(int y, int h, const wchar_t *label, int id)
{
    return add(L"BUTTON", label, BS_PUSHBUTTON | WS_TABSTOP,
               MARGIN + CARD_W - 16 - 76, y + (h - 32) / 2, 76, 32, id, A.f_text, F_CARD);
}

static HWND sensor_row(int row, const wchar_t *name)
{
    int y = 550 + row * 25;
    text(name, MARGIN, y, 120, 22, A.f_text, F_DIM);
    return text(L"", MARGIN + 128, y, CARD_W - 128, 22, A.f_text, 0);
}

#define REG_KEY L"Software\\IdeaPadFanControl"

/* PawnIO's installer ships next to FanControl.exe. When PawnIO is missing,
 * offer to run it once; "No" is remembered. TRUE when it ran. */
static BOOL offer_pawnio(HWND owner)
{
    wchar_t setup[MAX_PATH];
    DWORD skip = 0, size = sizeof skip;
    if (!g_pio_missing || !beside_exe(setup, L"PawnIO_setup.exe") ||
        GetFileAttributesW(setup) == INVALID_FILE_ATTRIBUTES)
        return FALSE;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"SkipPawnIO", RRF_RT_REG_DWORD, NULL,
                     &skip, &size) == ERROR_SUCCESS && skip)
        return FALSE;
    int r = MessageBoxW(owner,
        L"PawnIO is not installed.\n\n"
        L"With PawnIO the fan holds a steady speed. Without it Fan Control still works, "
        L"but a manual speed pulses up and down around the average.\n\n"
        L"Install PawnIO now? (The installer is included with Fan Control.)",
        APP_NAME, MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) {
        skip = 1;
        RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"SkipPawnIO", REG_DWORD, &skip, sizeof skip);
        return FALSE;
    }
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpFile = setup;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
        return FALSE;
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);
    return TRUE;
}

static void read_model(wchar_t *out, DWORD n)
{
    DWORD size = n * sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemVersion",
                     RRF_RT_REG_SZ, NULL, out, &size) != ERROR_SUCCESS || !*out)
        lstrcpynW(out, L"Unknown", n);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1)
        return run_cli(argc, argv);

    HANDLE once = CreateMutexW(NULL, TRUE, L"Local\\IdeaPadFanControlWindow");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"IdeaPadFanControl", NULL);
        if (other) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(NULL);
    A.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    A.b_bg = CreateSolidBrush(COL_BG);
    A.b_card = CreateSolidBrush(COL_CARD);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = A.b_bg;
    wc.lpszClassName = L"IdeaPadFanControl";
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S(WIN_W), S(WIN_H) };
    AdjustWindowRect(&r, style, FALSE);
    A.hwnd = CreateWindowW(wc.lpszClassName, APP_NAME, style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);

    A.f_title = make_font(170, FW_BOLD);
    A.f_text = make_font(100, FW_NORMAL);
    A.f_small = make_font(95, FW_NORMAL);
    A.f_bold = make_font(100, FW_BOLD);

    /* hero: the fan picture is painted; the title sits under it */
    A.title = add(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, MARGIN, 116, CARD_W, 36, 0, A.f_title, 0);

    /* automatic (default) */
    card_texts(CARD1_Y, L"Automatic (default)",
               L"The laptop's firmware sets the fan speed from the temperature.", CARD_W - 32 - 88, 40);
    A.auto_btn = card_button(CARD1_Y, CARD1_H, L"Use", ID_AUTO);

    /* manual speed */
    card_texts(CARD2_Y, L"Manual speed",
               L"Fan power in percent. 100% is about 5200 rpm. Steady with PawnIO "
               L"installed; without it the fan pulses around the average.", CARD_W - 32, 54);
    A.slider = add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | TBS_BOTTOM | WS_TABSTOP,
                   MARGIN + 8, CARD2_Y + 92, 262, 36, ID_SLIDER, A.f_text, F_CARD);
    SendMessageW(A.slider, TBM_SETRANGE, TRUE, MAKELPARAM(PCT_MIN / 5, PCT_MAX / 5));
    SendMessageW(A.slider, TBM_SETTICFREQ, 2, 0);
    SendMessageW(A.slider, TBM_SETPAGESIZE, 0, 2);
    SendMessageW(A.slider, TBM_SETPOS, TRUE, 50 / 5);
    A.pct = add(L"STATIC", L"50%", SS_CENTERIMAGE | SS_RIGHT | SS_NOPREFIX,
                MARGIN + 272, CARD2_Y + 94, 50, 32, 0, A.f_text, F_CARD | F_DIM);
    A.apply = add(L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP,
                  MARGIN + CARD_W - 16 - 76, CARD2_Y + 94, 76, 32, ID_APPLY, A.f_text, F_CARD);

    /* dust cleaning */
    card_texts(CARD3_Y, L"Dust Cleaning",
               L"The firmware's full-speed bursts. Choose Automatic to stop.", CARD_W - 32 - 88, 40);
    A.dust_btn = card_button(CARD3_Y, CARD3_H, L"Start", ID_DUST);

    /* details */
    text(L"Sensors", MARGIN, 522, 200, 22, A.f_bold, 0);
    A.rpm    = sensor_row(0, L"Fan speed");
    A.power  = sensor_row(1, L"Fan power");
    A.ssd    = sensor_row(2, L"SSD");
    A.raw    = sensor_row(3, L"Firmware value");
    A.method = sensor_row(4, L"Control");
    A.model  = sensor_row(5, L"Model");
    wchar_t model[128];
    read_model(model, 128);
    SetWindowTextW(A.model, model);

    A.msg = text(L"", MARGIN, 706, CARD_W, 64, A.f_text, 0);

    A.ready = hw_open();
    if (!g_pwm && offer_pawnio(A.hwnd) && pwm_open()) {
        g_pwm = TRUE;
        A.ready = TRUE;
    }
    if (!A.ready) {
        wchar_t b[400];
        swprintf(b, 400, L"%ls The Lenovo Energy Management driver was not found either. "
                         L"Install PawnIO from pawnio.eu (and put LpcIO.bin next to FanControl.exe), "
                         L"or Lenovo's Energy Management driver.", g_pwm_why);
        set_note(b, TRUE);
    } else if (!g_pwm) {
        wchar_t b[400];
        swprintf(b, 400, L"%ls Manual speed pulses around the average; install PawnIO "
                         L"for a steady speed.", g_pwm_why);
        set_note(b, FALSE);
    }
    CreateThread(NULL, 0, sensor_thread, NULL, 0, NULL);
    A.state = -1;
    update_ui();
    SetTimer(A.hwnd, ID_TIMER, TICK_MS, NULL);
    SetTimer(A.hwnd, ID_ANIM, 40, NULL);
    ShowWindow(A.hwnd, show);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(A.hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
