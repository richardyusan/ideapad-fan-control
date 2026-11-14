/*
 * Fan Control for Windows (Lenovo IdeaPad 3 14ALC6)
 *
 * Sets the cooling fan's power in percent by writing the PWM duty register
 * of the laptop's embedded controller (EC, an ITE IT8227):
 *   DCR4 at 0x1806 in the EC's register space: 0 = stopped, 200 = full speed
 *   tachometer period at 0x181E (low) / 0x181F (high)
 * The registers are reached through the chip's "I2EC" window on the Super
 * I/O ports 0x4E/0x4F. See the Linux source and tools/ for how they were found.
 *
 * Windows does not let a program touch I/O ports, so this uses PawnIO
 * (https://pawnio.eu), a signed kernel driver that runs small signed
 * modules. The official LpcIO module offers exactly the Super I/O register
 * read/write needed here. Because the driver and module are signed, this
 * works with Secure Boot on. Requirements:
 *   - PawnIO installed (PawnIO_setup.exe from pawnio.eu)
 *   - LpcIO.bin (from the PawnIO.Modules releases) next to FanControl.exe
 *   - administrator rights (the app asks through UAC)
 *
 * The firmware keeps running and moves the duty one step per second towards
 * its own target, so the app rewrites the duty four times a second. That
 * also tells us what the firmware wants: if it keeps stepping up, it wants
 * more cooling than the chosen value, and the app then follows it up. The
 * fan therefore never runs slower than the firmware asks for, and no
 * temperature sensor is needed for safety.
 *
 * The speed is held only while this program runs; closing it returns the
 * fan to the firmware.
 *
 *   FanControl.exe              open the app
 *   FanControl.exe status       show fan power and speed
 *   FanControl.exe N            hold N percent (30..100) until Ctrl+C
 *   FanControl.exe --hold N     same, with no window (for Task Scheduler)
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <winternl.h>
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>

#define APP_NAME    L"Fan Control"
#define APP_VERSION L"2.1.0"

/* --- PawnIO device interface (from PawnIO's pawnio_um.h / PawnIOLib.cpp) --- */
#define PIO_DEVICE_TYPE   41394
#define IOCTL_PIO_LOAD    CTL_CODE(PIO_DEVICE_TYPE, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PIO_EXECUTE CTL_CODE(PIO_DEVICE_TYPE, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define PIO_NAME_LEN      32
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif

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

static HANDLE g_pio = INVALID_HANDLE_VALUE;
static HANDLE g_bus_mutex;   /* convention shared by hardware monitors for the Super I/O ports */
static wchar_t g_error[512];

static void set_error(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(g_error, 511, fmt, ap);
    g_error[511] = 0;
    va_end(ap);
}

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

/* Opens PawnIO, loads LpcIO.bin and checks that the EC is the expected chip. */
static BOOL hw_open(void)
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
        set_error(L"PawnIO is not available (status 0x%08lX).\n"
                  L"Install PawnIO from https://pawnio.eu and run Fan Control as administrator.",
                  (unsigned long)st);
        return FALSE;
    }

    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (len && path[len - 1] != L'\\')
        len--;
    path[len] = 0;
    if (len + 10 >= MAX_PATH) {
        set_error(L"The program's folder path is too long.");
        return FALSE;
    }
    wcscat(path, L"LpcIO.bin");

    DWORD size = 0;
    BYTE *blob = read_whole_file(path, &size);
    if (!blob) {
        set_error(L"LpcIO.bin was not found next to FanControl.exe.\n"
                  L"Download the PawnIO modules from\n"
                  L"https://github.com/namazso/PawnIO.Modules/releases\n"
                  L"and copy LpcIO.bin into:\n%ls", path);
        return FALSE;
    }
    DWORD got = 0;
    BOOL ok = DeviceIoControl(g_pio, IOCTL_PIO_LOAD, blob, size, NULL, 0, &got, NULL);
    DWORD e = GetLastError();
    HeapFree(GetProcessHeap(), 0, blob);
    if (!ok) {
        set_error(L"PawnIO refused LpcIO.bin (error %lu).\n"
                  L"Use the signed LpcIO.bin from the official PawnIO.Modules release.", e);
        return FALSE;
    }

    g_bus_mutex = CreateMutexW(NULL, FALSE, L"Global\\Access_ISABUS.HTP.Method");
    if (!g_bus_mutex) {
        set_error(L"Could not create the bus mutex (error %lu).", GetLastError());
        return FALSE;
    }

    ULONG64 slot = 1; /* Super I/O at 0x4E/0x4F */
    if (!pio_execute("ioctl_select_slot", &slot, 1, NULL, 0)) {
        set_error(L"The LpcIO module did not accept the port selection (error %lu).", GetLastError());
        return FALSE;
    }
    int hi = i2ec_read(EC_CHIPID1), lo = i2ec_read(EC_CHIPID2);
    if (hi < 0 || lo < 0 || ((hi << 8) | lo) != EC_CHIP) {
        set_error(L"This laptop's embedded controller is not the expected ITE IT8227\n"
                  L"(chip id read: %02X%02X). Fan Control only supports the IdeaPad 3 14ALC6\n"
                  L"and will not write anything here.", hi & 0xFF, lo & 0xFF);
        return FALSE;
    }
    return TRUE;
}

static int read_duty(void) { return i2ec_read(EC_DCR4); }

static int read_rpm(void)
{
    int lo = i2ec_read(EC_TACH_LO), hi = i2ec_read(EC_TACH_HI);
    if (lo < 0 || hi < 0)
        return -1;
    int period = (hi << 8) | lo;
    return period ? TACH_K / period : 0;
}

/* --- holding a speed --- */

typedef struct {
    BOOL manual;
    int pct;        /* what the user asked for */
    int duty;       /* what we currently hold; above pct*2 while the firmware wants more */
} Hold;

static Hold H;

static void hold_start(int pct)
{
    H.manual = TRUE;
    H.pct = pct;
    H.duty = pct * DUTY_MAX / 100;
    i2ec_write(EC_DCR4, H.duty);
}

static void hold_stop(void) { H.manual = FALSE; }

/* Called every TICK_MS. Returns FALSE when the EC could not be reached. */
static BOOL hold_tick(void)
{
    if (!H.manual)
        return TRUE;
    int want = H.pct * DUTY_MAX / 100;
    int now = read_duty();
    if (now < 0)
        return FALSE;
    /* The firmware steps the register towards its own target. A step up means
     * it wants more cooling than we hold: follow it. A step down while we are
     * above the user's value means it is satisfied again: follow it back. */
    if (now > H.duty)
        H.duty = now > DUTY_MAX ? DUTY_MAX : now;
    else if (now < H.duty && H.duty > want)
        H.duty = now;
    if (H.duty < want)
        H.duty = want;
    return i2ec_write(EC_DCR4, H.duty);
}

/* --- command line --- */

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

static int run_hold(int pct, BOOL quiet)
{
    if (!hw_open()) {
        if (!quiet)
            fwprintf(stderr, L"%ls\n", g_error);
        return 1;
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    hold_start(pct);
    if (!quiet)
        wprintf(L"Holding the fan at %d%%. Press Ctrl+C to return to automatic.\n", pct);
    for (int k = 0; !g_stop; k++) {
        Sleep(TICK_MS);
        hold_tick();
        if (!quiet && k % 8 == 7) {
            wprintf(L"\r%d%% power, about %d rpm   ", H.duty * 100 / DUTY_MAX, read_rpm());
            fflush(stdout);
        }
    }
    if (!quiet)
        wprintf(L"\nReturned to automatic control.\n");
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
    if (argc == 2 && !wcscmp(argv[1], L"status")) {
        if (!hw_open()) {
            fwprintf(stderr, L"%ls\n", g_error);
            return 1;
        }
        int duty = read_duty();
        wprintf(L"Fan power: %d%%\nFan speed: about %d rpm\n", duty * 100 / DUTY_MAX, read_rpm());
        return 0;
    }
    int pct = argc == 2 ? parse_pct(argv[1]) : -1;
    if (pct < 0) {
        fwprintf(stderr,
            L"Usage: FanControl.exe [status|PERCENT|--hold PERCENT|--version]\n"
            L"  (no argument)   open the app\n"
            L"  status          show fan power and speed\n"
            L"  PERCENT         hold %d to %d percent until Ctrl+C\n"
            L"  --hold PERCENT  the same with no window, for Task Scheduler\n", PCT_MIN, PCT_MAX);
        return 2;
    }
    return run_hold(pct, FALSE);
}

/* --- window --- */

enum { ID_SLIDER = 101, ID_APPLY, ID_AUTO, ID_TIMER = 1 };

static struct {
    HWND hwnd, title, speed, note, slider, pct, apply, automatic, msg;
    HFONT f_title, f_text, f_bold;
    int dpi, ticks;
    BOOL ready;
} A;

static int S(int v) { return MulDiv(v, A.dpi, 96); }

static HFONT make_font(int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, A.dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Segoe UI");
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h,
                int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             A.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static int slider_pct(void)
{
    return (int)SendMessageW(A.slider, TBM_GETPOS, 0, 0) * 5;
}

static void update_labels(void)
{
    wchar_t b[160];
    if (!A.ready) {
        SetWindowTextW(A.title, L"Not available");
        SetWindowTextW(A.speed, L"");
        return;
    }
    if (H.manual)
        swprintf(b, 160, L"Fan: Manual %d%%", H.pct);
    else
        wcscpy(b, L"Fan: Automatic");
    SetWindowTextW(A.title, b);

    int duty = read_duty(), rpm = read_rpm();
    if (duty < 0 || rpm < 0)
        wcscpy(b, L"Fan speed: unknown");
    else
        swprintf(b, 160, L"Fan speed: about %d rpm   \x00B7   power now %d%%", rpm, duty * 100 / DUTY_MAX);
    SetWindowTextW(A.speed, b);

    if (H.manual && H.duty > H.pct * DUTY_MAX / 100)
        SetWindowTextW(A.msg, L"The laptop is warm: the firmware asks for more cooling, so the fan "
                              L"is running faster than your setting.");
    else if (H.manual)
        SetWindowTextW(A.msg, L"Keep this window open or minimised. Closing it returns the fan "
                              L"to automatic control.");
    else
        SetWindowTextW(A.msg, L"The laptop's firmware sets the fan speed from the temperature.");
    EnableWindow(A.automatic, H.manual);
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
    case WM_COMMAND:
        if (LOWORD(wp) == ID_APPLY && A.ready) {
            hold_start(slider_pct());
            update_labels();
        } else if (LOWORD(wp) == ID_AUTO) {
            hold_stop();
            update_labels();
        }
        return 0;
    case WM_TIMER:
        if (A.ready) {
            if (!hold_tick() && H.manual) {
                hold_stop();
                SetWindowTextW(A.msg, L"Lost contact with the embedded controller. "
                                      L"The fan is back on automatic control.");
            }
            if (++A.ticks % 4 == 0)
                update_labels();
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        hold_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
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

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"IdeaPadFanControl";
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, S(460), S(330) };
    AdjustWindowRect(&r, style, FALSE);
    A.hwnd = CreateWindowW(wc.lpszClassName, APP_NAME, style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);

    A.f_title = make_font(17, FW_BOLD);
    A.f_text = make_font(10, FW_NORMAL);
    A.f_bold = make_font(10, FW_BOLD);

    A.title = add(L"STATIC", L"", SS_CENTER, 20, 18, 420, 36, 0, A.f_title);
    A.speed = add(L"STATIC", L"", SS_CENTER, 20, 58, 420, 22, 0, A.f_text);

    add(L"STATIC", L"Manual speed", 0, 24, 100, 300, 20, 0, A.f_bold);
    add(L"STATIC", L"Fan power in percent. 100% is about 5200 rpm. The fan never runs slower "
                   L"than the laptop's firmware asks for.", 0, 24, 122, 412, 38, 0, A.f_text);
    A.slider = add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 18, 164, 290, 34,
                   ID_SLIDER, A.f_text);
    SendMessageW(A.slider, TBM_SETRANGE, TRUE, MAKELPARAM(PCT_MIN / 5, PCT_MAX / 5));
    SendMessageW(A.slider, TBM_SETPOS, TRUE, 50 / 5);
    A.pct = add(L"STATIC", L"50%", SS_CENTERIMAGE, 312, 164, 44, 30, 0, A.f_text);
    A.apply = add(L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, 360, 164, 76, 30, ID_APPLY, A.f_text);

    A.automatic = add(L"BUTTON", L"Back to Automatic (default)", BS_PUSHBUTTON | WS_TABSTOP,
                      24, 214, 412, 32, ID_AUTO, A.f_text);
    A.msg = add(L"STATIC", L"", 0, 24, 262, 412, 56, 0, A.f_text);

    A.ready = hw_open();
    if (!A.ready) {
        SetWindowTextW(A.msg, g_error);
        EnableWindow(A.slider, FALSE);
        EnableWindow(A.apply, FALSE);
        EnableWindow(A.automatic, FALSE);
        MoveWindow(A.msg, S(24), S(100), S(412), S(210), TRUE);
        SetWindowPos(A.msg, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        for (HWND c = GetWindow(A.hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            if (c != A.msg && c != A.title)
                ShowWindow(c, SW_HIDE);
    }
    update_labels();
    SetTimer(A.hwnd, ID_TIMER, TICK_MS, NULL);
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
