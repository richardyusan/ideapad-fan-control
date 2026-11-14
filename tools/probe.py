#!/usr/bin/env python3
"""Maps how the firmware's fan state machine reacts to fan_mode writes.

Run as root:  sudo tools/probe.py <log file>
Samples the EC fan speed byte (0x06, units of 100 rpm) every 0.1 s while
writing different values and toggle patterns to fan_mode. Takes ~4 minutes
and leaves the fan in automatic control.
"""
import glob, os, subprocess, sys, time

log = open(sys.argv[1], "w")
subprocess.run(["modprobe", "ec_sys"], check=False)
subprocess.run(["systemctl", "stop", "ideapad-fan-control.service"],
               stderr=subprocess.DEVNULL, check=False)
ec = os.open("/sys/kernel/debug/ec/ec0/io", os.O_RDONLY)
fan = glob.glob("/sys/bus/platform/drivers/ideapad_acpi/*/fan_mode")[0]


def w(v):
    with open(fan, "w") as f:
        f.write(str(v))


def rd(off):
    return os.pread(ec, 1, off)[0]


def phase(name, seconds, action=None):
    """action(t) is called every tick and may write to fan_mode."""
    out = []
    t0 = time.monotonic()
    k = 0
    while True:
        t = time.monotonic() - t0
        if t >= seconds:
            break
        if action:
            action(k)
        out.append("%d%s" % (rd(0x06), "" if rd(0x01) == 0x85 else "*"))
        k += 1
        time.sleep(max(0, t0 + k * 0.1 - time.monotonic()))
    log.write("%s: %s\n" % (name, " ".join(out)))
    log.flush()


def rest():
    w(0)
    phase("rest", 6)


def once(v):
    return lambda k: w(v) if k == 0 else None


def toggle(on, off, val=2):
    per = on + off
    def a(k):
        if k % per == 0:
            w(val)
        elif k % per == on:
            w(0)
    return a


try:
    rest()
    phase("write2 natural 60s", 60, once(2)); rest()
    phase("write1 20s", 20, once(1)); rest()
    phase("write4 20s", 20, once(4)); rest()
    phase("rewrite2 every 1s (no 0) 20s", 20, lambda k: w(2) if k % 10 == 0 else None); rest()
    phase("rewrite2 every 3s (no 0) 20s", 20, lambda k: w(2) if k % 30 == 0 else None); rest()
    phase("toggle 1.0s on / 1.0s off", 20, toggle(10, 10)); rest()
    phase("toggle 0.5s on / 0.5s off", 16, toggle(5, 5)); rest()
    phase("toggle 0.3s on / 0.3s off", 12, toggle(3, 3)); rest()
    phase("toggle 2.0s on / 0.5s off", 20, toggle(20, 5)); rest()
    phase("toggle 4.0s on / 0.2s off", 25, toggle(40, 2)); rest()
finally:
    w(0)
    log.close()
    os.chmod(sys.argv[1], 0o644)
print("done")
