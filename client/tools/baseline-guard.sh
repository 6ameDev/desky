#!/bin/sh
# Baseline no-growth guard: fails when either static-analysis baseline grows.
#
# Caps (recounted from the current tree; caps only ever decrease):
#   LINT_CAP=0     ("^ *<issue$" entries in androidApp/lint-baseline.xml;
#                   the "<issues" root tag is excluded by the anchor)
#   DETEKT_CAP=531 ("<ID>...</ID>" entries in config/detekt/baseline.xml;
#                   the "<SmellBaseline>" root tag carries no per-finding count)
#
# Triage: fix new findings (preferred); a genuinely legacy finding earns a cap
# bump only as its own reviewed change alongside the regenerated baseline.
# Never absorb silent growth.
#
# POSIX sh + grep only; no installs. Run: ./tools/baseline-guard.sh (from client/)
# or `make baseline-guard`.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CLIENT=$(CDPATH= cd -- "$HERE/.." && pwd)
LINT_BASELINE="$CLIENT/androidApp/lint-baseline.xml"
DETEKT_BASELINE="$CLIENT/config/detekt/baseline.xml"
LINT_CAP=0
DETEKT_CAP=531

fail=0
check() {
    name=$1; file=$2; pattern=$3; cap=$4
    if [ ! -f "$file" ]; then
        echo "baseline-guard: FAIL $name baseline missing: $file" >&2
        fail=1
        return
    fi
    count=$(grep -c "$pattern" "$file" || true)
    echo "baseline-guard: $name $count (cap $cap) $file"
    if [ "$count" -gt "$cap" ]; then
        echo "baseline-guard: FAIL $name baseline grew ($count > cap $cap)" >&2
        fail=1
    fi
}

check "lint" "$LINT_BASELINE" '^ *<issue$' "$LINT_CAP"
check "detekt" "$DETEKT_BASELINE" '<ID>.*</ID>' "$DETEKT_CAP"

if [ "$fail" -ne 0 ]; then
    echo "baseline-guard: FAILED — fix findings or bump the cap as a reviewed change" >&2
    exit 1
fi
echo "baseline-guard: OK"
