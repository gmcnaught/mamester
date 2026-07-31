#!/bin/bash
#
# MAMESTer auto-launch handler (TEMPLATE — the port does not exist yet).
#
# Invoked by MiSTer's Master_Daemon (Frontier) when the MAMESTer FPGA core loads:
# the daemon watches /tmp/CORENAME and routes a loaded core by name (from the
# RBF's CONF_STR setname) to games/<setname>/_handler.sh. The directory name
# MUST equal the CONF_STR setname exactly — "MAMESTer" here matches the core's
# CONF_STR first token (fpga/MAME.sv). The emulator binary + ROMs live under the
# lowercase data path games/mame (GAMEDIR below), not here.
#
# WHY A HANDLER, NOT A `main=` WRAPPER: a `main=` binary REPLACES MiSTer's
# main() and inherits its obligations to the core — notably the scheduler's
# per-iteration `while (!is_fpga_ready(1)) fpga_wait_to_reset();` guard. The
# sibling ports measured the wrapper causing frame-1 wedges (maldita 3/5 vs
# stock-main 0/5). Stock MiSTer main keeps running and keeps its FPGA-readiness
# contract; the emulator is launched as an ordinary child. The daemon kills this
# child on a core change, so no supervise loop is needed.
#
# TRADEOFF: OSD Reset and a joystick-SHM bridge are NOT available on this path
# (they need the wrapper). Defer the wrapper unless/until it satisfies main()'s
# contract (readiness before spawn).

GAMEDIR="/media/fat/games/mame"          # emulator binary + roms/ live here
LOGDIR="/media/fat/logs/MAME"
MAME_BIN="${GAMEDIR}/mame"                # TODO: the mame4all-pi ELF (or 2003-Plus frontend)

cd "$GAMEDIR" || exit 1
mkdir -p "$LOGDIR"

# Singleton guard — reap a survivor before relaunch (two daemon instances each
# spawning a handler is a real, observed failure on the sibling ports). busybox
# has no pkill; use killall.
if ps w | grep -q "[m]ame"; then
    echo "mame handler: reaping a pre-existing mame before relaunch" >&2
    killall -9 mame 2>/dev/null
    sleep 1
fi

# Rotate the previous log.
mv -f "$LOGDIR/mame.log" "$LOGDIR/mame.prev.log" 2>/dev/null

# FPGA settle after core load. The sibling ports use 1–2 s; the fabric's first
# DDR access before the core is up can wedge the arbiter. Cheap insurance on a
# path that only runs at core load.
sleep 2

# Software render + no window: SDL provides input+audio only. mame4all-pi
# renders to its own bitmap and the present shim copies it to the DDR
# framebuffer (see tools/mame-frontend/).
export SDL_VIDEODRIVER=dummy

echo "mame handler: CORENAME='$(cat /tmp/CORENAME 2>/dev/null)' BIN='$MAME_BIN'" \
    > "$LOGDIR/mame.log"

# TODO: real invocation once the port exists, e.g.:
#   exec "$MAME_BIN" "$1" >> "$LOGDIR/mame.log" 2>&1
# where "$1" is the game the frontend selects. Until then, fail loudly rather
# than pretend to launch.
echo "mame handler: NOT IMPLEMENTED — no emulator binary yet" >> "$LOGDIR/mame.log"
exit 1
