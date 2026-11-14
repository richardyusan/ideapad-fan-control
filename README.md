# Fan Control

A small app to set the **cooling fan speed** of a Lenovo IdeaPad 3 14ALC6, on Linux and Windows.
It is built the same way as the ConservationMode app (one C file, GTK 3, the in-kernel `ideapad_laptop` driver), but controls the fan instead of the battery.

![Fan Control](docs/screenshot.png)

| Setting | Command | Effect |
|---|---|---|
| Automatic (default) | `auto` | The firmware sets the fan speed from the temperature. |
| Manual speed | `30` to `100` | Fan power in percent. Steady with the kernel driver; an average without it. |
| Dust Cleaning | `dust` | The firmware's full-speed bursts. Choose Automatic to stop it. |

The app also shows the CPU, GPU and SSD temperatures.

## How it works

Found by reverse engineering this laptop's ACPI tables and embedded controller (EC), an ITE IT8227. The scripts and method are in `tools/`.

| What | Where | Needs |
|---|---|---|
| Fan PWM duty | Register `DCR4` at `0x1806` in the EC's internal register space, reached through the chip's I2EC window on Super I/O ports `0x4E`/`0x4F`. `0` = stopped, `200` = full speed. | Secure Boot **off** (the kernel blocks `/dev/port` otherwise) |
| Measured fan speed | Byte `0x06` of the EC's RAM (`FANS` in the DSDT), in units of 100 rpm, read through the in-kernel `ec_sys` module | Works with Secure Boot on or off |
| Fan state | `fan_mode` in the `ideapad_laptop` driver. Only two states: `0` is automatic; `1`, `2` and `4` all start dust cleaning (about 10 s at full speed, a 1 s stop, repeating). | Works with Secure Boot on or off |

The kernel documentation lists `fan_mode` `1` as "Standard" and `4` as "Efficient Thermal Dissipation", but on this model they are the same as dust cleaning (kernel Bugzilla 221353, reported on the sibling 15ALC6).

Manual speed runs as a small root service, which picks the best method available:

| Method | Speed | Secure Boot |
|---|---|---|
| 1. `ideapad_fan_pwm` kernel driver (in `kmod/`) | Steady | On or off (the driver is signed with your own key) |
| 2. PWM register through `/dev/port` | Steady | Must be off |
| 3. Switching `fan_mode` | Swings around an average | On or off |

### Steady speed (methods 1 and 2)

The PWM duty is written directly. The firmware only nudges that register one step per second towards its own target, so rewriting it four times a second holds it. The percentage is the real PWM duty.

Measured on a 14ALC6 (BIOS GLCN68WW), steady to the resolution of the sensor:

| Setting | Fan speed |
|---|---|
| 30% | 2100 rpm |
| 50% | 3200 rpm |
| 60% | 3700 rpm |
| 80% | 4500 rpm |
| 100% | 5200 rpm |

- **Overheat guard:** a manual setting can be slower than the firmware wants, so the service forces 100% while the CPU is at 85 °C or more, until it is back below 75 °C.
- **Only for this EC:** nothing is written unless the chip ID is `0x8227`; the kernel driver also checks for model `82KT`.

### The kernel driver, for Secure Boot on

User space cannot reach the PWM register with Secure Boot on, but a kernel module can, and a module signed with a key you enrolled yourself (a Machine Owner Key, the same mechanism Ubuntu uses for the NVIDIA driver) loads with Secure Boot on.

```sh
sudo kmod/install.sh
```

This builds the driver, creates a signing key if the machine has none, signs and installs the driver, and asks you for a one-time password. Then reboot: on the blue **Perform MOK management** screen choose **Enroll MOK → Continue → Yes**, type the password, and reboot. After that Secure Boot can be on.

The driver is a standard `hwmon` device named `ideapad_fan`, so `sensors`, `fancontrol` and similar tools work with it too:

| File | Meaning |
|---|---|
| `fan1_input` | Measured speed in rpm (readable without root, also in automatic mode) |
| `pwm1` | Duty, 0 to 255 |
| `pwm1_enable` | `1` = manual, `2` = automatic |

- **Fail-safe:** manual mode ends by itself if `pwm1` is not written for 30 s (module parameter `watchdog`), so a crashed program cannot leave the fan slow.
- **Kernel updates:** the driver is built per kernel, so run `sudo kmod/install.sh` again after a kernel update. Until then the app falls back to method 3.
- **Removal:** `sudo kmod/install.sh uninstall`.

### Average speed (method 3)

Without the driver and with Secure Boot on, the service switches `fan_mode` between full speed and automatic so that the **average** measured speed matches the percentage of 5200 rpm. The fan audibly rises and falls between about 2200 and 5200 rpm every few seconds, 100% has a 1 s stop every 11 s, and the average never goes below automatic control.

## Install

```sh
sudo apt install gcc libgtk-3-dev gcc-mingw-w64-x86-64   # build requirements (Ubuntu/Debian)
./build.sh                          # builds dist/FanControl and assets/icon-256.png
./install.sh                        # adds "Fan Control" to your app menu
```

Or run `dist/FanControl` directly without installing. Remove it again with `./install.sh uninstall`.

## Usage

Open **Fan Control** from the app menu. Changing the setting asks for your password (through `pkexec`), because the kernel only lets root change it.

A manual speed is kept after a restart, because its service (`ideapad-fan-control.service`) is enabled at boot. Choosing Automatic stops and disables the service.

### Command line

```sh
FanControl status      # setting and temperatures
FanControl auto        # back to the default
FanControl 60          # manual speed, 30 to 100
FanControl dust
```

## Windows

`dist/FanControl.exe` does the same on Windows, with Secure Boot on. Windows does not let a program touch I/O ports, so it uses [PawnIO](https://pawnio.eu), a signed kernel driver that runs small signed modules; its official `LpcIO` module provides exactly the Super I/O register access needed to reach the EC's PWM register.

**Not yet tested on Windows.** The EC access sequence and the control logic are the ones verified on Linux, and the program builds cleanly, but nobody has run it against a real PawnIO install yet.

Setup:

1. Install PawnIO: download `PawnIO_setup.exe` from <https://pawnio.eu> and run it.
2. Download the modules from <https://github.com/namazso/PawnIO.Modules/releases> and copy `LpcIO.bin` into the same folder as `FanControl.exe`.
3. Run `FanControl.exe`. It asks for administrator rights.

How it behaves:

- **Runs only while open.** The app rewrites the PWM duty four times a second. Keep the window open or minimised; closing it returns the fan to the firmware.
- **Never slower than the firmware asks.** The firmware nudges the duty one step per second towards its own target. If it keeps stepping up, it wants more cooling than your setting, and the app follows it up, then comes back down to your setting when the firmware is satisfied. This needs no temperature sensor. (Measured on Linux with the same logic: a setting of 20% climbed back to the firmware's 35% within 15 s; a setting of 60% held steady.)
- **Only for this EC.** Nothing is written unless the chip ID reads `0x8227`.
- **Lenovo Vantage** may change thermal modes on its own; if the two disagree, the firmware's request wins.

```
FanControl.exe              open the app
FanControl.exe status       show fan power and speed
FanControl.exe 60           hold 60% until Ctrl+C
FanControl.exe --hold 60    the same with no window
```

To hold a speed from logon without opening the window, create a Task Scheduler task that runs `FanControl.exe --hold 60` "with highest privileges" at logon.

## Files

```
src/fan_control_linux.c   Linux app (GTK 3), command line and background service
src/fan_control_win.c     Windows app (Win32, through PawnIO)
src/app.rc, app.manifest  Windows icon, version info and manifest
build.sh                  Build script
install.sh                Per-user installer
kmod/                     ideapad_fan_pwm kernel driver and its signing installer
tools/                    Reverse-engineering scripts (firmware dump, EC probes, PWM write test, speed test)
re-data/                  Raw data those scripts collected (ACPI tables, EC dumps, test logs)
assets/                   App icon (generated by build.sh)
```

Setting a manual speed also creates these root-owned files:

```
/usr/local/libexec/ideapad-fan-control             copy of the app that the service runs
/etc/systemd/system/ideapad-fan-control.service
/etc/ideapad-fan-control.conf                      the chosen percentage
```

## Disclaimer

Not affiliated with or endorsed by Lenovo. Use at your own risk.

## License

[MIT](LICENSE)
