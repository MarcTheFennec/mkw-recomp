#!/usr/bin/env bash
# Follows a game run's log and prints its lines that match PATTERN (plus FATAL and guard stops), then exits when the
# run ends: the Monitor of a run, without a `tail -F` left behind (09/11/2026: two monitors kept their tail after the
# run ended and the hooks had to name the shells to kill).
# Usage: tools/run/watch.sh NAME [PATTERN]   (log build/run/NAME.log; the run is its job-NAME.service or mkw)
# Without PATTERN only failures and the end come through: every printed line wakes the agent for a whole turn that
# rereads its context, and the old default (every `seq: enter`) gave 4 to 43 events per run (09-16/11/2026, 13 % of
# the week's tokens). Ask only for the line that changes the plan; phases are read from the log afterwards.
set -euo pipefail
cd "$(dirname "$0")/../.."
n=${1:?usage: tools/run/watch.sh NAME [PATTERN]}
pat="FATAL|GPU_GUARD:.*destroyed|device fault${2:+|$2}"
log=build/run/$n.log
for _ in $(seq 120); do [ -f "$log" ] && break; sleep 1; done
[ -f "$log" ] || { echo "watch: no $log after 2 min"; exit 1; }
alive() { systemctl --user is-active -q "job-$n.service" 2>/dev/null || pgrep -x mkw >/dev/null; }
pos=0
while :; do
    size=$(stat -c %s "$log")
    if [ "$size" -gt "$pos" ]; then
        tail -c +$((pos + 1)) "$log" | head -c $((size - pos)) | grep -E --line-buffered "$pat" || true
        pos=$size
    fi
    alive || { sleep 2; tail -c +$((pos + 1)) "$log" | grep -E "$pat" || true; echo "watch: $n ended"; exit 0; }
    sleep 2
done
