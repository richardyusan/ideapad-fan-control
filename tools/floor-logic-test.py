#!/usr/bin/env python3
"""Validates, on Linux, the "never below the firmware" logic used by the
Windows app (src/fan_control_win.c, hold_tick) and its tachometer formula.
Needs Secure Boot off (/dev/port). Run as root:
  sudo tools/floor-logic-test.py <log file>
"""
import glob, os, subprocess, sys, time

BASE, DCR4, TACH_K = 0x4E, 0x1806, 2130000
log = open(sys.argv[1], "w")
port = os.open("/dev/port", os.O_RDWR)
inb = lambda p: os.pread(port, 1, p)[0]
outb = lambda v, p: os.pwrite(port, bytes([v]), p)


def sel(a):
    for reg, val in ((0x11, a >> 8), (0x10, a & 0xFF)):
        outb(0x2E, BASE); outb(reg, BASE + 1); outb(0x2F, BASE); outb(val, BASE + 1)
    outb(0x2E, BASE); outb(0x12, BASE + 1); outb(0x2F, BASE)


def rd(a):
    sel(a); return inb(BASE + 1)


def wr(a, v):
    sel(a); outb(v, BASE + 1)


assert (rd(0x2000) << 8 | rd(0x2001)) == 0x8227
subprocess.run(["modprobe", "ec_sys"], check=False)
subprocess.run(["systemctl", "stop", "ideapad-fan-control.service"], stderr=subprocess.DEVNULL, check=False)
subprocess.run(["rmmod", "ideapad_fan_pwm"], stderr=subprocess.DEVNULL, check=False)
ec = os.open("/sys/kernel/debug/ec/ec0/io", os.O_RDONLY)


def tach_rpm():
    period = rd(0x181F) << 8 | rd(0x181E)
    return TACH_K // period if period else 0


def run(want, seconds):
    """Same steps as hold_tick() in the Windows source."""
    duty = want
    wr(DCR4, duty)
    trace = []
    for k in range(int(seconds * 4)):
        time.sleep(0.25)
        now = rd(DCR4)
        if now > duty:
            duty = min(now, 200)
        elif now < duty and duty > want:
            duty = now
        if duty < want:
            duty = want
        wr(DCR4, duty)
        if k % 4 == 3:
            trace.append("%d/%d/%d" % (duty, tach_rpm(), os.pread(ec, 1, 6)[0] * 100))
    log.write("want duty %d: held-duty/tach-rpm/ec-rpm each second\n  %s\n" % (want, " ".join(trace)))
    log.flush()


fw = rd(DCR4)
log.write("firmware duty before the test: %d\n" % fw)
run(120, 10)                 # above the firmware target: must stay at 120
run(max(fw - 30, 4), 22)     # below the firmware target: must climb back towards it
run(200, 6)
run(160, 6)
log.write("released; firmware duty now %d\n" % rd(DCR4))
log.close()
os.chmod(sys.argv[1], 0o644)
print("done")
