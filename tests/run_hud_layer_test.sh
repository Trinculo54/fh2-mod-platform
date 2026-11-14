#!/usr/bin/env bash
# Runs the HUD layer preview's offline test (tests/hud_layer_test.cpp) under Wine. Needs a build with
# -DFH2MOD_TESTS=ON in build-test/:
#   tests/run_hud_layer_test.sh
# With FH2_MEDIA set to your install's media folder, the template is also checked against its UI.zip and
# spliced into its A_Hud.xaml, and each page the test writes is checked again by tests/hud_template_check.py
# (when python3 is on PATH). FH2_TEXTURES, a folder with copies of media\ui\textures\Anthem.zip and
# data_bound\*.zip, adds the check of the controller pictures' textures. Exits non-zero on any FAIL.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
b=$root/build-test
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cp "$b/hud_layer_test.exe" "$dir/"
media=
[ -n "${FH2_MEDIA:-}" ] && media=$(winepath -w "$FH2_MEDIA" 2>/dev/null || echo "$FH2_MEDIA")
status=0
(cd "$dir" && FH2_MEDIA="$media" wine hud_layer_test.exe out) || status=1
if [ -n "${FH2_MEDIA:-}" ] && command -v python3 > /dev/null && [ -s "$dir/out/pages.txt" ]; then
    tex=()
    [ -n "${FH2_TEXTURES:-}" ] && tex=(--textures "$FH2_TEXTURES")
    while IFS=$'\t' read -r page props parts; do
        echo "==== hud_template_check.py $page ($parts)"
        python3 -I "$root/tests/hud_template_check.py" "$dir/out/$page" "$dir/out/$props" "$FH2_MEDIA/UI.zip" --parts "$parts" "${tex[@]}" ||
            status=1
    done < "$dir/out/pages.txt"
fi
exit $status
