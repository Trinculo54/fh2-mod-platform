// Mods' text in the game's language (API 10, platform 2.9.0): translations a mod embeds as gettext PO
// files, looked up by key, and text that follows the game's language on the pages the platform writes.
// See l10n.cpp. The platform's own text is lang.cpp's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

struct FH2Mod;

namespace fh2platform {

// ---- the parts without state (tests/l10n_test.cpp checks them with no game) ---------------------
namespace l10n {

// One entry of a PO file.
struct Entry {
    std::string msgid, msgid_plural;   // the English: msgid_plural only for an entry with plural forms
    std::vector<std::string> msgstr;   // the text: [0] alone, or one per plural form ("" for one left empty)
    int line = 0;                      // where its msgid is, for log lines
};
// A PO file's entries: by msgctxt (the key a mod passes to tr), or by msgid for those without one.
struct Catalogue {
    std::map<std::string, Entry> by_key;
    std::map<std::string, Entry> by_english;
};

// Reads a PO file for `tag`'s language (msgctxt, msgid, msgid_plural, msgstr, msgstr[n], strings over
// several lines and C escapes) into `out`, adding to what's there. Entries marked fuzzy, untranslated
// ones and the header are skipped. An entry whose {placeholders} don't match the English is left out,
// with a sentence in `refused`, as is one whose text has a control character other than \n and \t
// or an escape that isn't UTF-8, and a line that isn't PO. A file that isn't UTF-8 (UTF-16, or
// Windows-1250 "ANSI") isn't read at all, with one sentence. A header whose nplurals isn't
// plural_forms(tag) gets a sentence too, though its entries are taken. Returns how many entries it took.
int parse_po(std::string_view po, const std::string& tag, Catalogue& out, std::vector<std::string>& refused);
// How many plural forms plural_form() has for `tag` (Polish 3, English 2, Japanese 1).
int plural_forms(const std::string& tag);
// The {placeholders} in `s` ("{count}", "{0}"), sorted, each once: a {...} with no space or brace inside.
std::vector<std::string> placeholders(std::string_view s);
// Whether a translation's placeholders fit the English's. A plain entry: the same ones. A plural
// form: none the English doesn't have in one form or the other, and every one both forms have,
// unless the form is `only_for_one` (only ever used for 1, so it may say "one" instead of {n}).
bool placeholders_fit(const Entry& e, const std::string& text, bool only_for_one = false);
// Whether msgstr[`form`] of a PO file for `tag` is only ever used for 1: Polish, Czech, English and
// most others' form 0, but not Russian's (21, 31...) or Portuguese and French's (0 too).
bool form_only_for_one(const std::string& tag, int form);

// The BCP-47 tag for one of the game's language codes, the names of its string-table zips ("PL" or
// "pl" -> "pl", "BR" -> "pt-BR", "CHT" -> "zh-Hant"), or nullptr for none of them (DEV, LOC).
const char* tag_for_code(std::string_view code);
// A tag spelt the usual way ("pt_BR", "PT-br" -> "pt-BR", "zh-hant" -> "zh-Hant"), or "" if it isn't one.
std::string normal_tag(std::string_view tag);
// The catalogues looked in for `tag`, best first: pt-BR, then pt; en-GB, then en; es-MX, then es-ES,
// then es. English, the msgid, comes after all of them.
std::vector<std::string> fallbacks(const std::string& tag);
// The tag a catalogue named `tag` (normal_tag's) is kept under, so the game's language finds it: a
// language the game has in one form, named with its usual region, is the language alone ("pl-PL" ->
// "pl", "ja-JP" -> "ja", "en-US" -> "en"), and Chinese for Taiwan, Hong Kong or Macau, or in
// Traditional script, is "zh-Hant" ("zh-TW", "zh-Hant-HK"). Others stay as they are ("pt-BR", "de-AT").
std::string catalogue_tag(const std::string& tag);
// "" when one of the game's languages looks in a catalogue for `tag` (fallbacks()); else, for the log,
// why none does and what to name the file instead ("... so name the file es_ES.po, es_MX.po or es.po").
std::string never_shown(const std::string& tag);
// Which msgstr[] holds the plural form for `n` in a PO file for `tag`, numbered as gettext's usual
// Plural-Forms for the language do (CLDR's rules): en and most others one/other (singular only
// for 1); pl, ru, uk, cs one/few/many; pt and fr 0 and 1 singular; ja, ko, zh one form.
int plural_form(const std::string& tag, int64_t n);
// The test language's text for `english`: [brackets] round it, its letters swapped for Polish and
// Portuguese ones, padded by how much translations grow. {placeholders} are kept as they are.
std::string pseudo(std::string_view english);
// `v` with `decimals` digits after the point (0 to 6), and `y-m-d`, as written in `tag`'s language.
// The length written (without its NUL), or 0 when `out` is too small or the value isn't usable.
int format_number(const std::string& tag, double v, int decimals, char* out, std::size_t n);
int format_date(const std::string& tag, int y, int m, int d, char* out, std::size_t n);
// "Polish" for "pl", for log lines; the tag itself for one the platform doesn't know.
std::string language_name(const std::string& tag);
// The language the game picks for a Windows regional format (GetUserDefaultLocaleName: "pl-PL", "pt-PT",
// "en-IE"), from the game's own 46-entry table (game+0x141245CF0): pl -> pl, every pt -> pt-BR, es and
// es-ES -> es-ES, other es -> es-MX, most en -> en-GB but en-US, en-AU and en-CA -> en, de, fr, it, ja,
// ru, zh-HK and zh-TW -> zh-Hant. Anything else, Korean, Czech, Hungarian and Dutch included, is "en", as
// the game gives them its English text.
std::string guess_from_windows(std::wstring_view locale);

} // namespace l10n

// ---- API 10 -----------------------------------------------------------------------------------
const char* l10n_language();
int l10n_add_translation(FH2Mod* mod, const char* lang, const char* po_utf8, std::size_t size);
const char* l10n_tr(FH2Mod* mod, const char* key, const char* english);
const char* l10n_tr_plural(FH2Mod* mod, const char* key, const char* one, const char* other, int64_t n);
int l10n_format_number(double v, int decimals, char* out, std::size_t n);
int l10n_format_date(int y, int m, int d, char* out, std::size_t n);

// ---- the platform's side ----------------------------------------------------------------------
// Before the mods load: reads fh2-mods\platform-language.ini (the language last time, and the test
// language's switch), says in the log which language mods' text starts in, and watches for the
// game's own, which it records there for the next launch.
void l10n_start();
// `english` for the string table of the game's language `table_code` (a zip's name: "PL", "br"):
// set_string_tr's text, so each language's table gets its own.
std::wstring l10n_table_text(FH2Mod* mod, const std::string& key, const std::wstring& english, std::string_view table_code);

// Following the game's language. The game picks its language seconds after fh2mod_init, so text a
// mod got from tr() then can be in the wrong one. Every text tr() and tr_plural() hand out is the
// platform's own copy, and the platform knows which key and English each one came from. Where a mod
// passes text in (a row's label, a page's title...), the platform notes the place, a `slot`, with
// what was passed: tr()'s text, or something else, which forgets the slot. Before it writes a page,
// it asks for each slot's text in the language now.
//
// `given` is what the mod passed for `slot` (any pointer, nullptr included).
void l10n_note(FH2Mod* mod, const std::string& slot, const char* given);
// `slot` holds `english` with `key` (FH2ModInfo.description, under "mod.description").
void l10n_note_key(FH2Mod* mod, const std::string& slot, const std::string& key, const std::string& english);
// The slot's text in the language now into `text`, if the slot holds tr()'s text; else `text` stays.
void l10n_follow(FH2Mod* mod, const std::string& slot, std::string& text);
// The same in `tag`'s language (l10n_language()'s), for a page that follows many slots at once.
void l10n_follow(FH2Mod* mod, const std::string& slot, std::string& text, const std::string& tag);
// The slot's text in the string table of `table_code`'s language; false (and `text` as it was) if
// the slot doesn't hold tr()'s text.
bool l10n_text_in(FH2Mod* mod, const std::string& slot, std::string_view table_code, std::string& text);
// The same for text the platform keeps as UTF-16 and the game asks for as it shows it (a
// title-screen list): `shown` is the platform's copy, which stays put, `suffix` what the platform
// added after the mod's text, and `max_chars` (0: no limit) how long the mod's text can be in
// characters (text_chars), cut short (cut_text) as the platform cut what it was given.
void l10n_note_w(FH2Mod* mod, const wchar_t* shown, const char* given, const std::wstring& suffix = {}, std::size_t max_chars = 0);
// `shown`'s text in the language now, or nullptr if it doesn't hold tr()'s text. Stays valid.
const wchar_t* l10n_follow_w(const wchar_t* shown);

} // namespace fh2platform
