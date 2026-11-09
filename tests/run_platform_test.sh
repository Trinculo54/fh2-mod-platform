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
# EN.zip is copied as PL.zip and br.zip too, for the tables each language gets in its own words
# (API 10), and the language last time is Polish (fh2-mods\platform-language.ini).
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
    cp "$en" "$dir/FH2/Mount/media/Stripped/StringTables/PL.zip"
    cp "$en" "$dir/FH2/Mount/media/Stripped/StringTables/br.zip"
    # Translations (API 10): Polish last time, and a Brazilian Portuguese file for TestMod that wins
    # over the one built into it (with an entry whose placeholder is wrong, on line 10)
    printf '; written by the test\r\nlast=pl\r\n' > "$dir/fh2-mods/platform-language.ini"
    mkdir -p "$dir/fh2-mods/lang/TestMod"
    printf 'msgctxt "row.Greeting.label"\nmsgid "Test greeting"\nmsgstr "Sauda\303\247\303\243o de teste"\n\nmsgctxt "loading.tip"\nmsgid "A loading tip from TestMod"\nmsgstr "Uma dica do TestMod"\n\nmsgctxt "row.Bad.label"\nmsgid "{count} unlocked"\nmsgstr "{n\303\272mero} desbloqueados"\n' \
        > "$dir/fh2-mods/lang/TestMod/pt_BR.po"
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
    # The cues about where mod settings are (platform 2.9.0), told from fh2-mods\platform-seen.txt: an older platform
    # ran here (platform-mods.txt there, platform-seen.txt not); then OfflineMod added once the welcome has had its
    # three sessions; then YOUR MODS opened. Both files are put back after, for the phases below.
    cp fh2-mods/platform-seen.txt seen.keep
    cp fh2-mods/platform-mods.txt mods.keep
    rm fh2-mods/platform-seen.txt
    wine azimuth.exe upgrade
    printf '*\twelcome.sessions\t3\t2026-11-08\n' >> fh2-mods/platform-seen.txt
    cp "$b/OfflineMod.dll" fh2-mods/
    wine azimuth.exe newmod
    printf '*\thome\tseen\t2026-11-08\n' >> fh2-mods/platform-seen.txt
    wine azimuth.exe home
    rm fh2-mods/OfflineMod.dll
    mv seen.keep fh2-mods/platform-seen.txt
    mv mods.keep fh2-mods/platform-mods.txt
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
    # Online play found where 2.8.0 missed it (platform 2.9.0, issue #22): the session next to azimuth.exe in a
    # folder without data\bin; then a stand-in FH2MP under another name (FakeFH2MP.dll as ZzFH2MP.dll, after
    # OfflineMod) that writes the session while the mods start, and the same again once it's known as FH2MP.
    rm -rf data/bin fh2-mods/platform-disabled.txt fh2-mods/platform-online.txt
    printf 'xuid=1\ntoken=test\n' > fh2mp-session.txt
    wine azimuth.exe online-root
    rm fh2mp-session.txt fh2-mods/FH2MP.dll
    mkdir -p data/bin
    cp "$b/FakeFH2MP.dll" fh2-mods/ZzFH2MP.dll
    FH2_TEST_OFFLINE_EDITS=1 wine azimuth.exe online-late
    wine azimuth.exe online-renamed
    # FH2MP under a name the platform doesn't know yet (MpClient.dll), its session already there: online once it
    # registers, with mods before it that the player set to stay off online (HelloMod) stopped, and the ones after
    # it (OfflineMod, TestMod) not started. Then FH2MP saying it's going online with no session at all, after the
    # mods have started: OfflineMod's text, SQL and file come out again.
    rm fh2-mods/ZzFH2MP.dll
    cp "$b/FakeFH2MP.dll" fh2-mods/MpClient.dll
    cp "$b/TestMod.dll" fh2-mods/
    printf 'HelloMod.dll\t0\nTestMod.dll\t0\n' > fh2-mods/platform-online.txt
    wine azimuth.exe online-register
    rm fh2-mods/TestMod.dll data/bin/fh2mp-session.txt
    FH2_TEST_NO_SESSION=1 FH2_TEST_OFFLINE_EDITS=1 wine azimuth.exe online-ready
    # back as the HUD phase has always had it
    rm fh2-mods/MpClient.dll
    cp "$b/Legacy.dll" fh2-mods/FH2MP.dll
    printf 'OfflineMod.dll\t1\nHelloMod.dll\t0\n' > fh2-mods/platform-online.txt
    printf 'fh2mp.dll\n' > fh2-mods/platform-disabled.txt
    # The HUD layer preview: a mod that adds a layer (tests/hud_mod.cpp). The phases above have none, so
    # they also check that the build behaves as 2.8.0 without one. With the exe given, this phase maps all
    # of it and has it in the game folder, as the hook test does, so the layer's code checks and hooks run.
    cp "$b/HudTestMod.dll" fh2-mods/
    [ -n "$game_exe" ] && ln -s "$game_exe" FH2/Mount/forza_durango_release_final.exe
    wine azimuth.exe hud "${exe[@]}"
    cd - >/dev/null
}

# Mods' text in the pages (src/xml_text.cpp), the platform's text in any language (src/text.cpp), car packs'
# car Ids (src/car_ids.cpp), mods' translations (src/l10n.cpp), what the player has already seen (src/seen.cpp), the
# pop-up queue's rules (src/popup_queue.cpp), the rules of message boxes, sounds and pages' help and hints
# (src/ui_rules.cpp) and what's different under Wine and Proton (src/wine.cpp) on their own, then the platform. With
# Python 3, TestMod's translation files are checked to be up to date with tools/l10n (its .pot from its source, its
# built-in .inc from tests/l10n/*.po).
wine "$b/xml_text_test.exe"
wine "$b/text_test.exe"
# Every English text the code translates is in src/lang.cpp's table, with its placeholders kept (and with
# FH2_MEDIA set, every translation is in its language's font), when python3 is on PATH.
if command -v python3 > /dev/null; then
    fonts=()
    [ -n "${FH2_MEDIA:-}" ] && [ -f "$FH2_MEDIA/ui/Fonts.zip" ] && fonts=("$FH2_MEDIA/ui/Fonts.zip")
    python3 -I "$root/scripts/check_fonts.py" "${fonts[@]}"
fi
wine "$b/car_ids_test.exe"
wine "$b/l10n_test.exe"
wine "$b/seen_test.exe"
wine "$b/ui_queue_test.exe"
wine "$b/ui_rules_test.exe"
# The platform's own UI files, the check on pages of a mod's own design and the page crash guard's state
# (src/xaml_safety.cpp), against fake zips; with FH2_MEDIA set, against the install's UI.zip and textures as well.
if [ -n "${FH2_MEDIA:-}" ]; then
    FH2_MEDIA="$(winepath -w "$FH2_MEDIA" 2>/dev/null || echo "$FH2_MEDIA")" wine "$b/xaml_safety_test.exe"
else
    wine "$b/xaml_safety_test.exe"
fi
if command -v python3 > /dev/null; then
    python3 -I "$root/tools/l10n/extract.py" --check --domain TestMod -o "$root/tests/l10n/test_mod.pot" "$root/tests/test_mod.cpp"
    python3 -I "$root/tools/l10n/po2inc.py" --check "$root/tests/l10n/test_mod_l10n.inc" test_mod_translations \
        "$root/tests/l10n/pl.po" "$root/tests/l10n/pt_BR.po" 2> /dev/null
    echo "ok   tools/l10n: TestMod's .pot and .inc are up to date"
fi
wine "$b/wine_test.exe"
run_layout mediaoverride
run_layout own
