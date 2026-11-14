#!/bin/sh
# Loads the freshly built kmod/ideapad_fan_pwm.ko, exercises its hwmon
# interface on the real fan, and unloads it. Run as root:
#   sudo tools/kmod-test.sh <path to .ko> <log file>
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
KO=${1:?module}; LOG=${2:?log}
exec > "$LOG" 2>&1
systemctl stop ideapad-fan-control.service 2>/dev/null
rmmod ideapad_fan_pwm 2>/dev/null
insmod "$KO" || { echo "insmod failed"; dmesg | tail -5; chmod 644 "$LOG"; exit 1; }
H=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = ideapad_fan ] && echo $h; done)
echo "hwmon: $H"; ls -l $H | grep -E 'fan1|pwm1'
echo "initial: rpm=$(cat $H/fan1_input) pwm=$(cat $H/pwm1) enable=$(cat $H/pwm1_enable)"
hold() { # pwm seconds
    printf 'pwm %s:' "$1"
    i=0; while [ $i -lt "$2" ]; do echo "$1" > $H/pwm1; sleep 1; printf ' %s' "$(cat $H/fan1_input)"; i=$((i+1)); done
    echo "  (readback pwm=$(cat $H/pwm1) enable=$(cat $H/pwm1_enable))"
}
echo 1 > $H/pwm1_enable
hold 128 8; hold 204 7; hold 255 7; hold 77 8
echo "invalid write: $(sh -c "echo 300 > $H/pwm1" 2>&1)"
echo 3 > /sys/module/ideapad_fan_pwm/parameters/watchdog
echo 128 > $H/pwm1
printf 'watchdog (3 s), no more writes: enable='
for i in 1 2 3 4 5 6; do sleep 1; printf '%s ' "$(cat $H/pwm1_enable)"; done; echo
echo 1 > $H/pwm1_enable; echo 2 > $H/pwm1_enable; echo "after explicit auto: enable=$(cat $H/pwm1_enable)"
rmmod ideapad_fan_pwm && echo "rmmod ok"
dmesg | grep ideapad_fan_pwm | tail -5
chmod 644 "$LOG"
