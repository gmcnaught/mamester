#!/bin/sh
# gap-triage.sh — fetch a 0.37b5 romset and classify how the driver runs.
#
# Runs ON THE DEVICE (scp it to /tmp and run there). For each setname: fetch the
# romset if absent, run it unthrottled with sound for a fixed frame count, and
# print one line of verdict.
#
#   sh gap-triage.sh nbajam paperboy klax ...
#
# ROMs are pulled from the MAME 0.37b5 collection and stay on the device — never
# commit them (pre-2016 MAME licence: no ROM bundling). The local romset that
# ships with a MiSTer install is a MODERN set: it fails to load on many 0.37b5
# drivers ("required files are missing") because files were renamed or split,
# which is why triage pulls a matching-version set instead.
#
# The fps is unthrottled with sound and with the FPGA core loaded — i.e. the
# production configuration. Divide by 60 for the real-time multiple; anything
# under ~70 will not hold 60 Hz once the throttle and OSD are in play.
#
# Prerequisite on the device: /tmp/drivers.txt, the setnames this build knows —
#   cd /media/fat/games/mame && ./mame "*" -sourcefile | awk '{print $1}' | sort > /tmp/drivers.txt
# (a wrong setname otherwise looks identical to a missing driver; zaccaria and
# kingobox are compiled in as monymony/jackrabt and kingofb).

BASE="https://archive.org/download/MAME0.37b5RomCollectionByGhostware"
GAMEDIR="${GAMEDIR:-/media/fat/games/mame}"
FRAMES="${FRAMES:-600}"
TIMEOUT="${TIMEOUT:-60}"

cd "$GAMEDIR" || exit 1
mkdir -p roms

for g in "$@"; do
    if [ -f /tmp/drivers.txt ] && ! grep -qx "$g" /tmp/drivers.txt; then
        printf "%-10s NOT A DRIVER in this build\n" "$g"; continue
    fi
    if [ ! -f "roms/$g.zip" ]; then
        # curl's TLS fails on this device; wget works.
        wget -q -O "roms/$g.zip.part" "$BASE/$g.zip" \
            && mv "roms/$g.zip.part" "roms/$g.zip" \
            || { rm -f "roms/$g.zip.part"; printf "%-10s DOWNLOAD FAILED\n" "$g"; continue; }
    fi

    killall -9 mame 2>/dev/null; sleep 1      # MAME ignores SIGTERM
    SDL_VIDEODRIVER=dummy MISTER_BENCH_FRAMES="$FRAMES" timeout -s KILL "$TIMEOUT" \
        ./mame "$g" -rompath roms -nothrottle >/tmp/g.log 2>&1
    rc=$?
    fps=$(grep MISTER-BENCH /tmp/g.log | tail -1 | sed 's/.*fps=\([0-9.]*\).*/\1/')

    if   [ -n "$fps" ];                      then verdict="OK fps=$fps"
    elif grep -q "NOT FOUND" /tmp/g.log;     then verdict="ROMSET (missing files)"
    elif grep -q "not supported" /tmp/g.log; then verdict="NO DRIVER"
    elif [ "$rc" = 137 ];                    then verdict="HANG (killed at ${TIMEOUT}s)"
    else verdict="EXIT rc=$rc: $(grep -v '^$' /tmp/g.log | tail -1 | cut -c1-70)"
    fi
    printf "%-10s %s\n" "$g" "$verdict"
done
killall -9 mame 2>/dev/null
