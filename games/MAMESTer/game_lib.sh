# shellcheck shell=sh
# game_lib.sh — shared MAMESTer game-selection helpers.
#
# Sourced by game_manager.sh (and by tests/game_manager_test.sh) so the
# OSD-selection logic has exactly one implementation. No side effects on source.
#
# TWO kinds of pick, because the OSD browser cannot select a romset zip: it
# fakes every .zip into a directory and descends into it (Main_MiSTer
# file_io.cpp ScanDirectory, suppressed only by SCANO_NOZIP, which a core cannot
# request). So the CONF_STR slot is "SC0,MGLZIP" and:
#
#   .mgl   a MiSTer shortcut file, one per game, written by
#          tools/make-shortcuts.py. Selecting one from MiSTer's MAIN MENU loads
#          this core and mounts the romset named in its XML; selecting the same
#          file from THIS core's picker mounts the .mgl itself, and the romset
#          path is read out of the XML here. One set of files, both entry points.
#   .zip   a romset mounted directly — what the main-menu path produces, and what
#          you get if you descend into a romset and pick a member (the stored
#          path is cut at ".zip", so any member resolves to the romset itself).

# _cut_at_ext <text> <ext>
#   Shortest prefix of <text> ending in .<ext>, in either case, or nothing.
#   `%%` strips the LONGEST matching suffix, so it cuts at the first occurrence.
_cut_at_ext() {
    _t="$1"; _e="$2"
    _lo=$(printf '%s' "$_e" | tr 'A-Z' 'a-z')
    _hi=$(printf '%s' "$_e" | tr 'a-z' 'A-Z')
    _a="${_t%%.$_lo*}"; [ "$_a" = "$_t" ] && _a=""
    _b="${_t%%.$_hi*}"; [ "$_b" = "$_t" ] && _b=""
    if   [ -z "$_a" ] && [ -z "$_b" ]; then return 0
    elif [ -z "$_b" ]; then printf '%s.%s' "$_a" "$_lo"
    elif [ -z "$_a" ]; then printf '%s.%s' "$_b" "$_hi"
    elif [ ${#_a} -lt ${#_b} ]; then printf '%s.%s' "$_a" "$_lo"
    else printf '%s.%s' "$_b" "$_hi"
    fi
}

# resolve_rom <s0_file> [fatroot] [homedir]
#   Reads the OSD selection Main_MiSTer wrote for the SC0 slot and echoes the
#   resolved absolute path of the picked .mgl or .zip, or nothing if there is no
#   valid selection.
#
#   The stored path is NOT truncated when a longer pick is overwritten by a
#   shorter one, so bytes of a previous pick can trail the real path — hence the
#   cut at the first extension match. (Same shape as solarus-mister's
#   resolve_quest, which cuts at ".sol".)
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

    # Whichever extension appears first wins: a .mgl sitting in a directory
    # whose own name contains ".zip" must still cut at ".mgl".
    _sel=""
    for _ext in mgl zip; do
        _try=$(_cut_at_ext "$_raw" "$_ext")
        [ -n "$_try" ] || continue
        [ "$_try" = ".$_ext" ] && continue                      # empty basename
        if [ -z "$_sel" ] || [ ${#_try} -lt ${#_sel} ]; then _sel="$_try"; fi
    done
    [ -n "$_sel" ] || return 0

    _sel=$(printf '%s' "$_sel" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    [ -n "$_sel" ] || return 0

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

# mgl_romset <mgl_file> [homedir]
#   The romset an .mgl points at, as an absolute path, or nothing.
#
#   The XML is
#       <file delay="2" type="s" index="0" path="roms/gng.zip"/>
#   and Main_MiSTer resolves that path against the core's home directory, so we
#   do the same. Absolute and /media/fat-relative forms are accepted too, since
#   a hand-written .mgl may use either.
mgl_romset() {
    _f="$1"
    _h="${2:-${HOMEDIR:-/media/fat/games/MAMESTer}}"
    _p=$(tr -d '\r\000' < "$_f" 2>/dev/null \
         | sed -n 's/.*<file[^>]*path="\([^"]*\)".*/\1/p' | head -1)
    [ -n "$_p" ] || return 0
    case "$_p" in
        # Only an absolute path is taken as written. A relative one is NOT
        # resolved against the cwd — the manager's cwd is the home directory
        # only by accident of how the handler starts it, and MAME then chdir()s
        # somewhere else again, so a relative -rompath would be read against the
        # wrong directory. Home-relative is what Main_MiSTer itself does.
        /*) [ -f "$_p" ] && printf '%s\n' "$_p"; return 0 ;;
    esac
    for _c in "$_h/$_p" "${FATROOT:-/media/fat}/$_p"; do
        if [ -f "$_c" ]; then printf '%s\n' "$_c"; return 0; fi
    done
    return 0
}

# rom_setname <pick_path> [homedir]
#   MAME's driver name: the romset zip's basename, following an .mgl to the
#   romset it names. Driver lookup is case-insensitive (mame4all
#   src/rpi/rpi.cpp:259 strcasecmp) and the zip must be opened under its real
#   filename, so nothing is case-folded here.
rom_setname() {
    _z="$1"
    case "$1" in
        *.mgl|*.MGL) _z=$(mgl_romset "$1" "${2:-}") ;;
    esac
    [ -n "$_z" ] || return 0
    _b=$(basename "$_z")
    _b="${_b%.zip}"
    printf '%s\n' "${_b%.ZIP}"
}

# rom_path <pick_path> [homedir]
#   Directory to hand MAME as -rompath: wherever the romset actually lives.
rom_path() {
    _z="$1"
    case "$1" in
        *.mgl|*.MGL) _z=$(mgl_romset "$1" "${2:-}") ;;
    esac
    [ -n "$_z" ] || return 0
    dirname "$_z"
}

# game_optfile <gamedir> <setname>
#   The per-game options file: opts/<setname>.opt if present, else
#   opts/default.opt, else nothing. Factored out so game_opts and game_engine
#   cannot disagree about which file a game's settings come from.
game_optfile() {
    _dir="$1/opts"
    _f="$_dir/$2.opt"
    [ -f "$_f" ] || _f="$_dir/default.opt"
    [ -f "$_f" ] || return 0
    echo "$_f"
}

# game_opts <gamedir> <setname>
#   Per-game launch flags. mame4all takes orientation and speed switches on the
#   command line (-norotate / -ror / -rol, -frameskip N, -nosound), which is how
#   a portrait driver gets presented as its real cabinet signal — see
#   docs/superpowers/progress.md Stage 3. Echoes opts/<setname>.opt if present,
#   else opts/default.opt, else nothing. '#' starts a comment.
#
#   An `engine <name>` line is a directive for game_engine below, NOT a flag, so
#   it is deleted here. Every other non-comment token goes to the emulator
#   verbatim, and passing `engine lrmame` to MAME would be an unknown-option
#   error rather than something ignorable.
game_opts() {
    _f=$(game_optfile "$1" "$2")
    [ -n "$_f" ] || return 0
    # Two delete expressions rather than one alternation: `\|` is a GNU sed
    # extension and these tests also run on macOS.
    sed -e 's/#.*$//' "$_f" \
        | sed -e '/^[[:space:]]*engine[[:space:]]/d' \
              -e '/^[[:space:]]*engine[[:space:]]*$/d' \
        | tr '\n' ' '
}

# game_engine <gamedir> <setname>
#   Which emulator this driver should launch under, from an `engine <name>` line
#   in the same options file. Echoes the name, or nothing for "the default".
#
#   The port ships three engines and the right one is per-driver: mame4all-pi is
#   fastest where it works at all, 2003-plus covers families mame4all lacks, and
#   lrmame (current MAME) covers what neither has. That choice cannot be made at
#   deploy time — it is a property of the driver — so it lives beside the
#   driver's other launch settings.
#
#   Last line wins, so a `default.opt` engine can be overridden per game.
game_engine() {
    _f=$(game_optfile "$1" "$2")
    [ -n "$_f" ] || return 0
    sed -e 's/#.*$//' "$_f" \
        | sed -n -e 's/^[[:space:]]*engine[[:space:]]\{1,\}\([^[:space:]]\{1,\}\).*$/\1/p' \
        | tail -1
}
