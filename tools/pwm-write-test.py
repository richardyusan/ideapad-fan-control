#!/usr/bin/env python3
"""Step 2: does writing the fan's PWM duty register give a steady speed?

Needs Secure Boot OFF. Run as root:  sudo tools/pwm-write-test.py <log file>

Step 1 (ec-chip-probe.py) found, on the IT8227 embedded controller:
  0x1806  DCR4, the fan's PWM duty: 0 = stopped ... 200 (0xC8) = full speed
  0x181E/0x181F  tachometer period (low/high byte)
This script WRITES 0x1806, and nothing else in the EC. The firmware keeps
running and writes its own value back, so the effect ends by itself when the
script stops. It takes about 90 seconds and bails out if the CPU gets hot.
"""
import glob, os, subprocess, sys, time

DCR4 = 0x1806
BASE = 0x4E
log = open(sys.argv[1], "w")


def out(s=""):
    log.write(s + "\n")
    log.flush()


port = os.open("/dev/port", os.O_RDWR)


def inb(p):
    return os.pread(port, 1, p)[0]


def outb(v, p):
    os.pwrite(port, bytes([v]), p)


def _select(addr):
    for reg, val in ((0x11, addr >> 8), (0x10, addr & 0xFF)):
        outb(0x2E, BASE); outb(reg, BASE + 1)
        outb(0x2F, BASE); outb(val, BASE + 1)
    outb(0x2E, BASE); outb(0x12, BASE + 1)
    outb(0x2F, BASE)


def rd(addr):
    _select(addr)
    return inb(BASE + 1)


def wr(addr, val):
    _select(addr)
    outb(val, BASE + 1)


chip = (rd(0x2000) << 8) | rd(0x2001)
out("chip id 0x%04X" % chip)
if chip != 0x8227:
    out("not the expected IT8227; not writing anything")
    print("unexpected chip, stopped")
    sys.exit(1)

subprocess.run(["modprobe", "ec_sys"], check=False)
subprocess.run(["systemctl", "stop", "ideapad-fan-control.service"],
               stderr=subprocess.DEVNULL, check=False)
ec = os.open("/sys/kernel/debug/ec/ec0/io", os.O_RDONLY)
fan = glob.glob("/sys/bus/platform/drivers/ideapad_acpi/*/fan_mode")[0]
temp_file = [h + "/temp1_input" for h in glob.glob("/sys/class/hwmon/hwmon*")
             if open(h + "/name").read().strip() == "k10temp"][0]


def temp():
    return int(open(temp_file).read()) // 1000


def rpm():
    return os.pread(ec, 1, 0x06)[0]


def watch(name, seconds, hz=20):
    """Only read: shows how often and to what the firmware sets the duty."""
    vals = []
    t0 = time.monotonic()
    k = 0
    while time.monotonic() - t0 < seconds:
        vals.append("%d" % rd(DCR4))
        k += 1
        time.sleep(max(0, t0 + k / hz - time.monotonic()))
    out("%s  (duty at %d Hz, rpm/100 now %d, cpu %d C)\n  %s" % (name, hz, rpm(), temp(), " ".join(vals)))


def hold(duty, seconds, hz=20):
    """Rewrite the duty at hz; log the speed and how often the firmware had changed it."""
    overwritten = 0
    writes = 0
    seen = {}
    speeds = []
    t0 = time.monotonic()
    k = 0
    while time.monotonic() - t0 < seconds:
        if temp() >= 85:
            out("  CPU reached 85 C, stopping the test")
            raise SystemExit(2)
        before = rd(DCR4)
        if k and before != duty:
            overwritten += 1
            seen[before] = seen.get(before, 0) + 1
        wr(DCR4, duty)
        writes += 1
        if k % (hz // 2) == 0:
            speeds.append("%d" % rpm())
        k += 1
        time.sleep(max(0, t0 + k / hz - time.monotonic()))
    out("hold duty %d (%d%%) for %ds at %d Hz: firmware had overwritten %d of %d times %s, cpu %d C\n  rpm/100 every 0.5s: %s"
        % (duty, duty // 2, seconds, hz, overwritten, writes, seen, temp(), " ".join(speeds)))


try:
    with open(fan, "w") as f:
        f.write("0")
    time.sleep(3)
    watch("automatic, no writes", 10)
    out("single write of duty 120:")
    wr(DCR4, 120)
    watch("  after one write", 6)
    for duty in (120, 160, 200, 100, 60, 140):
        hold(duty, 12)
    hold(120, 8, hz=5)
    hold(120, 8, hz=2)
    watch("released, no writes", 12)
finally:
    with open(fan, "w") as f:
        f.write("0")
    log.close()
    os.chmod(sys.argv[1], 0o644)
print("done")
