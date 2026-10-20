#!/usr/bin/env bash
# Runs the platform test under Wine. Needs a build with -DFH2MOD_TESTS=ON in
# build-test/ and files from your own install (EN.zip from
# media\Stripped\StringTables and media\Stripped\gamedbRC.slt). SQLite is built
# into the platform and the test, so a fresh Wine prefix is all it needs:
#   tests/run_platform_test.sh <EN.zip> <gamedbRC.slt> [forza_durango_release_final.exe]
# With FH2_MEDIA set to your install's media folder, its UI.zip and stateflow.zip
# are copied in too, and the menus the platform writes (the MODS tab, pages) are
# checked.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
b=$root/build-test
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/fh2-mods" "$dir/FH2/Mount/media/Stripped/StringTables" "$dir/FH2/Mount/mediaoverride/UI"
cp "$b/platform_test.exe" "$dir/azimuth.exe"
cp "$b/d3dcompiler_47.dll" "$dir/"
cp "$b/AaaMod.dll" "$b/HelloMod.dll" "$b/TestMod.dll" "$b/Legacy.dll" "$dir/fh2-mods/"
cp "$1" "$dir/FH2/Mount/media/Stripped/StringTables/EN.zip"
cp "$2" "$dir/FH2/Mount/media/Stripped/gamedbRC.slt"
chmod u+w "$dir/FH2/Mount/media/Stripped/gamedbRC.slt"
printf 'made by hand' > "$dir/FH2/Mount/mediaoverride/UI/handmade.xml"
if [ -n "${FH2_MEDIA:-}" ]; then
    cp "$FH2_MEDIA/UI.zip" "$FH2_MEDIA/stateflow.zip" "$dir/FH2/Mount/media/"
fi
exe=()
[ -n "${3:-}" ] && exe=("$(winepath -w "$3" 2>/dev/null || echo "Z:${3//\//\\}")")
cd "$dir"
wine azimuth.exe full "${exe[@]}"
rm fh2-mods/TestMod.dll
wine azimuth.exe removed
wine azimuth.exe crash
