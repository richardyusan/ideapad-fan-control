#!/bin/sh
# Tests the app together with the (unsigned, freshly built) kernel driver.
#   sudo tools/app-driver-test.sh <module.ko> <FanControl binary> <log>
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
KO=$1; BIN=$2; LOG=$3
exec > "$LOG" 2>&1
rmmod ideapad_fan_pwm 2>/dev/null
insmod "$KO" || { echo insmod failed; chmod 644 "$LOG"; exit 1; }
echo "== auto, driver loaded"; "$BIN" status
"$BIN" --apply 60; echo "apply 60 exit $?"; sleep 6
echo "== manual 60"; "$BIN" status; cat /run/ideapad-fan-control
H=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = ideapad_fan ] && echo $h; done)
echo "driver: enable=$(cat $H/pwm1_enable) pwm=$(cat $H/pwm1)"
"$BIN" --apply auto; echo "apply auto exit $?"; sleep 1
echo "== after auto"; "$BIN" status; echo "driver: enable=$(cat $H/pwm1_enable)"
systemctl is-active ideapad-fan-control.service
rmmod ideapad_fan_pwm && echo "rmmod ok"
chmod 644 "$LOG"
