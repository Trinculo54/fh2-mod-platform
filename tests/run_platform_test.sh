#!/usr/bin/env bash
# Runs the platform test under Wine. Needs a build with -DFH2MOD_TESTS=ON in
# build-test/ and the game's EN.zip (and optionally the exe) from your install:
#   tests/run_platform_test.sh <EN.zip> [forza_durango_release_final.exe]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
b=$root/build-test
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/fh2-mods" "$dir/FH2/Mount/media/Stripped/StringTables"
cp "$b/platform_test.exe" "$dir/azimuth.exe"
cp "$b/d3dcompiler_47.dll" "$dir/"
cp "$b/HelloMod.dll" "$b/TestMod.dll" "$b/Legacy.dll" "$dir/fh2-mods/"
cp "$1" "$dir/FH2/Mount/media/Stripped/StringTables/EN.zip"
exe=()
[ -n "${2:-}" ] && exe=("$(winepath -w "$2" 2>/dev/null || echo "Z:${2//\//\\}")")
cd "$dir"
wine azimuth.exe full "${exe[@]}"
rm fh2-mods/TestMod.dll
wine azimuth.exe removed
