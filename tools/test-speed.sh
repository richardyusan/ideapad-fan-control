#!/bin/sh
# Runs the manual-speed loop on the real fan at several percentages and logs
# the measured speed. Run as root:
#   sudo tools/test-speed.sh <FanControl binary> <log file> [percent...]
# Leaves the fan in automatic control and the saved setting untouched.
set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
BIN=${1:?binary}
LOG=${2:?log file}
shift 2
[ $# -gt 0 ] || set -- 30 50 60 80 100

CONF=/etc/ideapad-fan-control.conf
RUN=/run/ideapad-fan-control
SAVED=$(cat "$CONF" 2>/dev/null || true)
WAS_ACTIVE=0
if systemctl is-active --quiet ideapad-fan-control.service 2>/dev/null; then
    WAS_ACTIVE=1
    systemctl stop ideapad-fan-control.service
fi

: > "$LOG"
for p in "$@"; do
    echo "$p" > "$CONF"
    "$BIN" --daemon 2>>"$LOG" &
    pid=$!
    printf '%s%%:' "$p" >> "$LOG"
    i=0
    while [ "$i" -lt 24 ]; do
        sleep 0.5
        printf ' %s' "$(cut -d' ' -f3 "$RUN" 2>/dev/null || echo x)" >> "$LOG"
        i=$((i + 1))
    done
    echo >> "$LOG"
    kill "$pid"
    wait "$pid" 2>/dev/null || true
    sleep 3
done

if [ -n "$SAVED" ]; then echo "$SAVED" > "$CONF"; else rm -f "$CONF"; fi
[ "$WAS_ACTIVE" = 1 ] && systemctl start ideapad-fan-control.service
chmod 644 "$LOG"
echo done
