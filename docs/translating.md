# Translating mods

From platform 2.9.0 (API 10), a mod can show its text in the player's language: on the Mods page, the MODS tab, its own pages, the title screen and in the game's string tables. This page is for mod makers who want that, and for the translators who help them. The API itself is in [`api.md`](api.md#translations-api-10).

How it works, in short:

1. The mod's text is written in English, each text with a **key**: a name for it that stays the same when the English changes.
2. `tools/l10n/extract.py` lists every text in **`l10n/<mod>.pot`**, with where it shows and how long it can be.
3. Translators make a **PO file** per language from it: `l10n/pl.po`, `l10n/pt_BR.po`. Poedit, Weblate and most other translation tools read and write them.
4. `tools/l10n/po2inc.py` builds the PO files into the mod's DLL, so the mod's zip doesn't change and nothing is downloaded.
5. In game, the platform looks each text up in the game's language, and shows the English for anything not translated.

The game's languages, and the names the PO files go by:

| Game language | Tag | PO file | Notes |
|---|---|---|---|
| English (US) | `en` | | The English in the code |
| English (UK) | `en-GB` | `en_GB.po` | Falls back to `en` |
| Polish | `pl` | `pl.po` | Three plural forms |
| Brazilian Portuguese | `pt-BR` | `pt_BR.po` | Falls back to `pt` (`pt.po`) |
| German | `de` | `de.po` | |
| French | `fr` | `fr.po` | |
| Spanish (Spain) | `es-ES` | `es_ES.po` | Falls back to `es` |
| Spanish (Mexico) | `es-MX` | `es_MX.po` | Falls back to `es-ES`, then `es` |
| Italian | `it` | `it.po` | |
| Japanese | `ja` | `ja.po` | Only the characters the game's own Japanese uses |
| Korean | `ko` | `ko.po` | The same |
| Traditional Chinese | `zh-Hant` | `zh_Hant.po` | The same |
| Russian | `ru` | `ru.po` | Three plural forms |
| Dutch | `nl` | `nl.po` | |
| Czech | `cs` | `cs.po` | The game itself shows English |
| Hungarian | `hu` | `hu.po` | The game itself shows English |

## For mod makers

### Write the text with keys

Use the helpers in `include/fh2mod_l10n.h` (in your `platform/` submodule). They work on older platforms too, where they give the English.

```cpp
#include "fh2mod_l10n.h"

// L10N: the switch for breakable trees, on the Mods page
s.label = FH2_TR(api, g_mod, "row.Trees.label", "Breakable trees");
// L10N(description): what the switch does, beside the list
s.description = FH2_TR(api, g_mod, "row.Trees.description", "Trees break when you hit them.");
```

- **The key** is the PO file's `msgctxt`. Make it say where the text is: `row.<setting key>.label`, `row.<key>.description`, `row.<key>.choice.<n>`, `page.<page key>.title`, `title.<entry key>.label`, `notice`, `popup.<name>.title`. Keep a key when you reword its English: the old translation then stops showing (English shows until it's translated again), and translation tools mark it to be checked rather than lost.
- **The English** has to be a plain string literal, so `extract.py` can see it. Never build it from pieces: a sentence glued from parts can't be translated, since other languages put the parts in another order.
- **Numbers and names** go in as `{placeholders}`, filled in after the lookup: `"{name} is available"`. Name them for what they are (`{count}`, `{name}`, `{version}`), so a translator knows what goes there; `fh2_fill` fills them in.
- **Counts** take `FH2_TR_PLURAL`: `FH2_TR_PLURAL(api, g_mod, "count.trees", "{n} tree broken", "{n} trees broken", n)`. The platform picks the language's form for `n`; Polish has three. Write the number with `fh2_number(api, n)` (or `api->format_number`), so it's `1 234` in Polish and `1.234` in Portuguese.
- **A note for translators** goes in a `// L10N:` comment on the line above (or at the end of the line): where the text shows, what its placeholders hold, anything easy to get wrong. Start it with a slot kind in brackets to set the length limit, `// L10N(choice): ...`, or `// L10N(max=24): ...` for a limit of your own.
- **Your description** (`FH2ModInfo.description`) is marked `FH2_TR_DESCRIPTION("...")`, and is listed under the key `mod.description`; the platform looks it up itself.
- **Game string tables:** `api->set_string_tr(g_mod, "Table.str", key, "my.key", L"English")` writes each language's table with that language's text.

Text you get from `FH2_TR` and pass on to the platform as it is follows the game's language by itself, even though the game picks its language after `fh2mod_init` ([`api.md`](api.md#translations-api-10) lists where). Text you build from it (with a number filled in) is yours: set it again when the page opens.

### The length limits

The game's menus have room for so many characters, and translations are often longer than the English (short labels up to twice as long). `extract.py` gives each text a `max-length` that translation tools show and check. It comes from the slot kind in the `L10N(...)` comment, or else from the key: `*.label` 30, `*.description` and `mod.description` 450, `*.choice.*` 12, `title.*` 24, `notice*` 40, `tile.*` 18, `tab.*` 12.

| Slot kind | Characters | Slot kind | Characters |
|---|---|---|---|
| `row-label` | 30 | `title-entry` | 24 |
| `slider-label` | 26 | `notice-line` | 40 |
| `description` | 450 | `hud-title` | 28 |
| `tile-heading` | 18 | `hud-text` | 90 |
| `tab-name` | 12 | `section-title` | 22 |
| `choice` | 12 | `card-name` | 40 |
| `popup-title` | 28 | `card-description` | 110 |
| `popup-subtitle` | 45 | | |

### Make the files

Run these from your repo; Python 3 is all they need.

```sh
# the English, with notes and limits: l10n/MyMod.pot (commit it)
python3 platform/tools/l10n/extract.py --domain MyMod src
# the translations, built in: src/mymod_l10n.inc
python3 platform/tools/l10n/po2inc.py src/mymod_l10n.inc mymod_translations l10n/*.po
```

Both have `--check`, which writes nothing and fails when the file isn't up to date: run them in CI. `extract.py` stops, with the file and line, at a key or English in `FH2_TR` (or its siblings) that isn't a plain string literal, and at a key used for two different English texts. A call to `api->tr()` it can't read gets a warning instead and is passed over, since another object can have a `tr` of its own; use the macros, and nothing slips past. `po2inc.py` leaves out what the platform would refuse anyway (entries marked fuzzy, untranslated ones, ones whose placeholders don't match the English) and warns about the placeholders.

Then hand them to the platform straight after `register_mod`:

```cpp
#include "fh2mod_l10n.h"
#include "mymod_l10n.inc"

g_mod = api->register_mod(&info);
FH2_ADD_TRANSLATIONS(api, g_mod, mymod_translations);
```

When you change the English, run `extract.py` again. Translation tools (Weblate, or `msgmerge -U l10n/pl.po l10n/MyMod.pot`) then mark changed texts as fuzzy, to be checked; fuzzy text never ships.

### Test it

- **The test language** shows every translatable text padded and with Polish and Portuguese letters, in brackets: `[Bręãkãbłę tręęś ćęłńóśźż]`. Put `test=1` in `fh2-mods\platform-language.ini` and start the game. Text that's cut off, text that isn't translatable (it shows without brackets) and capitals that only work for A to Z show straight away. Put `test=0` back afterwards.
- **A translator's file** in `fh2-mods\lang\<your mod's name>\pl.po` is used instead of the one in your DLL, with a line in `platform.log`. That's how translators try their work in game.
- `platform.log` says which language the game is in ("Language: ..."), which translations each mod added, and every entry it left out, and why.

## For translators

You'll get the mod's `.pot` file (or a link to it on Weblate), and you give back a `.po` file for your language. Poedit (poedit.net) is a free editor for them: open the `.pot`, and it makes a new translation from it.

- **Each text has a context** (`msgctxt`): its key, which says where it shows (`row.Trees.label` is a row's label on the Mods page). Read the note (`#.`) above it too.
- **Keep every `{placeholder}`** exactly as it is: `{count}`, `{name}`. You can move them, but not translate or drop them, or the platform leaves your translation out. One exception: a plural form your language uses for 1 alone may leave out the number ("Jedno drzewo" for "{n} tree broken"). That's the first form in Polish, Czech, German and most others; not in Russian, whose first form is also for 21, 31 and so on, nor in Brazilian Portuguese and French, which use it for 0 as well.
- **Plurals** have one box per form in your language: Polish has three (1 drzewo; 2, 3, 4, 22 drzewa; 5, 11, 25 drzew). Fill in all of them.
- **Mind `max-length`**: more characters than that won't fit on screen. Shorten rather than overflow.
- **Use the game's own words** for its things (Barn Finds, Bucket List, Showcase): look at how the game says them in your language. Address the player the way the game does (in Polish "ty", in Brazilian Portuguese "você").
- **Only letters the game's font has** show; others come out as boxes. The letters of the languages the game itself is in (Polish, Portuguese, German, French, Spanish, Italian, Dutch, Russian) are fine. The game's Czech and Hungarian are English, so check letters such as ř, ů and ő in game first. In Japanese, Korean and Chinese the font only has the characters the game's own text uses.
- **Don't use `"`, `<` or `&`** in text that goes on the Mods page or the MODS tab; use `''` for quotes.
- **Fuzzy means unchecked**: an entry marked fuzzy (Poedit: "Needs work") isn't shown. Clear it once you're happy with the text.

To see your work in game, put your file in `fh2-mods\lang\<the mod's name>\` next to `azimuth.exe`, named for your language (`pl.po`, `pt_BR.po`; the table above has the rest), and start the game in your language. `fh2-mods\platform.log` says that it read your file, and names any entry it left out and why. When it looks right, send the file back to the mod's author (or commit it on Weblate).
