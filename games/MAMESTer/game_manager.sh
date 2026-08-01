#!/bin/sh
# game_manager.sh — MAMESTer game lifecycle manager (Stage 7).
#
# The core loads with NO game running: MAME starts only once a romset is picked
# from the OSD ("Load Game"), the same idle-until-picked pattern as
# PICO-8/OpenBOR/PSX and the sibling solarus-mister quest_manager.sh.
#
# Main_MiSTer writes the pick to /media/fat/config/MAMESTer.s0 (the SC0 slot in
# fpga/MAME.sv's CONF_STR). A NEW pick is detected by mtime, not by content, so
# (a) a stale .s0 left from a previous session is never auto-launched and
# (b) re-picking the game that is already running is a no-op rather than a
# restart. Loop: launch on a pick, switch on a different pick, go idle if MAME
# exits on its own, tear down and exit when the user loads another core.
#
# TWO DIRECTORIES, and they are not interchangeable:
#   HOMEDIR  /media/fat/games/MAMESTer — must equal the CONF_STR setname, because
#            that is how Master_Daemon finds _handler.sh and where MiSTer's OSD
#            file browser opens. Holds this harness, and nothing else.
#   GAMEDIR  /media/fat/games/mame — the emulator and all of its data: the mame
#            binary, mame.cfg, roms/, opts/, and the cfg/nvram/hi/inp/snap state
#            directories. MAME chdir()s to its own binary's directory at startup
#            (src/rpi/rpi.cpp realpath(argv[0]) + chdir), so this is also the cwd
#            every relative path in mame.cfg resolves against.
#
# Env (production defaults):
#   GAMEDIR=/media/fat/games/mame       HOMEDIR=/media/fat/games/MAMESTer
#   GAME_LIB=$HOMEDIR/game_lib.sh       MAME_BIN=$GAMEDIR/mame
#   CORENAME_FILE=/tmp/CORENAME         EXPECT_CORE=MAMESTer
#   S0_FILE=/media/fat/config/MAMESTer.s0
#   LOGDIR=/media/fat/logs/MAMESTer     FATROOT=/media/fat   POLL_SEC=1
#   LAUNCH_CMD=  (unset -> run MAME; set -> "$LAUNCH_CMD <rom>", for tests)
set -u
GAMEDIR="${GAMEDIR:-/media/fat/games/mame}"
HOMEDIR="${HOMEDIR:-/media/fat/games/MAMESTer}"
GAME_LIB="${GAME_LIB:-$HOMEDIR/game_lib.sh}"
MAME_BIN="${MAME_BIN:-$GAMEDIR/mame}"
CORENAME_FILE="${CORENAME_FILE:-/tmp/CORENAME}"
EXPECT_CORE="${EXPECT_CORE:-MAMESTer}"
S0_FILE="${S0_FILE:-/media/fat/config/MAMESTer.s0}"
LOGDIR="${LOGDIR:-/media/fat/logs/MAMESTer}"
FATROOT="${FATROOT:-/media/fat}"
POLL_SEC="${POLL_SEC:-1}"

# shellcheck disable=SC1090  # dynamic path (env-overridable); defined in game_lib.sh
. "$GAME_LIB"   # provides resolve_rom, rom_setname, rom_path, game_opts

file_mtime() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null || echo 0; }

launch_game() {     # $1 = rom zip path; echoes the child pid
    if [ -n "${LAUNCH_CMD:-}" ]; then
        "$LAUNCH_CMD" "$1" >/dev/null 2>&1 &
        echo $!
        return 0
    fi

    _set=$(rom_setname "$1" "$HOMEDIR")
    _rompath=$(rom_path "$1" "$HOMEDIR")
    if [ -z "$_set" ] || [ -z "$_rompath" ]; then
        # An .mgl whose romset is missing, most likely.
        # stderr, not stdout: this function's stdout is captured as the pid.
        echo "MAMESTer manager: '$1' does not point at a readable romset — ignoring" >&2
        return 1
    fi
    _opts=$(game_opts "$GAMEDIR" "$_set" \
            | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    _log="$LOGDIR/$_set.log"

    mv -f "$_log" "$_log.prev" 2>/dev/null
    {
        echo "MAMESTer: launching '$_set'"
        echo "  pick    $1"
        echo "  rompath $_rompath"
        echo "  opts    ${_opts:-(none)}"
    } > "$_log"

    # -rompath is the directory the picked romset actually lives in, so a pick
    # from outside GAMEDIR still works. It has to be passed explicitly because
    # MAME's cwd is its own binary's directory, not wherever the zip was found.
    # (An absolute -rompath only works because of patch 0003 — mame4all's
    # frontend_help() used to rewrite any argv starting with '/' into an option.)
    #
    # No X and no GPU: SDL must take the dummy video driver. Frames reach the
    # screen through the DDR framebuffer our present backend writes
    # (tools/mame-frontend/mister-backend/mister_video.cpp), not through SDL.
    # Launched directly (no setsid) so $! IS MAME's pid — kill_game() and the
    # liveness poll both depend on that. stdin from /dev/null: Master_Daemon
    # spawns this whole chain with no terminal.
    # shellcheck disable=SC2086  # $_opts is a deliberate word-split flag list
    SDL_VIDEODRIVER=dummy "$MAME_BIN" "$_set" -rompath "$_rompath" $_opts \
        >> "$_log" 2>&1 </dev/null &
    echo $!
}

kill_game() {       # $1 = pid
    [ -n "$1" ] || return 0
    # MAME ignores SIGTERM (Stage 6 finding — `timeout -s KILL`/`killall -9` are
    # what actually stop it), so the grace window is a formality and SIGKILL is
    # the one that lands. Nothing is lost: mame4all flushes nvram/hiscores on its
    # own quit path, which a core change never reaches anyway.
    kill -TERM "$1" 2>/dev/null
    sleep 1
    kill -9 "$1" 2>/dev/null
}

game_pid=""
cleanup() { kill_game "$game_pid"; exit 0; }
trap cleanup TERM INT

mkdir -p "$LOGDIR" 2>/dev/null

# Baseline for "have I already seen this pick?". A .s0 left from a previous
# session must NOT auto-launch, but a pick made for THIS core load must, even
# though it can land before this loop starts: an MGL shortcut mounts its file
# on a delay measured from core load (`<file delay="2" .../>`), while the
# handler sleeps 2 s for the FPGA to settle before exec'ing this script. Anchor
# on /tmp/CORENAME's mtime, which Main_MiSTer stamps at core load: a .s0 newer
# than that belongs to this session and is treated as unseen; anything older is
# a leftover.
core_mtime=$(file_mtime "$CORENAME_FILE")
s0_mtime=$(file_mtime "$S0_FILE")
if [ "$s0_mtime" -gt "$core_mtime" ] 2>/dev/null; then
    last_mtime=""                      # pick belongs to this core load -> act on it
    echo "MAMESTer manager: a game was already selected for this core load"
else
    last_mtime="$s0_mtime"             # leftover from a previous session -> ignore
fi
loaded=""                              # rom currently loaded ("" = idle)

echo "MAMESTer manager: idle, waiting for an OSD game pick ($S0_FILE)"

while :; do
    sleep "$POLL_SEC"

    # Core changed away from MAMESTer: tear down and exit.
    core=$(tr -d '\000\r\n ' 2>/dev/null < "$CORENAME_FILE")
    if [ "$core" != "$EXPECT_CORE" ]; then
        echo "MAMESTer manager: core is now '$core' — stopping"
        kill_game "$game_pid"
        exit 0
    fi

    # MAME exited on its own (quit / crash): back to idle. Do NOT auto-restart —
    # a driver that dies during init would otherwise spin here forever.
    if [ -n "$game_pid" ] && ! kill -0 "$game_pid" 2>/dev/null; then
        echo "MAMESTer manager: '$loaded' exited — idle"
        game_pid=""
    fi

    # A new OSD pick = a fresh write to MAMESTer.s0 (mtime advanced).
    cur_mtime=$(file_mtime "$S0_FILE")
    [ "$cur_mtime" = "$last_mtime" ] && continue
    last_mtime="$cur_mtime"

    sel=$(resolve_rom "$S0_FILE" "$FATROOT" "$HOMEDIR")
    if [ -z "$sel" ]; then
        echo "MAMESTer manager: pick did not resolve to a readable .mgl or .zip — ignoring"
        continue
    fi

    # Re-picking the game that is already running: no-op.
    if [ "$sel" = "$loaded" ] && [ -n "$game_pid" ] && kill -0 "$game_pid" 2>/dev/null; then
        continue
    fi

    kill_game "$game_pid"
    game_pid=$(launch_game "$sel")
    if [ -z "$game_pid" ]; then
        loaded=""                      # nothing started; stay idle for the next pick
        continue
    fi
    loaded="$sel"
    echo "MAMESTer manager: started $(rom_setname "$sel" "$HOMEDIR") (pid $game_pid)"
done
