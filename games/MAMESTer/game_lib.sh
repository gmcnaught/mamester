# shellcheck shell=sh
# game_lib.sh — shared MAMESTer game-selection helpers.
#
# Sourced by game_manager.sh (and by tests/game_manager_test.sh) so the
# OSD-selection logic has exactly one implementation. No side effects on source.

# resolve_rom <s0_file> [fatroot] [gamedir]
#   Reads the OSD selection Main_MiSTer wrote for the "SC0,ZIP,Load Game" slot
#   and echoes the resolved absolute .zip path, or nothing if there is no valid
#   selection.
#
#   The stored path is NOT truncated when a longer pick is overwritten by a
#   shorter one, so bytes of a previous pick can trail the real path. Cut at the
#   FIRST ".zip" — the browser only offers the CONF_STR extension, so the first
#   match ends the selection. (Same shape as solarus-mister's resolve_quest,
#   which cuts at ".sol".)
#
#   What the path is relative TO depends on how the mount happened, so try all
#   three forms. An OSD browse writes it relative to the MiSTer root
#   (/media/fat), which is what the sibling cores assume; an MGL shortcut goes
#   through Main_MiSTer's home-dir join and lands relative to the core's own
#   games/<setname>/ directory — device-observed, an MGL naming
#   "games/MAMESTer/roms/gng.zip" stored "games/MAMESTer/games/MAMESTer/roms/gng.zip".
resolve_rom() {
    _s0="$1"
    _fat="${2:-${FATROOT:-/media/fat}}"
    _home="${3:-${HOMEDIR:-/media/fat/games/MAMESTer}}"
    [ -f "$_s0" ] || return 0

    _raw=$(tr -d '\r\000' < "$_s0")

    # Shortest prefix ending in the extension, in either case. `%%` strips the
    # longest matching suffix, so it cuts at the first occurrence.
    _lc="${_raw%%.zip*}"; [ "$_lc" = "$_raw" ] && _lc=""
    _uc="${_raw%%.ZIP*}"; [ "$_uc" = "$_raw" ] && _uc=""
    if   [ -z "$_lc" ] && [ -z "$_uc" ]; then return 0
    elif [ -z "$_uc" ]; then _sel="$_lc.zip"
    elif [ -z "$_lc" ]; then _sel="$_uc.ZIP"
    elif [ ${#_lc} -lt ${#_uc} ]; then _sel="$_lc.zip"
    else _sel="$_uc.ZIP"
    fi

    _sel=$(printf '%s' "$_sel" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    if [ "$_sel" = ".zip" ] || [ "$_sel" = ".ZIP" ]; then return 0; fi

    if [ -f "$_sel" ]; then
        printf '%s\n' "$_sel"
        return 0
    fi
    case "$_sel" in
        /*) return 0 ;;                              # absolute, already tried
    esac
    if [ -f "$_fat/$_sel" ]; then
        printf '%s\n' "$_fat/$_sel"
    elif [ -f "$_home/$_sel" ]; then
        printf '%s\n' "$_home/$_sel"
    fi
    return 0
}

# rom_setname <rom_path>
#   MAME's driver name is the romset zip's basename. Driver lookup is
#   case-insensitive (mame4all src/rpi/rpi.cpp:259 strcasecmp), and the zip must
#   be opened under its real filename, so the name is passed through as-is.
rom_setname() {
    _b=$(basename "$1")
    _b="${_b%.zip}"
    printf '%s\n' "${_b%.ZIP}"
}

# game_opts <gamedir> <setname>
#   Per-game launch flags. mame4all takes orientation and speed switches on the
#   command line (-norotate / -ror / -rol, -frameskip N, -nosound), which is how
#   a portrait driver gets presented as its real cabinet signal — see
#   docs/superpowers/progress.md Stage 3. Echoes opts/<setname>.opt if present,
#   else opts/default.opt, else nothing. '#' starts a comment.
game_opts() {
    _dir="$1/opts"
    _f="$_dir/$2.opt"
    [ -f "$_f" ] || _f="$_dir/default.opt"
    [ -f "$_f" ] || return 0
    sed -e 's/#.*$//' "$_f" | tr '\n' ' '
}
