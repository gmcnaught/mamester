#!/bin/sh
# coverage_diff_test.sh -- host-side tests for tools/coverage-diff.py
#
# Fixture world: 2003-plus has taito_f3.c (1 parent + 1 clone), gng.c, wide.c and
# neogeo.c. mame4all has gng.cpp and wide.cpp. An MRA covers gng. So:
#   taito_f3.c -> NEW (2003-plus only, uncovered)
#   gng.c      -> covered by an MRA, excluded entirely
#   wide.c     -> uncovered but mame4all has it, so "improved", not a coverage win
#   neogeo.c   -> hand-excluded
#   wideone    -> 640 px wide, a geometry risk
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
FIX="$HERE/fixtures"
TOOL="$REPO/tools/coverage-diff.py"
JSON=/tmp/coverage-diff-test.json
pass=0
fail=0

check() { # check <label> <expected> <actual>
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1))
        printf "ok   %s\n" "$1"
    else
        fail=$((fail + 1))
        printf "FAIL %s\n  expected: %s\n  actual:   %s\n" "$1" "$2" "$3"
    fi
}

jq_count() { # jq_count <python-expression-over-d>
    python3 -c "import json;d=json.load(open('$JSON'));print($1)"
}

OUT=$(python3 "$TOOL" \
        --listinfo "$FIX/listinfo-sample.xml" \
        --mame4all "$FIX/mame4all-drivers-sample.txt" --mame4all-kind sourcefile \
        --mra      "$FIX/mra-setnames-sample.txt" \
        --json     "$JSON" 2>/dev/null) || {
    echo "FAIL coverage-diff.py exited non-zero"
    exit 1
}

check "taito_f3 reported as a new uncovered family" \
      "1" "$(jq_count "sum(1 for f in d['new_families'] if f['family']=='taito_f3.c')")"

check "clones do not inflate the parent count" \
      "1" "$(jq_count "[f['count'] for f in d['new_families'] if f['family']=='taito_f3.c'][0]")"

check "gng.c excluded entirely (an MRA covers it)" \
      "0" "$(jq_count "sum(1 for f in d['new_families']+d['improved'] if f['family']=='gng.c')")"

check "wide.c is 'improved', not a coverage win (mame4all has it)" \
      "1" "$(jq_count "sum(1 for f in d['improved'] if f['family']=='wide.c')")"

check "wide.c is not counted as new" \
      "0" "$(jq_count "sum(1 for f in d['new_families'] if f['family']=='wide.c')")"

check "neogeo hand-excluded from both lists" \
      "0" "$(jq_count "sum(1 for f in d['new_families']+d['improved'] if 'neogeo' in f['family'])")"

check "640-wide game flagged as a geometry risk" \
      "1" "$(jq_count "sum(1 for g in d['geometry_risk'] if g['name']=='wideone')")"

check "232-wide game not flagged" \
      "0" "$(jq_count "sum(1 for g in d['geometry_risk'] if g['name']=='ridleofb')")"

check "report names the new family" \
      "1" "$(printf '%s' "$OUT" | grep -c 'taito_f3.c')"

# .cpp vs .c normalisation is the whole reason wide.c/gng.c match mame4all at all;
# without it every one of mame4all's 414 families would read as a coverage win.
check "family match survives the .cpp/.c extension difference" \
      "1" "$(jq_count "sum(1 for f in d['improved'] if f['family']=='wide.c')")"

# The bare `ls src/drivers/` form must give the same answer as the two-column
# `mame \"*\" -sourcefile` form, since it is the one that needs no device.
python3 "$TOOL" \
    --listinfo "$FIX/listinfo-sample.xml" \
    --mame4all "$FIX/mame4all-families-sample.txt" --mame4all-kind families \
    --mra      "$FIX/mra-setnames-sample.txt" \
    --json     "$JSON.bare" >/dev/null 2>&1

check "bare family-listing input gives the same new-family set" \
      "$(python3 -c "import json;print(sorted(f['family'] for f in json.load(open('$JSON'))['new_families']))")" \
      "$(python3 -c "import json;print(sorted(f['family'] for f in json.load(open('$JSON.bare'))['new_families']))")"

# --- regression: driver files get renamed between MAME versions -------------
# mame4all calls the Midway Y-unit hardware wmsyunit.cpp; 2003-plus calls it
# midyunit.c. The setname `narc` is identical in both. Matching on filenames
# calls midyunit.c a coverage win, which is wrong -- Stage 8 benched mk and
# nbajam on that exact hardware under mame4all. Matching on setnames is right.
python3 "$TOOL" \
    --listinfo "$FIX/listinfo-sample.xml" \
    --mame4all "$FIX/mame4all-setnames-sample.txt" --mame4all-kind setnames \
    --mra      "$FIX/mra-setnames-sample.txt" \
    --json     "$JSON.sets" >/dev/null 2>&1

check "renamed family is NOT a coverage win when matching on setnames" \
      "0" "$(python3 -c "import json;d=json.load(open('$JSON.sets'));print(sum(1 for f in d['new_families'] if f['family']=='midyunit.c'))")"

check "renamed family lands in 'improved' instead" \
      "1" "$(python3 -c "import json;d=json.load(open('$JSON.sets'));print(sum(1 for f in d['improved'] if f['family']=='midyunit.c'))")"

check "filename matching gets the renamed family WRONG (documents why not to)" \
      "1" "$(jq_count "sum(1 for f in d['new_families'] if f['family']=='midyunit.c')")"

printf "\n%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
