#!/bin/sh
# game_manager_test.sh — host-side tests for the MAMESTer launch path.
#
# Covers the selection logic in games/MAMESTer/game_lib.sh and the lifecycle
# loop in game_manager.sh, using env overrides (HOMEDIR, GAMEDIR, S0_FILE,
# CORENAME_FILE, LAUNCH_CMD, POLL_SEC) so nothing touches a device. Runs on
# macOS and on the MiSTer itself.
#
# LAUNCH_CMD replaces the real MAME invocation, so launch_game()'s argv
# (-rompath, per-game opts) is only exercised on hardware.
#
#   sh tests/game_manager_test.sh
set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
LIB="$REPO/games/MAMESTer/game_lib.sh"
MGR="$REPO/games/MAMESTer/game_manager.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n     want: %s\n     got:  %s\n' "$1" "$2" "$3"; }
is()   { [ "$2" = "$3" ] && ok "$1" || bad "$1" "$2" "$3"; }

# shellcheck disable=SC1090
. "$LIB"

echo "resolve_rom"

# The device layout: romsets live with the emulator in games/mame, and the OSD
# browser's home games/MAMESTer reaches them through a roms symlink.
FAT="$TMP/fat"
HOME_DIR="$FAT/games/MAMESTer"
ROMS="$FAT/games/mame/roms"
mkdir -p "$ROMS" "$HOME_DIR"
ln -s "$ROMS" "$HOME_DIR/roms"
: > "$ROMS/gng.zip"
: > "$ROMS/1943.zip"
S0="$TMP/MAMESTer.s0"

printf 'games/mame/roms/gng.zip' > "$S0"
is "fatroot-relative pick" "$FAT/games/mame/roms/gng.zip" "$(resolve_rom "$S0" "$FAT" "$HOME_DIR")"

printf '%s' "$ROMS/gng.zip" > "$S0"
is "absolute pick" "$ROMS/gng.zip" "$(resolve_rom "$S0" "$FAT" "$HOME_DIR")"

# An MGL mount goes through Main_MiSTer's home-dir join, so the stored path is
# relative to games/<setname>/ rather than to /media/fat (device-observed).
printf 'roms/gng.zip' > "$S0"
is "home-relative pick (MGL mount)" "$HOME_DIR/roms/gng.zip" "$(resolve_rom "$S0" "$FAT" "$HOME_DIR")"

# Main_MiSTer does not truncate the config file, so a shorter pick written over
# a longer one leaves the tail of the previous path behind.
printf 'games/mame/roms/1943.zipame/roms/gng.zip' > "$S0"
is "trailing junk from a longer previous pick" \
   "$FAT/games/mame/roms/1943.zip" "$(resolve_rom "$S0" "$FAT" "$HOME_DIR")"

printf 'games/mame/roms/gng.zip\r\n' > "$S0"
is "CR/LF tolerated" "$FAT/games/mame/roms/gng.zip" "$(resolve_rom "$S0" "$FAT" "$HOME_DIR")"

printf 'games/mame/roms/nosuch.zip' > "$S0"
is "pick that is not on disk resolves to nothing" "" "$(resolve_rom "$S0" "$FAT" "$HOME_DIR")"

: > "$S0"
is "empty .s0 resolves to nothing" "" "$(resolve_rom "$S0" "$FAT" "$HOME_DIR")"

is "missing .s0 resolves to nothing" "" "$(resolve_rom "$TMP/absent.s0" "$FAT" "$HOME_DIR")"

echo "rom_setname"
is "basename minus extension" "gng"  "$(rom_setname /media/fat/games/MAMESTer/roms/gng.zip)"
is "uppercase extension"      "GnG"  "$(rom_setname /roms/GnG.ZIP)"
is "dots in the name"         "mk.2" "$(rom_setname /roms/mk.2.zip)"

echo "game_opts"
G="$TMP/gamedir"; mkdir -p "$G/opts"
is "no opts files" "" "$(game_opts "$G" "gng")"
printf -- '-norotate\n' > "$G/opts/default.opt"
is "falls back to default.opt" "-norotate " "$(game_opts "$G" "gng")"
printf -- '# real cabinet signal\n-ror\n' > "$G/opts/gng.opt"
is "per-game file wins, comments stripped" " -ror " "$(game_opts "$G" "gng")"

echo "game_manager lifecycle"

# A stand-in for MAME: records its argument and sleeps until killed.
LAUNCHER="$TMP/fake_mame.sh"
cat > "$LAUNCHER" <<'EOF'
#!/bin/sh
echo "$1" >> "$LAUNCHED"
while :; do sleep 1; done
EOF
chmod +x "$LAUNCHER"

CORE="$TMP/CORENAME"
LAUNCHED="$TMP/launched"
: > "$LAUNCHED"
printf 'MAMESTer' > "$CORE"
: > "$S0"                                    # stale, pre-existing pick

run_manager() {
    GAMEDIR="$G" HOMEDIR="$HOME_DIR" GAME_LIB="$LIB" CORENAME_FILE="$CORE" \
    EXPECT_CORE=MAMESTer S0_FILE="$S0" LOGDIR="$TMP/logs" FATROOT="$FAT" \
    POLL_SEC=0.2 LAUNCH_CMD="$LAUNCHER" LAUNCHED="$LAUNCHED" \
    sh "$MGR" > "$TMP/mgr.log" 2>&1 &
    MGR_PID=$!
}

# A .s0 OLDER than the core load is a leftover session and must NOT auto-launch.
printf 'games/mame/roms/gng.zip' > "$S0"
sleep 1
printf 'MAMESTer' > "$CORE"                  # core loaded after the pick was written
run_manager
sleep 1
is "leftover .s0 from a previous session does not auto-launch" \
   "0" "$(wc -l < "$LAUNCHED" | tr -d ' ')"

# A fresh pick does launch. Rewriting the file bumps its mtime.
sleep 1                                      # mtime has 1-second granularity
printf 'games/mame/roms/gng.zip' > "$S0"
sleep 1.5
is "fresh pick launches" "$FAT/games/mame/roms/gng.zip" "$(tail -1 "$LAUNCHED")"
is "launched exactly once" "1" "$(wc -l < "$LAUNCHED" | tr -d ' ')"

# Re-picking the same game while it runs is a no-op, not a restart.
sleep 1
printf 'games/mame/roms/gng.zip' > "$S0"
sleep 1.5
is "re-picking the running game does not relaunch" "1" "$(wc -l < "$LAUNCHED" | tr -d ' ')"

# A different pick switches.
sleep 1
printf 'games/mame/roms/1943.zip' > "$S0"
sleep 2.5
is "different pick switches" "$FAT/games/mame/roms/1943.zip" "$(tail -1 "$LAUNCHED")"
is "switch is the second launch" "2" "$(wc -l < "$LAUNCHED" | tr -d ' ')"

# Loading another core stops the manager (and its child).
printf 'Genesis' > "$CORE"
sleep 2.5
if kill -0 "$MGR_PID" 2>/dev/null; then
    bad "core change exits the manager" "exited" "still running"
    kill -9 "$MGR_PID" 2>/dev/null
else
    ok "core change exits the manager"
fi
if pgrep -f "$LAUNCHER" > /dev/null 2>&1; then
    bad "core change kills the game" "no fake_mame" "fake_mame still running"
    pkill -9 -f "$LAUNCHER" 2>/dev/null
else
    ok "core change kills the game"
fi

# A pick that lands BEFORE the manager starts — an MGL shortcut mounts its file
# on a delay measured from core load, which can beat the handler's FPGA-settle
# sleep. Newer than /tmp/CORENAME means it belongs to this core load.
: > "$LAUNCHED"
printf 'MAMESTer' > "$CORE"
sleep 1
printf 'games/mame/roms/1943.zip' > "$S0"    # pick is newer than the core load
run_manager
sleep 1.5
is "pick made before the manager starts still launches" \
   "$FAT/games/mame/roms/1943.zip" "$(tail -1 "$LAUNCHED")"
kill -9 "$MGR_PID" 2>/dev/null
wait "$MGR_PID" 2>/dev/null            # reap, so the shell does not report "Killed"
pkill -9 -f "$LAUNCHER" 2>/dev/null

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
