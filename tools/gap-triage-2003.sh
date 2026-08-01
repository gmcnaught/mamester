#!/bin/sh
# gap-triage-2003.sh -- fetch a 2003-plus romset and classify how the driver runs.
# Runs ON THE DEVICE. ROMs stay there; never commit them.
#
#   sh gap-triage-2003.sh arabianm gseeker ...
#   sh gap-triage-2003.sh -f list.txt
#
# Measures the SHIPPING configuration, because that is the only figure that
# decides anything: FPGA core loaded, DDR present live, sound chips emulated AND
# written to ALSA, ASM CPU cores on their curated per-driver default, unthrottled,
# 600 frames. A number taken with the present off or the sound off is measuring a
# machine we do not ship.
#
# THE FPS FIGURE ALONE IS NOT A PASS. klax runs at 126 fps while publishing a
# byte-identical frame forever -- full speed, blank screen. So every run also
# hashes the presented frame at 300 and 600 and compares: equal means the driver
# advanced its counter without drawing anything new. That check is the reason
# this script exists rather than a loop around the old one.
#
# Verdicts:
#   NOROM      the reference set has no such zip (or the fetch failed)
#   LOADFAIL   retro_load_game refused it -- wrong/missing ROMs for this driver
#   CRASH      died or produced no fps line within the timeout
#   STATIC     ran, but frame 300 and frame 600 are byte-identical: nothing moves
#   SLOW       below 60 fps: will not hold its native rate
#   MARGINAL   60-75 fps: holds now, no headroom for a busier scene
#   OK         above 75 fps
set -u

BASE="${BASE:-https://archive.org/download/mame-2003-plus-reference-set/roms}"
GAMEDIR="${GAMEDIR:-/media/fat/games/mame}"
ROMDIR="${ROMDIR:-roms2003}"
FRAMES="${FRAMES:-600}"
TIMEOUT="${TIMEOUT:-180}"
BIN="${BIN:-./mame2003}"
OUT="${OUT:-/tmp/triage-2003.tsv}"
KEEP="${KEEP:-0}"          # 1 = keep romsets; default deletes to spare the SD card

cd "$GAMEDIR" || exit 1
mkdir -p "$ROMDIR"

if [ "${1:-}" = "-f" ]; then
    [ -f "${2:-}" ] || { echo "no such list: ${2:-}" >&2; exit 1; }
    set -- $(cat "$2")
fi

[ -f "$OUT" ] || printf "game\tverdict\tfps\tgeometry\thz\tformat\tnote\n" > "$OUT"

for g in "$@"; do
    grep -q "^$g	" "$OUT" 2>/dev/null && continue     # resumable

    fetched=0
    if [ ! -f "$ROMDIR/$g.zip" ]; then
        # curl's TLS fails on this device; wget works.
        if wget -q -O "$ROMDIR/$g.zip.part" "$BASE/$g.zip" 2>/dev/null; then
            mv "$ROMDIR/$g.zip.part" "$ROMDIR/$g.zip"; fetched=1
        else
            rm -f "$ROMDIR/$g.zip.part"
            printf "%s\tNOROM\t\t\t\t\t\n" "$g" >> "$OUT"
            printf "%-12s NOROM\n" "$g"
            continue
        fi
    fi

    killall -9 mame2003 2>/dev/null; sleep 1
    MISTER_FRAME_HASH=$((FRAMES / 2)) timeout -s KILL "$TIMEOUT" \
        "$BIN" "$g" -rompath "$ROMDIR" -frames "$FRAMES" >/tmp/t2003.log 2>&1
    killall -9 mame2003 2>/dev/null

    fps=$(grep -o 'fps=[0-9.]*' /tmp/t2003.log | tail -1 | cut -d= -f2)
    geo=$(grep -o 'present [0-9]*x[0-9]*' /tmp/t2003.log | tail -1 | cut -d' ' -f2)
    hz=$(grep -oE '@ [0-9.]+ Hz' /tmp/t2003.log | head -1 | tr -dc '0-9.')
    fmt=$(grep -o 'pixel format [A-Z0-9]*' /tmp/t2003.log | tail -1 | awk '{print $3}')
    h1=$(grep -o 'hash=[0-9a-f]*' /tmp/t2003.log | head -1)
    h2=$(grep -o 'hash=[0-9a-f]*' /tmp/t2003.log | tail -1)
    note=""

    if grep -q 'retro_load_game failed' /tmp/t2003.log; then
        v=LOADFAIL
    elif [ -z "$fps" ]; then
        v=CRASH; note=$(tail -1 /tmp/t2003.log | cut -c1-60)
    elif [ -n "$h1" ] && [ "$h1" = "$h2" ]; then
        v=STATIC; note="$h1"
    else
        # POSIX sh has no floats; compare in tenths.
        t=$(echo "$fps" | awk '{printf "%d", $1*10}')
        if   [ "$t" -lt 600 ]; then v=SLOW
        elif [ "$t" -lt 750 ]; then v=MARGINAL
        else                        v=OK
        fi
    fi

    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$g" "$v" "$fps" "$geo" "$hz" "$fmt" "$note" >> "$OUT"
    printf "%-12s %-9s %-7s %-9s %s\n" "$g" "$v" "${fps:--}" "${geo:--}" "${fmt:--}"

    [ "$KEEP" = "1" ] || [ "$fetched" = "0" ] || rm -f "$ROMDIR/$g.zip"
done
