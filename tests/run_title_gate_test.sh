#!/usr/bin/env bash
# Runs the title gate test under Wine. Needs a build with -DFH2MOD_TESTS=ON in build-test/:
#   tests/run_title_gate_test.sh
# Under Wine the gate stays out of it by design, so there only "alone" and "told" say anything; run natively (the
# test as azimuth.exe on Windows), all four do. FH2MOD_GATE_NATIVE=1 says the stand-in for wine is a native run.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
b=$root/build-test
cases="alone told"
[ -n "${FH2MOD_GATE_NATIVE:-}" ] && cases="other fh2 alone told"
status=0
for c in $cases; do
    dir=$(mktemp -d)
    mkdir -p "$dir/fh2-mods"
    cp "$b/title_gate_test.exe" "$dir/azimuth.exe"
    cp "$b/d3dcompiler_47.dll" "$dir/"
    cp "$b/GateMod.dll" "$dir/fh2-mods/"
    args=()
    [ "$c" = told ] && args=(--skip-launcher)
    echo "==== $c"
    (cd "$dir" && wine azimuth.exe "$c" "${args[@]}") || status=1
    rm -rf "$dir"
done
[ $status = 0 ] && echo "ALL PASSED"
exit $status
