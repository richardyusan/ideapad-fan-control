#!/bin/sh
# Collects firmware data for reverse engineering the fan control. Run as root:
#   sudo tools/collect.sh <output-dir> [owner]
#
# Reads only, except for fan_mode (the same file the app writes):
#   - copies the ACPI tables (DSDT, SSDT*)
#   - loads the in-kernel ec_sys module (read-only) and snapshots the embedded
#     controller's 256-byte RAM while the fan is automatic, then at full speed
#     (dust cleaning, about 25 s), then automatic again
# Diffing the snapshots shows which EC registers follow the fan.
set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
OUT=${1:?usage: collect.sh <output-dir> [owner]}
OWNER=${2:-}
mkdir -p "$OUT/acpi" "$OUT/ec"

for t in /sys/firmware/acpi/tables/DSDT /sys/firmware/acpi/tables/SSDT*; do
    [ -f "$t" ] && cat "$t" > "$OUT/acpi/$(basename "$t").dat"
done

modprobe ec_sys
EC=/sys/kernel/debug/ec/ec0/io
FAN=$(ls /sys/bus/platform/drivers/ideapad_acpi/*/fan_mode | head -n 1)
TEMP=$(for h in /sys/class/hwmon/hwmon*; do
           [ "$(cat "$h/name")" = k10temp ] && echo "$h/temp1_input"; done | head -n 1)

WAS_ACTIVE=0
if systemctl is-active --quiet ideapad-fan-control.service 2>/dev/null; then
    WAS_ACTIVE=1
    systemctl stop ideapad-fan-control.service
fi

n=0
sample() { # phase count
    i=0
    while [ "$i" -lt "$2" ]; do
        n=$((n + 1))
        printf '%03d %s fan_mode=%s temp=%s ' "$n" "$1" "$(cat "$FAN")" "$(cat "$TEMP" 2>/dev/null || echo -)"
        od -An -v -tx1 "$EC" | tr -d '\n ' ; echo
        sleep 0.5
        i=$((i + 1))
    done
}

{
    echo 0 > "$FAN"; sleep 4
    sample auto1 16
    echo 2 > "$FAN"
    sample full 50
    echo 0 > "$FAN"
    sample auto2 24
} > "$OUT/ec/samples.txt"

dmesg | grep -iE 'ideapad|ACPI: EC|dytc|VPC' > "$OUT/dmesg.txt" || true
cat /sys/kernel/security/lockdown > "$OUT/lockdown.txt" 2>/dev/null || true

[ "$WAS_ACTIVE" = 1 ] && systemctl start ideapad-fan-control.service
[ -n "$OWNER" ] && chown -R "$OWNER" "$OUT"
echo "Collected into $OUT"
