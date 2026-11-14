# Reverse-engineering data

Raw data collected on one IdeaPad 3 14ALC6 (82KT, BIOS GLCN68WW, kernel 7.0) with the scripts in `../tools/`. It is here so the findings in the main README can be checked or repeated.

| File | Made by | Contents |
|---|---|---|
| `acpi/*.dat`, `acpi/*.dsl` | `collect.sh` | The laptop's ACPI tables (DSDT, SSDT1-8) and their `iasl` disassembly. The EC field names (`FANS`, `FCMO`, ...) come from `DSDT.dsl`. |
| `ec/samples.txt` | `collect.sh` | The EC's 256-byte RAM every 0.5 s: fan automatic, then full speed, then automatic. Byte `0x06` follows the fan speed. |
| `dmesg.txt`, `lockdown.txt` | `collect.sh` | Kernel messages about the EC and the `ideapad_laptop` driver; kernel lockdown state. |
| `probe.log` | `probe.py` | Fan speed every 0.1 s while writing values and toggle patterns to `fan_mode`. A `*` after a value is not meaningful. |
| `speed-test.log`, `speed-test2.log` | `test-speed.sh` | Fan speed with the `fan_mode` switching method (Secure Boot on). |
| `ec-chip.log` | `ec-chip-probe.py` | EC chip ID (IT8227), GPIO block, and the PWM block `0x1800-0x184F` while automatic and at full speed. `0x1806` is the fan duty. The "EC RAM 0x0000-0x07FF" part did not read real data and can be ignored. |
| `pwm-write.log` | `pwm-write-test.py` | Fan speed while holding different PWM duties, and how the firmware steps the duty back. |
| `speed-test3.log` | `test-speed.sh` | Fan speed with direct PWM control. |
| `kmod-test.log`, `app-driver-test.log` | `kmod-test.sh`, `app-driver-test.sh` | The kernel driver's hwmon interface, alone and with the app. |
| `floor-logic.log` | `floor-logic-test.py` | The Windows app's "never below the firmware" logic, run on Linux. |

Two things were removed before publishing: the machine's hostname (from `dmesg.txt`) and the battery serial number (EC RAM bytes `0xC4-0xC5`, shown as `XXXX` in `ec/samples.txt`).

The ACPI tables are part of Lenovo's firmware and are included only for interoperability research.
