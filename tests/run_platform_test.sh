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
# later without Extra Options (a data\titles folder, with the tmp\Turn10Temp.scratch folder the game
# makes on its T:\ drive), where they go to the folder's own layer.
# EN.zip is copied as PL.zip and br.zip too, for the tables each language gets in its own words
# (API 10), and the language last time is Polish (fh2-mods\platform-language.ini).
# With the exe given, the phases that map it ("full" and "hud") have it in the game folder too, as
# a real install does, so the platform checks the game's code against it and its hooks go in (pop-ups,
# UI sounds, the database guards; the HUD layer's too with FH2_MEDIA); without it, they stay out and say
# why. The test's copy of the game can't run game code, so with FH2_MEDIA it keeps the MODS menus out
# (tests/platform_test.cpp, map_game).
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
    [ "$1" = own ] && mkdir -p "$dir/data/titles/1A96545B/saves/1A96545B" "$dir/data/titles/1A96545B/tmp/Turn10Temp.scratch"
    cp "$b/platform_test.exe" "$dir/azimuth.exe"
    cp "$b/d3dcompiler_47.dll" "$b/database_test.exe" "$b/DbMod.dll" "$b/fh2mod.exe" "$dir/"
    # the hook test's stand-in for Extra Options, which the database test loads by this name when it plays it
    cp "$b/winmm.dll" "$dir/fake-eo.dll"
    cp "$b/AaaMod.dll" "$b/HelloMod.dll" "$b/TestMod.dll" "$b/Legacy.dll" "$dir/fh2-mods/"
    # the game exe where Azimuth has it, for the phases that map it (the platform reads its code from there)
    [ -n "$game_exe" ] && ln -s "$game_exe" "$dir/FH2/Mount/forza_durango_release_final.exe"
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
    # FH2MP's own rules for its session (platform 2.9.0): only where FH2MP looks (data\bin when there is one), and
    # not once it's older than FH2MP's limit (30 minutes, or FH2MP_SESSION_MAX_AGE_S). A fresh one left next to
    # azimuth.exe and a three-hour-old one in data\bin: offline. With the limit off, the old one counts.
    rm fh2-mods/platform-disabled.txt fh2-mods/platform-online.txt
    printf 'xuid=1\ntoken=test\n' > fh2mp-session.txt
    touch -d '3 hours ago' data/bin/fh2mp-session.txt
    wine azimuth.exe online-stale
    FH2MP_SESSION_MAX_AGE_S=0 wine azimuth.exe online-nolimit
    rm fh2mp-session.txt
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
    # it (OfflineMod, TestMod) not started. Then FH2MP asking to go online with no session at all, after the mods have
    # started: OfflineMod and HelloMod have run (a hook, text, SQL, a file), so nothing changes and FH2MP is told.
    rm fh2-mods/ZzFH2MP.dll
    cp "$b/FakeFH2MP.dll" fh2-mods/MpClient.dll
    cp "$b/TestMod.dll" fh2-mods/
    printf 'HelloMod.dll\t0\nTestMod.dll\t0\n' > fh2-mods/platform-online.txt
    wine azimuth.exe online-register
    rm fh2-mods/TestMod.dll data/bin/fh2mp-session.txt
    FH2_TEST_NO_SESSION=1 FH2_TEST_OFFLINE_EDITS=1 wine azimuth.exe online-ready
    # The same with OfflineMod only registered, nothing of it run, and HelloMod set to load online: FH2MP's
    # going_online stops OfflineMod, and the game goes online.
    printf 'HelloMod.dll\t1\n' > fh2-mods/platform-online.txt
    FH2_TEST_NO_SESSION=1 FH2_TEST_OFFLINE_IDLE=1 wine azimuth.exe online-clean
    # back as the HUD phase has always had it
    rm fh2-mods/MpClient.dll
    cp "$b/Legacy.dll" fh2-mods/FH2MP.dll
    printf 'OfflineMod.dll\t1\nHelloMod.dll\t0\n' > fh2-mods/platform-online.txt
    printf 'fh2mp.dll\n' > fh2-mods/platform-disabled.txt
    # The HUD layer preview: a mod that adds a layer (tests/hud_mod.cpp). The phases above have none, so
    # they also check that the build behaves as 2.8.0 without one. With the exe given, this phase maps all
    # of it (and has it in the game folder, as above), as the hook test does, so the layer's code checks and
    # hooks run.
    cp "$b/HudTestMod.dll" fh2-mods/
    wine azimuth.exe hud "${exe[@]}"
    cd - >/dev/null
}

# What the built platform imports (the d3dcompiler_47.dll and DWrite.dll copied in below): only KERNEL32.dll, USER32.dll
# and the C runtime, load-time or delay-loaded (CMakeLists.txt).
wine "$b/imports_test.exe"
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
    # po2inc.py reads a file named with a region the game doesn't use as the game's language (pl_PL.po as pl,
    # zh_TW.po as zh-Hant), leaves out one no language looks for (es_AR.po) and an entry with a control character,
    # warns about a header whose nplurals isn't the platform's, and stops at a file that isn't UTF-8, as the platform does
    po=$(mktemp -d)
    dirs+=("$po")
    printf '%s\n' 'msgid ""' 'msgstr ""' '"Plural-Forms: nplurals=4; plural=0;\n"' '' 'msgctxt "a"' 'msgid "A"' \
        "msgstr \"$(printf '\304\204')\"" '' 'msgctxt "b"' 'msgid "B"' 'msgstr "B\x01"' > "$po/pl_PL.po"
    printf '%s\n' 'msgctxt "a"' 'msgid "A"' "msgstr \"$(printf '\347\224\262')\"" > "$po/zh_TW.po"
    printf '%s\n' 'msgctxt "a"' 'msgid "A"' 'msgstr "A"' > "$po/es_AR.po"
    printf '%s\n' 'msgctxt "a"' 'msgid "A"' "msgstr \"$(printf '\243')\"" > "$po/pl.po"
    tools_ok=1
    python3 -I "$root/tools/l10n/po2inc.py" "$po/out.inc" t "$po/pl_PL.po" "$po/zh_TW.po" "$po/es_AR.po" 2> "$po/warnings.txt" || tools_ok=0
    grep -q '{"pl",' "$po/out.inc" && grep -q '{"zh-Hant",' "$po/out.inc" && ! grep -q '"es-AR"' "$po/out.inc" || tools_ok=0
    grep -q "es_AR.po: none of the game's languages looks for es-AR text (its Spanish is es-ES, for Spain, or es-MX, for Latin America), so name the file es_ES.po, es_MX.po or es.po; left out" "$po/warnings.txt" || tools_ok=0
    grep -q 'pl_PL.po:10: "b": its text has a control character (U+0001)' "$po/warnings.txt" || tools_ok=0
    grep -q "pl_PL.po: its header says nplurals=4, but the platform reads 3 plural forms for Polish" "$po/warnings.txt" || tools_ok=0
    if python3 -I "$root/tools/l10n/po2inc.py" "$po/out2.inc" t "$po/pl.po" 2> "$po/error.txt"; then tools_ok=0; fi
    grep -q "pl.po:3 isn't UTF-8: save it as UTF-8" "$po/error.txt" || tools_ok=0
    # of two files for one language, pl.po is used, though pl-PL.po sorts first; an escape that isn't UTF-8 is left out
    mkdir "$po/two"
    printf '%s\n' 'msgctxt "a"' 'msgid "A"' 'msgstr "Ok"' '' 'msgctxt "e"' 'msgid "E"' 'msgstr "Caf\xE9"' > "$po/two/pl.po"
    printf '%s\n' 'msgctxt "a"' 'msgid "A"' 'msgstr "Hej"' > "$po/two/pl-PL.po"
    python3 -I "$root/tools/l10n/po2inc.py" "$po/two.inc" t "$po/two/pl-PL.po" "$po/two/pl.po" 2> "$po/two.txt" || tools_ok=0
    [ "$(grep -c '{"pl",' "$po/two.inc")" = 1 ] && grep -q 'msgstr \\"Ok\\"' "$po/two.inc" && ! grep -q 'Hej\|Caf' "$po/two.inc" || tools_ok=0
    grep -q "pl-PL.po is Polish (pl) too, and pl.po is the one used; left out" "$po/two.txt" || tools_ok=0
    grep -q "pl.po:6: \"e\": its text has an escape that isn't UTF-8 (such as \\\\xE9); left out" "$po/two.txt" || tools_ok=0
    if [ "$tools_ok" = 1 ]; then
        echo "ok   tools/l10n: po2inc.py reads pl_PL.po as pl and zh_TW.po as zh-Hant, and leaves out and warns as the platform does"
    else
        echo "FAIL tools/l10n: po2inc.py's tags and warnings"
        cat "$po/warnings.txt" "$po/error.txt" "$po/two.txt"
    fi
fi
wine "$b/wine_test.exe"
# The database guards' rules (src/db_guard_rules.cpp); given the exe, their code is checked against it too.
wine "$b/db_guards_test.exe" "${exe[@]}"
run_layout mediaoverride
run_layout own
