#!/usr/bin/env bash
# Runs the platform test under Wine. Needs a build with -DFH2MOD_TESTS=ON in
# build-test/ and files from your own install (EN.zip from
# media\Stripped\StringTables and media\Stripped\gamedbRC.slt). SQLite is built
# into the platform and the test, so a fresh Wine prefix is all it needs:
#   tests/run_platform_test.sh <EN.zip> <gamedbRC.slt> [forza_durango_release_final.exe]
# With FH2_MEDIA set to your install's media folder, its UI.zip and stateflow.zip
# are copied in too, and the menus the platform writes (the MODS tab, pages) are
# checked. Then the database test (tests/database_test.cpp) runs in the same
# folder, with other mods' databases in mediaoverride and mediapc.
# The phases run twice, each time in a fresh folder (platform 2.9.0): first as an Azimuth before
# 1.0 (no data\titles), where the platform's files go to mediaoverride; then as Azimuth 1.0 or
# later without Extra Options (a data\titles folder), where they go to the folder's own layer.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
b=$root/build-test
exe=()
[ -n "${3:-}" ] && exe=("$(winepath -w "$3" 2>/dev/null || echo "Z:${3//\//\\}")")
game_exe=
[ -n "${3:-}" ] && game_exe=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
en=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
gamedb=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dirs=()
trap 'rm -rf "${dirs[@]}"' EXIT

run_layout() {   # run_layout mediaoverride|own
    local dir
    dir=$(mktemp -d)
    dirs+=("$dir")
    echo "==== platform test, layout: $1"
    mkdir -p "$dir/fh2-mods" "$dir/FH2/Mount/media/Stripped/StringTables" "$dir/FH2/Mount/mediaoverride/UI"
    [ "$1" = own ] && mkdir -p "$dir/data/titles/1A96545B/saves/1A96545B"
    cp "$b/platform_test.exe" "$dir/azimuth.exe"
    cp "$b/d3dcompiler_47.dll" "$b/database_test.exe" "$b/DbMod.dll" "$b/fh2mod.exe" "$dir/"
    cp "$b/AaaMod.dll" "$b/HelloMod.dll" "$b/TestMod.dll" "$b/Legacy.dll" "$dir/fh2-mods/"
    cp "$en" "$dir/FH2/Mount/media/Stripped/StringTables/EN.zip"
    cp "$gamedb" "$dir/FH2/Mount/media/Stripped/gamedbRC.slt"
    chmod u+w "$dir/FH2/Mount/media/Stripped/gamedbRC.slt"
    printf 'made by hand' > "$dir/FH2/Mount/mediaoverride/UI/handmade.xml"
    if [ -n "${FH2_MEDIA:-}" ]; then
        cp "$FH2_MEDIA/UI.zip" "$FH2_MEDIA/stateflow.zip" "$dir/FH2/Mount/media/"
    fi
    cd "$dir"
    wine azimuth.exe full "${exe[@]}"
    rm fh2-mods/TestMod.dll
    wine azimuth.exe removed
    # The database test makes its own Azimuth folders (without data\titles): once is enough.
    [ "$1" = mediaoverride ] && wine database_test.exe
    wine azimuth.exe crash
    rm -f crash.txt
    # A crash handler installed after the platform's that passes crashes on to the one it replaced
    # (platform 2.8.1); the platform puts its own back in front 10 s after the game is found.
    wine azimuth.exe crash-chained
    # Online play (platform 2.8.0): a stand-in FH2MP.dll (it only has to be there), OfflineMod, and
    # the session file the FH2MP launcher writes.
    cp "$b/Legacy.dll" fh2-mods/FH2MP.dll
    cp "$b/OfflineMod.dll" fh2-mods/
    mkdir -p data/bin
    printf 'xuid=1\ntoken=test\n' > data/bin/fh2mp-session.txt
    wine azimuth.exe online-first
    wine azimuth.exe online
    printf 'OfflineMod.dll\t1\nHelloMod.dll\t0\n' > fh2-mods/platform-online.txt
    wine azimuth.exe online-chosen
    printf 'fh2mp.dll\n' > fh2-mods/platform-disabled.txt
    wine azimuth.exe fh2mp-off
    # The HUD layer preview: a mod that adds a layer (tests/hud_mod.cpp). The phases above have none, so
    # they also check that the build behaves as 2.8.0 without one. With the exe given, this phase maps all
    # of it and has it in the game folder, as the hook test does, so the layer's code checks and hooks run.
    cp "$b/HudTestMod.dll" fh2-mods/
    [ -n "$game_exe" ] && ln -s "$game_exe" FH2/Mount/forza_durango_release_final.exe
    wine azimuth.exe hud "${exe[@]}"
    cd - >/dev/null
}

# Mods' text in the pages (src/xml_text.cpp) and car packs' car Ids (src/car_ids.cpp) on their own, then the
# platform.
wine "$b/xml_text_test.exe"
wine "$b/car_ids_test.exe"
run_layout mediaoverride
run_layout own
