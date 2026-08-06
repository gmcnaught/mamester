#!/bin/sh
# lever-ab.sh -- interleaved A/B of one perf lever over a game list.
#
#   lever-ab.sh <label> <binA> <envA> <binB> <envB> <games-file> [reps] [frames]
#
# Arm order alternates per (game, rep) so that cell order cannot masquerade as
# an effect: docs/bench-results-2003plus.md records order alone moving one arm
# by 2% on this device, against a 1.5-4% per-cell spread. Nothing under ~5% from
# a non-interleaved run means anything, which is why this script exists rather
# than two sequential loops.
#
# envA/envB are space-separated VAR=VAL strings, or "-" for none. The binary may
# be the same in both arms; that is how a pure env lever is measured.
#
# ROMPATH selects the romset directory and defaults to roms2003. lrmame is MAME
# 0.289 and cannot load the 0.78-era sets, so an lrmame run needs ROMPATH=roms289
# -- and the "$g NOROM" skip below is what stops a wrong ROMPATH from being
# reported as a run of zero games rather than as a mistake.
#
# Output: one TSV row per (game, rep) to /tmp/lev-<label>.tsv, and a per-game
# mean-of-reps summary at the end. fps comes from the MISTER-BENCH line, which
# counts retro_run() calls, not presented frames.
set -u

LABEL="$1"; BIN_A="$2"; ENV_A="$3"; BIN_B="$4"; ENV_B="$5"; GAMES="$6"
REPS="${7:-3}"; FRAMES="${8:-600}"
ROMPATH="${ROMPATH:-roms2003}"

# WORKDIR is where the two arm binaries live, and it is not always the engine
# directory: an A/B between two builds of the SAME engine needs both .so files
# side by side, and the deployed lrmame_libretro.so cannot be overwritten
# without invalidating every other binary that links it.
cd "${WORKDIR:-/media/fat/games/mame}" || exit 1
OUT=/tmp/lev-$LABEL.tsv
[ -f "$OUT" ] || printf "game\trep\ta_fps\tb_fps\n" > "$OUT"

run_one() {  # binary, env, game -> fps on stdout
    _bin="$1"; _env="$2"; _game="$3"
    killall -9 "$BIN_A" "$BIN_B" 2>/dev/null
    sleep 1
    if [ "$_env" = "-" ]; then
        _log=$(timeout -s KILL 180 ./"$_bin" "$_game" -rompath "$ROMPATH" -frames "$FRAMES" 2>&1)
    else
        _log=$(env $_env timeout -s KILL 180 ./"$_bin" "$_game" -rompath "$ROMPATH" -frames "$FRAMES" 2>&1)
    fi
    echo "$_log" | grep -o 'fps=[0-9.]*' | cut -d= -f2 | head -1
}

n=0
for g in $(cat "$GAMES"); do
    [ -f "$ROMPATH/$g.zip" ] || { echo "$g NOROM"; continue; }
    rep=1
    while [ "$rep" -le "$REPS" ]; do
        grep -q "^$g	$rep	" "$OUT" 2>/dev/null && { rep=$((rep+1)); continue; }
        n=$((n+1))
        if [ $((n % 2)) -eq 1 ]; then
            a=$(run_one "$BIN_A" "$ENV_A" "$g"); b=$(run_one "$BIN_B" "$ENV_B" "$g")
        else
            b=$(run_one "$BIN_B" "$ENV_B" "$g"); a=$(run_one "$BIN_A" "$ENV_A" "$g")
        fi
        printf "%s\t%s\t%s\t%s\n" "$g" "$rep" "${a:-FAIL}" "${b:-FAIL}" >> "$OUT"
        printf "%-12s rep%s  a=%-7s b=%-7s\n" "$g" "$rep" "${a:-FAIL}" "${b:-FAIL}"
        rep=$((rep+1))
    done
done

echo
echo "== $LABEL: per-game mean of $REPS reps =="
awk -F'\t' 'NR>1 && $3!="FAIL" && $4!="FAIL" {
        sa[$1]+=$3; sb[$1]+=$4; n[$1]++ }
     END {
        printf "%-12s %8s %8s %8s\n", "game", "A", "B", "B/A";
        for (g in n) {
            a=sa[g]/n[g]; b=sb[g]/n[g];
            printf "%-12s %8.1f %8.1f %+7.1f%%\n", g, a, b, 100*(b-a)/a;
            ta+=a; tb+=b; c++ }
        if (c) printf "%-12s %8.1f %8.1f %+7.1f%%\n", "MEAN", ta/c, tb/c, 100*(tb-ta)/ta
     }' "$OUT"
