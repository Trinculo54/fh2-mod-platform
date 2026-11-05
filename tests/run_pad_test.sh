#!/usr/bin/env bash
# Runs the pad layer preview's tests: its rules (pad_engine_test), then the hook against the real
# game exe under Wine (pad_hook_test, with PadMod.dll). Needs a build with -DFH2MOD_TESTS=ON in
# build-test/:
#   tests/run_pad_test.sh <forza_durango_release_final.exe>
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
b=$root/build-test
exe=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
wine "$b/pad_engine_test.exe"
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/fh2-mods" "$dir/FH2/Mount/media/Stripped/StringTables"
cp "$b/pad_hook_test.exe" "$dir/azimuth.exe"
cp "$b/d3dcompiler_47.dll" "$dir/"
cp "$b/PadMod.dll" "$dir/fh2-mods/"
ln -s "$exe" "$dir/FH2/Mount/forza_durango_release_final.exe"
cd "$dir"
wine azimuth.exe "$(winepath -w "$exe" 2>/dev/null || echo "Z:${exe//\//\}")"
