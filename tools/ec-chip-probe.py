#!/usr/bin/env python3
"""Step 1 of the search for a real fan PWM register. READ-ONLY on the EC.

Needs Secure Boot OFF (the kernel lockdown otherwise blocks /dev/port).
Run as root:  sudo tools/ec-chip-probe.py <log file>

What it does:
  1. Reads the embedded controller's chip ID from the Super I/O config
     ports (0x4E/0x4F, then 0x2E/0x2F).
  2. If it is an ITE chip, reads its internal registers through the "I2EC"
     window: the PWM block (0x1800-0x184F: prescalers, duty registers DCR0-7,
     tachometers) and the GPIO function block.
  3. Repeats the PWM block read while the fan is automatic and while it is
     at full speed, so the duty register that drives the fan shows up as the
     byte that changes.

The only thing it writes is fan_mode (the same file the app uses) and the
Super I/O index registers that select which byte to read. It never writes
EC memory. The fan is left in automatic control.
"""
import glob, os, subprocess, sys, time

log = open(sys.argv[1], "w")


def out(s=""):
    log.write(s + "\n")
    log.flush()


try:
    port = os.open("/dev/port", os.O_RDWR)
    os.pread(port, 1, 0x80)
except OSError as e:
    out("Cannot use /dev/port: %s" % e)
    out("lockdown: " + open("/sys/kernel/security/lockdown").read().strip())
    print("Cannot use /dev/port (is Secure Boot still on?): %s" % e)
    sys.exit(1)


def inb(p):
    return os.pread(port, 1, p)[0]


def outb(v, p):
    os.pwrite(port, bytes([v]), p)


def sio_read(base, idx):
    outb(idx, base)
    return inb(base + 1)


BASE = None
for base in (0x4E, 0x2E):
    cid = (sio_read(base, 0x20) << 8) | sio_read(base, 0x21)
    rev = sio_read(base, 0x22)
    out("Super I/O at 0x%02X: chip id 0x%04X rev 0x%02X" % (base, cid, rev))
    if BASE is None and (cid >> 8) in (0x55, 0x81, 0x82, 0x83, 0x85, 0x86, 0x89):
        BASE = base

if BASE is None:
    out("No ITE embedded controller recognised; stopping here.")
    print("done (chip not recognised, see log)")
    sys.exit(0)


def i2ec_read(addr):
    """Read one byte of EC internal memory through the ITE I2EC window."""
    for reg, val in ((0x11, addr >> 8), (0x10, addr & 0xFF)):
        outb(0x2E, BASE); outb(reg, BASE + 1)
        outb(0x2F, BASE); outb(val, BASE + 1)
    outb(0x2E, BASE); outb(0x12, BASE + 1)
    outb(0x2F, BASE)
    return inb(BASE + 1)


def dump(start, length):
    return bytes(i2ec_read(start + i) for i in range(length))


def show(title, start, data):
    out(title)
    for i in range(0, len(data), 16):
        out("  %04X: %s" % (start + i, " ".join("%02x" % b for b in data[i:i + 16])))


out("ECHIPID1/2/VER (0x2000-0x2002): %s" % dump(0x2000, 3).hex())
show("GPIO control block 0x1600-0x16FF", 0x1600, dump(0x1600, 0x100))

subprocess.run(["modprobe", "ec_sys"], check=False)
subprocess.run(["systemctl", "stop", "ideapad-fan-control.service"],
               stderr=subprocess.DEVNULL, check=False)
ec = os.open("/sys/kernel/debug/ec/ec0/io", os.O_RDONLY)
fan = glob.glob("/sys/bus/platform/drivers/ideapad_acpi/*/fan_mode")[0]


def w(v):
    with open(fan, "w") as f:
        f.write(str(v))


def series(name, seconds):
    out(name)
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        rpm = os.pread(ec, 1, 0x06)[0]
        out("  rpm=%02d  %s" % (rpm, dump(0x1800, 0x50).hex()))
        time.sleep(0.4)


try:
    w(0); time.sleep(4)
    series("PWM block 0x1800-0x184F, fan automatic", 6)
    w(2)
    series("PWM block 0x1800-0x184F, fan full speed", 14)
    w(0)
    series("PWM block 0x1800-0x184F, fan automatic again", 6)
    # The EC's own RAM area, where the firmware keeps its fan target/duty variables.
    show("EC RAM 0x0000-0x07FF (automatic)", 0x0000, dump(0x0000, 0x800))
    w(2); time.sleep(4)
    show("EC RAM 0x0000-0x07FF (full speed)", 0x0000, dump(0x0000, 0x800))
finally:
    w(0)
    log.close()
    os.chmod(sys.argv[1], 0o644)
print("done")
