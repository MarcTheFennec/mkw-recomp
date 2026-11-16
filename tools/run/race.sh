#!/usr/bin/env bash
# A race run: menu presses until the race, then run_perf.sh. The phase watch (MKW_SEQ_WATCH) finds the race's start
# (the first IsSequenceFlag after the cup movie) and goes off there (MKW_SEQ_UNTIL), so the race is measured without
# its cost; the run ends RACE_SECS (default 20) s later (MKW_SEQ_EXIT), with the MKW_HOST_TIME census of that window.
# SECONDS is only the safety limit.
# Usage: race.sh NAME [SECONDS]; extra env (MKW_*, MKW_DUMP...) passes through.
set -euo pipefail
cd "$(dirname "$0")/../.."
# Refuse a stale build/mkw: a runtime source newer than it means the last build did not run or failed (01/11/2026:
# three runs measured the previous binary after a commit hook stopped the build and a compile error went unseen).
if [ -n "$(find runtime -newer build/mkw \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) -print -quit)" ]; then
  echo "${0##*/}: runtime/ is newer than build/mkw: build first (tools/build/build.py)" >&2; exit 1
fi
# RACE_TT=1: a time trial instead of the Grand Prix (MKW_QUICK_RACE=tt, runtime/input/input.c): one kart and no
# rivals, at the grid 28-30 s after boot (tt2, tt3) instead of 52,6; measured from ReadyRace (no cup movie there).
if [ "${RACE_TT:-0}" = 1 ]; then
  # MKW_SEED fixes the course (the map's first cursor follows the clock: tt6a/b loaded the same world tiles)
  export MKW_QUICK_RACE=tt MKW_SEQ_UNTIL=${MKW_SEQ_UNTIL-ReadyRace} MKW_SEED=${MKW_SEED-1}
# RACE_GP=1: a Grand Prix by phases (MKW_QUICK_RACE=gp): 24 karts, the save's last cup; the kart drives with the
# save's Smart Steering and Auto-Accelerate, else A is held (into the walls). Measured from ReadyRace.
elif [ "${RACE_GP:-0}" = 1 ]; then
  export MKW_QUICK_RACE=gp MKW_SEQ_UNTIL=${MKW_SEQ_UNTIL-ReadyRace} MKW_SEED=${MKW_SEED-1}
else
  export MKW_INPUT_SCRIPT=${MKW_INPUT_SCRIPT:-tools/run/press-race.txt}
fi
export MKW_ROMFS_PATCHES=${MKW_ROMFS_PATCHES-build/romfs-res}
# The overlay above is the 720 lines of the 660M; MKW_RESOLUTION gives the game output the same lines (patches.c).
export MKW_RESOLUTION=${MKW_RESOLUTION:-720}
export MKW_SPARSE_MIB=${MKW_SPARSE_MIB:-768}
export MKW_SEQ_WATCH=${MKW_SEQ_WATCH:-1} MKW_SEQ_UNTIL=${MKW_SEQ_UNTIL--WaitIntroMovie,IsSequenceFlag} MKW_SEQ_EXIT=${MKW_SEQ_EXIT-${RACE_SECS:-20}}
# The world clock starts with the race: --seconds must not end the run before MKW_SEQ_EXIT (01/11/2026: RACE_SECS=48
# runs ended at 18-28 s on run_perf.sh's default 30).
# The way to the race at turbo speed with the cup movie skipped (core/hacks): the turbo goes off at MKW_SEQ_UNTIL's
# event, so the race itself runs at normal speed. MKW_TURBO=off MKW_SKIP_MOVIES=0 for the menus as the player sees them.
export MKW_TURBO=${MKW_TURBO:-until} MKW_SKIP_MOVIES=${MKW_SKIP_MOVIES:-1}
export WORLD_SECS=${WORLD_SECS:-$(( ${RACE_SECS:-20} + 30 ))}
export MKW_FLIGHT=${MKW_FLIGHT:-1} MKW_FLIGHT_SPIKE_MS=${MKW_FLIGHT_SPIKE_MS:-0} MKW_FLIGHT_AT=${MKW_FLIGHT_AT:-2000,2700,2900,3000}
exec tools/run/run_perf.sh "$1" "${2:-240}"
