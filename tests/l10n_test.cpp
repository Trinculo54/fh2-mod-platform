// Mods' text in the game's language (src/l10n.cpp, API 10), with no game: PO files, lookups and their
// fallbacks, plural forms, number and date formats, the test language, the override folder and text
// that follows the game's language. Built with stand-ins for the three things l10n.cpp asks the rest
// of the platform: platform.log, the Azimuth folder (a folder of its own under %TEMP%) and the game's
// language (set by the test). Run l10n_test.exe; it runs itself again with "qps" for the test
// language and with "first" for a first launch, since the language state is read once per session.
#include "l10n.hpp"
#include "platform.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace fh2platform;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(x) do { bool ok_ = (x); std::printf("%s %s\n", ok_ ? "ok  " : "FAIL", #x); if (!ok_) ++fails; } while (0)

// ---- the stand-ins ----------------------------------------------------------------------------
static std::vector<std::string> g_log;
static fs::path g_dir;
static std::string g_game;   // the game's language code, "" before it has one

namespace fh2platform {
void log_line(const std::string& who, const std::string& text) {
    g_log.push_back(who.empty() ? text : "[" + who + "] " + text);
    std::printf("log: %s\n", g_log.back().c_str());
}
fs::path azimuth_dir() { return g_dir; }
Launched launched_game() { return Launched::not_yet; }
int game_language(char* code, size_t size) {
    if (!code || size < 4) return 0;
    code[0] = '\0';
    if (g_game.empty()) return 0;
    std::snprintf(code, size, "%s", g_game.c_str());
    return 1;
}
} // namespace fh2platform

static bool logged(const std::string& s) {
    for (const std::string& l : g_log)
        if (l.find(s) != std::string::npos) return true;
    return false;
}

static void write(const fs::path& f, const std::string& text) {
    fs::create_directories(f.parent_path());
    std::ofstream{f, std::ios::binary} << text;
}

static int add(FH2Mod* m, const char* lang, const char* po) { return l10n_add_translation(m, lang, po, std::strlen(po)); }

static std::string num(const char* tag, double v, int d) {
    char b[64];
    return l10n::format_number(tag, v, d, b, sizeof b) ? b : "<none>";
}
static std::string date(const char* tag, int y, int m, int d) {
    char b[32];
    return l10n::format_date(tag, y, m, d, b, sizeof b) ? b : "<none>";
}

// Every character of `s` is printable ASCII or one of the Polish and Portuguese letters.
static bool in_letter_set(const std::string& s) {
    const std::wstring allowed = L"\u0105\u0107\u0119\u0142\u0144\u00F3\u015B\u017A\u017C\u0104\u0106\u0118\u0141\u0143\u00D3\u015A\u0179\u017B"
                                 L"\u00E1\u00E2\u00E3\u00E0\u00E7\u00E9\u00EA\u00ED\u00F4\u00F5\u00FA\u00C1\u00C2\u00C3\u00C0\u00C7\u00C9\u00CA\u00CD\u00D4\u00D5\u00DA";
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0 && !s.empty()) return false;
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    for (wchar_t c : w)
        if (!(c >= 0x20 && c < 0x7F) && c != L'\n' && allowed.find(c) == std::wstring::npos) return false;
    return true;
}

// A Polish PO file with a bit of everything: the header, comments, context, text over several
// lines, escapes, plural forms, a fuzzy entry, an untranslated one, one without context, one whose
// placeholders are wrong, an old one (#~) and a line that isn't PO. CRLF, with a BOM.
static const char kPolish[] =
    "\xEF\xBB\xBF# Polish translation for TestMod.\r\n"
    "msgid \"\"\r\n"
    "msgstr \"\"\r\n"
    "\"Language: pl\\n\"\r\n"
    "\"Plural-Forms: nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\\n\"\r\n"
    "\r\n"
    "#. L10N: row label on the Mods page\r\n"
    "#: src/test.cpp:12\r\n"
    "#, max-length:30\r\n"
    "msgctxt \"row.Trees.label\"\r\n"
    "msgid \"Breakable trees\"\r\n"
    "msgstr \"Łamliwe drzewa\"\r\n"
    "\r\n"
    "msgctxt \"row.Trees.description\"\r\n"
    "msgid \"\"\r\n"
    "\"Trees break when you hit them.\\n\"\r\n"
    "\"Logs fly.\"\r\n"
    "msgstr \"\"\r\n"
    "\"Drzewa łamią się, gdy w nie uderzysz.\\n\"\r\n"
    "\"Kłody \\\"latają\\\"\\t\\x41\\102.\"\r\n"
    "\r\n"
    "msgctxt \"count.trees\"\r\n"
    "msgid \"One tree broken\"\r\n"
    "msgid_plural \"{n} trees broken\"\r\n"
    "msgstr[0] \"Jedno drzewo złamane\"\r\n"
    "msgstr[1] \"{n} drzewa złamane\"\r\n"
    "msgstr[2] \"{n} drzew złamanych\"\r\n"
    "\r\n"
    "#, fuzzy\r\n"
    "msgctxt \"row.Fuzzy.label\"\r\n"
    "msgid \"Not sure yet\"\r\n"
    "msgstr \"Jeszcze nie wiem\"\r\n"
    "\r\n"
    "msgctxt \"row.Empty.label\"\r\n"
    "msgid \"Not translated\"\r\n"
    "msgstr \"\"\r\n"
    "\r\n"
    "msgid \"No context here\"\r\n"
    "msgstr \"Tu bez kontekstu\"\r\n"
    "\r\n"
    "msgctxt \"row.Bad.label\"\r\n"
    "msgid \"{count} unlocked\"\r\n"
    "msgstr \"Odblokowano {liczba}\"\r\n"
    "\r\n"
    "msgctxt \"row.Stale.label\"\r\n"
    "msgid \"Old English\"\r\n"
    "msgstr \"Stary polski\"\r\n"
    "\r\n"
    "#~ msgctxt \"row.Gone.label\"\r\n"
    "#~ msgid \"Gone\"\r\n"
    "#~ msgstr \"Nie ma\"\r\n"
    "\r\n"
    "this line isn't PO\r\n"
    "msgctxt \"greeting\"\r\n"
    "msgid \"Hello, {name}!\"\r\n"
    "msgstr \"Cześć, {name}!\"";

static int qps_phase() {
    write(g_dir / "fh2-mods" / "platform-language.ini", "; test\r\nlast=pl\r\ntest=1\r\n");
    FH2Mod mod;
    mod.name = "QpsMod";
    CHECK(l10n_add_translation(&mod, "pl", kPolish, sizeof kPolish - 1) == 6);
    CHECK(std::strcmp(l10n_language(), "qps") == 0);
    const std::string t = l10n_tr(&mod, "row.Trees.label", "Breakable trees");
    std::printf("qps: %s\n", t.c_str());
    CHECK(t.front() == '[' && t.back() == ']' && in_letter_set(t) && t.find("Breakable") == std::string::npos);
    const std::string g = l10n_tr(&mod, "greeting", "Hello, {name}!");
    CHECK(g.find("{name}") != std::string::npos && in_letter_set(g));
    CHECK(std::string(l10n_tr_plural(&mod, "count.trees", "One tree broken", "{n} trees broken", 3)).find("{n}") != std::string::npos);
    // every language's string table gets the test language too
    CHECK(l10n_table_text(&mod, "row.Trees.label", L"Breakable trees", "EN").front() == L'[');
    char b[32];
    CHECK(l10n_format_number(1234.5, 1, b, sizeof b) && std::string(b) == "1\xC2\xA0" "234,5");
    // the game's language is still recorded for next time, with test=1 kept
    l10n_start();
    g_game = "BR";
    std::string ini;
    for (int i = 0; i < 50 && ini.find("last=pt-BR") == std::string::npos; ++i) {
        Sleep(100);
        std::ifstream in{g_dir / "fh2-mods" / "platform-language.ini", std::ios::binary};
        ini.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(ini.find("last=pt-BR") != std::string::npos && ini.find("test=1") != std::string::npos);
    CHECK(logged("Language: the test language is on (test=1 in fh2-mods\\platform-language.ini)"));
    CHECK(std::strcmp(l10n_language(), "qps") == 0);
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails;
}

// The first launch: no platform-language.ini, so mods' text starts in the language the game will pick for
// this PC's Windows regional format, and the game is in Polish.
static int first_phase() {
    wchar_t windows[LOCALE_NAME_MAX_LENGTH] = {};
    GetUserDefaultLocaleName(windows, LOCALE_NAME_MAX_LENGTH);
    const std::string guess = l10n::guess_from_windows(windows);
    l10n_start();
    CHECK(logged("Language: mods' text starts in " + l10n::language_name(guess) + " (" + guess + "), the game's choice for "));
    CHECK(std::string(l10n_language()) == guess);
    g_game = "PL";
    for (int i = 0; i < 50 && !logged("Language: the game's text is in"); ++i) Sleep(100);
    if (guess == "pl")
        CHECK(logged("Language: the game's text is in Polish (pl), as expected"));
    else
        CHECK(logged("Language: the game's text is in Polish (pl); mods' text started in " + l10n::language_name(guess) +
                     " this first time (from Windows' regional format), and follows it on every page"));
    CHECK(!logged("as last time"));
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails;
}

// Runs this exe again for a phase of its own (the language state is read once per session).
static DWORD run_phase(const wchar_t* phase) {
    std::fflush(stdout);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" " + phase;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    DWORD code = 1;
    if (CreateProcessW(self, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return code;
}

int wmain(int argc, wchar_t** argv) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = fs::path(tmp) / (L"fh2-l10n-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(g_dir / "fh2-mods");
    struct Cleanup {
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(g_dir, ec);
        }
    } cleanup;
    if (argc > 1 && std::wstring(argv[1]) == L"qps") return qps_phase();
    if (argc > 1 && std::wstring(argv[1]) == L"first") return first_phase();

    // ---- PO files
    {
        l10n::Catalogue c;
        std::vector<std::string> refused;
        const int n = l10n::parse_po(kPolish, "pl", c, refused);
        CHECK(n == 6);   // Trees label and description, the plural, no context, Stale, greeting
        CHECK(c.by_key.count("row.Trees.label") && c.by_key["row.Trees.label"].msgstr[0] == "Łamliwe drzewa");
        CHECK(c.by_key["row.Trees.description"].msgid == "Trees break when you hit them.\nLogs fly.");
        CHECK(c.by_key["row.Trees.description"].msgstr[0] == "Drzewa łamią się, gdy w nie uderzysz.\nKłody \"latają\"\tAB.");
        const l10n::Entry& p = c.by_key["count.trees"];
        CHECK(p.msgid_plural == "{n} trees broken" && p.msgstr.size() == 3 && p.msgstr[2] == "{n} drzew złamanych");
        CHECK(!c.by_key.count("row.Fuzzy.label") && !c.by_key.count("row.Empty.label") && !c.by_key.count("row.Gone.label"));
        CHECK(c.by_english.count("No context here") && c.by_english.size() == 1);
        CHECK(!c.by_key.count("row.Bad.label"));
        CHECK(refused.size() == 2);
        CHECK(refused.size() > 0 && refused[0].find("\"row.Bad.label\": its placeholders ({liczba}) don't match the English's ({count})") !=
                                        std::string::npos);
        CHECK(refused.size() > 1 && refused[1].find("isn't PO") != std::string::npos);
        CHECK(c.by_key["greeting"].msgstr[0] == "Cześć, {name}!" && c.by_key["row.Trees.label"].line == 11);
        // LF only, no BOM, an entry at the very end without a newline
        l10n::Catalogue d;
        refused.clear();
        CHECK(l10n::parse_po("msgctxt \"a\"\nmsgid \"A\"\nmsgstr \"\\u0041x\"", "pl", d, refused) == 1 && d.by_key["a"].msgstr[0] == "u0041x");
    }
    // control characters: a msgstr with one other than \n and \t is left out, so the English shows
    {
        l10n::Catalogue c;
        std::vector<std::string> refused;
        const char* po = "msgctxt \"nul\"\nmsgid \"A\"\nmsgstr \"A\\x01B\\0C\"\n\n"
                         "msgctxt \"bell\"\nmsgid \"Ring\"\nmsgstr \"Dzwo\\a\"\n\n"
                         "msgctxt \"cr\"\nmsgid \"Line\"\nmsgstr \"Linia\\r\"\n\n"
                         "msgctxt \"plural\"\nmsgid \"One\"\nmsgid_plural \"{n}\"\nmsgstr[0] \"Jeden\"\nmsgstr[1] \"{n}\\b\"\nmsgstr[2] \"{n}\"\n\n"
                         "msgctxt \"latin1\"\nmsgid \"Cafe\"\nmsgstr \"Caf\\xE9\"\n\n"
                         "msgctxt \"ok\"\nmsgid \"Tab\\tand line\\n\"\nmsgstr \"Tab\\ti linia\\n\"\n";
        CHECK(l10n::parse_po(po, "pl", c, refused) == 1 && c.by_key.count("ok") && c.by_key["ok"].msgstr[0] == "Tab\ti linia\n");
        CHECK(!c.by_key.count("nul") && !c.by_key.count("bell") && !c.by_key.count("cr") && !c.by_key.count("plural") && !c.by_key.count("latin1"));
        CHECK(refused.size() == 5);
        CHECK(refused.size() == 5 && refused[0] == "line 2, \"nul\": its text has a control character (U+0001), which the game can't show, so it's "
                                                  "left out and English shows there");
        CHECK(refused.size() == 5 && refused[1].find("\"bell\": its text has a control character (U+0007)") != std::string::npos);
        CHECK(refused.size() == 5 && refused[2].find("\"cr\": its text has a control character (U+000D)") != std::string::npos);
        CHECK(refused.size() == 5 && refused[3].find("\"plural\": its text has a control character (U+0008)") != std::string::npos);
        CHECK(refused.size() == 5 && refused[4].find("\"latin1\": its text has an escape that isn't UTF-8 (such as \\xE9), so it's left out") !=
                                         std::string::npos);
    }
    // a file that isn't UTF-8 isn't read at all, with one sentence; nor one saved as UTF-16 (Notepad's "Unicode")
    {
        l10n::Catalogue c;
        std::vector<std::string> refused;
        const char* ansi = "msgctxt \"a\"\nmsgid \"Trees\"\nmsgstr \"Drzewa\"\n\nmsgctxt \"b\"\nmsgid \"Breakable\"\nmsgstr \"\xA3" "amliwe\"\n";
        CHECK(l10n::parse_po(ansi, "pl", c, refused) == 0 && c.by_key.empty());
        CHECK(refused.size() == 1 && refused[0] == "line 7 isn't UTF-8, so nothing in the file is read: save it as UTF-8");
        refused.clear();
        const std::string utf16("\xFF\xFEm\0s\0g\0i\0d\0 \0\"\0\"\0\n\0", 22);
        CHECK(l10n::parse_po(utf16, "pl", c, refused) == 0 && refused.size() == 1 &&
              refused[0] == "it's saved as UTF-16 (Notepad's \"Unicode\"), so nothing in it is read: save it as UTF-8");
        refused.clear();
        CHECK(l10n::parse_po(std::string("\xFE\xFF\0m", 4), "pl", c, refused) == 0 && refused.size() == 1 && refused[0].find("UTF-16") != std::string::npos);
        refused.clear();
        CHECK(l10n::parse_po("msgid \"\"\nmsgstr \"\"\n\"Language: pl\\n\"\n\nmsgctxt \"x\"\nmsgid \"\xF0\x9F\x8F\x81 Go\"\nmsgstr \"\xC5\x81" "ap\"\n", "pl", c,
                             refused) == 1 && refused.empty());   // UTF-8, 4 bytes too
    }
    // the header's nplurals against the platform's rule for the language
    {
        CHECK(l10n::plural_forms("pl") == 3 && l10n::plural_forms("ru") == 3 && l10n::plural_forms("cs") == 3 && l10n::plural_forms("en") == 2 &&
              l10n::plural_forms("pt-BR") == 2 && l10n::plural_forms("ja") == 1 && l10n::plural_forms("zh-Hant") == 1);
        const std::string cldr = "msgid \"\"\nmsgstr \"\"\n\"Language: cs\\n\"\n\"Plural-Forms: nplurals=4; plural=(n==1 ? 0 : n>=2 && n<=4 ? 1 : n%1!=0 ? 2: 3);\\n\"\n\n"
                                 "msgctxt \"k\"\nmsgid \"{n} tree\"\nmsgid_plural \"{n} trees\"\n"
                                 "msgstr[0] \"{n} strom\"\nmsgstr[1] \"{n} stromy\"\nmsgstr[2] \"{n} stromu\"\nmsgstr[3] \"{n} strom\xC5\xAF\"\n";
        l10n::Catalogue c;
        std::vector<std::string> refused;
        CHECK(l10n::parse_po(cldr, "cs", c, refused) == 1 && refused.size() == 1);
        CHECK(refused.size() == 1 && refused[0] == "its header says nplurals=4, but the platform reads 3 plural forms for Czech (msgstr[0] to "
                                                   "msgstr[2], in gettext's usual order for it), so a count may show the wrong form: check its plural texts");
        refused.clear();
        CHECK(l10n::parse_po("#, fuzzy\nmsgid \"\"\nmsgstr \"Plural-Forms: nplurals = 1; plural=0;\\n\"\n", "ja", c, refused) == 0 && refused.empty());
        CHECK(l10n::parse_po("msgid \"\"\nmsgstr \"Plural-Forms: nplurals=2; plural=(n != 1);\\n\"\n", "ja", c, refused) == 0 && refused.size() == 1 &&
              refused[0].find("reads 1 plural form for Japanese (msgstr[0] alone,") != std::string::npos);
        refused.clear();
        CHECK(l10n::parse_po(kPolish, "pl", c, refused) == 6 && refused.size() == 2);   // nplurals=3, as the platform's
    }
    // braces as the game's own text formatting reads them (set_string_tr)
    {
        CHECK(l10n::braces_fit("{0} joined", "{0} do\xC5\x82\xC4\x85" "czy\xC5\x82"));
        CHECK(l10n::braces_fit("{0} joined", "{0} i {0}"));                         // the same placeholder twice is fine
        CHECK(l10n::braces_fit("{0} joined", "do\xC5\x82\xC4\x85" "czono"));        // and none (parse_po checks they're all there)
        CHECK(!l10n::braces_fit("{0} joined", "{0} do\xC5\x82\xC4\x85" "czy\xC5\x82 {"));
        CHECK(!l10n::braces_fit("{0} joined", "{ 0} do\xC5\x82\xC4\x85" "czy\xC5\x82"));
        CHECK(!l10n::braces_fit("{0} joined", "}{0}"));
        CHECK(l10n::braces_fit("{{font_pop}}Ready {0}", "{{font_pop}}Gotowe {0}"));
        CHECK(!l10n::braces_fit("{{font_pop}}Ready {0}", "{font_pop}Gotowe {0}"));   // the token as a placeholder
        CHECK(!l10n::braces_fit("{{font_pop}}Ready {0}", "{{font_pop}Gotowe {0}"));
        CHECK(l10n::braces_fit("Score { of {0}", "Wynik { z {0}"));                  // a brace on its own the English has too
        CHECK(!l10n::braces_fit("Score { of {0}", "Wynik {{ z {0}"));
        CHECK(l10n::braces_fit("Plain", "Zwyk\xC5\x82y"));
    }
    // placeholders
    {
        CHECK((l10n::placeholders("{a} and {b}, {a} {not one} {} {x{y}") == std::vector<std::string>{"{a}", "{b}", "{y}"}));
        l10n::Entry e;
        e.msgid        = "One car in {place}";
        e.msgid_plural = "{n} cars in {place}";
        CHECK(l10n::placeholders_fit(e, "Jeden samochód w {place}"));   // {n} may be left out
        CHECK(l10n::placeholders_fit(e, "{n} samochody w {place}"));
        CHECK(!l10n::placeholders_fit(e, "{n} samochodów"));             // {place} is in both English forms
        CHECK(!l10n::placeholders_fit(e, "{n} samochodów w {miejsce}"));
        CHECK(l10n::placeholders_fit(e, "Jeden samochód", true));        // a form only for 1 may leave {place} out too
        CHECK(!l10n::placeholders_fit(e, "Jeden samochód w {miejsce}", true));
        e.msgid_plural.clear();
        CHECK(!l10n::placeholders_fit(e, "Jeden samochód"));
        CHECK(!l10n::placeholders_fit(e, "Jeden samochód", true));        // not a plural: the same ones
        // which forms are only for 1
        CHECK(l10n::form_only_for_one("pl", 0) && !l10n::form_only_for_one("pl", 1) && !l10n::form_only_for_one("pl", 2));
        CHECK(l10n::form_only_for_one("en", 0) && l10n::form_only_for_one("de", 0) && l10n::form_only_for_one("cs", 0));
        CHECK(!l10n::form_only_for_one("ru", 0) && !l10n::form_only_for_one("pt-BR", 0) && !l10n::form_only_for_one("fr", 0));
        CHECK(!l10n::form_only_for_one("ja", 0) && !l10n::form_only_for_one("en", 1));
        // docs/translating.md's example, "{n} tree broken", with Polish's singular without {n}; not so in Russian,
        // whose first form is also for 21, 31...
        const char* tree = "msgctxt \"count.trees\"\nmsgid \"{n} tree broken\"\nmsgid_plural \"{n} trees broken\"\n"
                           "msgstr[0] \"Jedno drzewo\"\nmsgstr[1] \"{n} drzewa\"\nmsgstr[2] \"{n} drzew\"\n";
        l10n::Catalogue pl, ru;
        std::vector<std::string> refused;
        CHECK(l10n::parse_po(tree, "pl", pl, refused) == 1 && refused.empty() && pl.by_key["count.trees"].msgstr[0] == "Jedno drzewo");
        CHECK(l10n::parse_po(tree, "ru", ru, refused) == 0 && refused.size() == 1);
        // the other forms keep every placeholder both English forms have
        CHECK(l10n::parse_po("msgctxt \"k\"\nmsgid \"{n} tree\"\nmsgid_plural \"{n} trees\"\nmsgstr[0] \"{n} drzewo\"\n"
                             "msgstr[1] \"drzewa\"\nmsgstr[2] \"{n} drzew\"\n", "pl", pl, refused) == 0);
    }
    // tags and fallbacks
    {
        CHECK(std::strcmp(l10n::tag_for_code("PL"), "pl") == 0 && std::strcmp(l10n::tag_for_code("br"), "pt-BR") == 0);
        CHECK(std::strcmp(l10n::tag_for_code("cht"), "zh-Hant") == 0 && std::strcmp(l10n::tag_for_code("GB"), "en-GB") == 0);
        CHECK(std::strcmp(l10n::tag_for_code("MX"), "es-MX") == 0 && std::strcmp(l10n::tag_for_code("CZ"), "cs") == 0);
        CHECK(l10n::tag_for_code("DEV") == nullptr && l10n::tag_for_code("LOC") == nullptr);
        CHECK(l10n::normal_tag("pt_BR") == "pt-BR" && l10n::normal_tag("PT-br") == "pt-BR" && l10n::normal_tag("zh-hant") == "zh-Hant");
        CHECK(l10n::normal_tag("es-419") == "es-419" && l10n::normal_tag("qps") == "qps" && l10n::normal_tag("PL") == "pl");
        CHECK(l10n::normal_tag("x").empty() && l10n::normal_tag("polish").empty() && l10n::normal_tag("pl-").empty() &&
              l10n::normal_tag("../pl").empty());
        CHECK((l10n::fallbacks("pt-BR") == std::vector<std::string>{"pt-BR", "pt"}));
        CHECK((l10n::fallbacks("en-GB") == std::vector<std::string>{"en-GB", "en"}));
        CHECK((l10n::fallbacks("es-MX") == std::vector<std::string>{"es-MX", "es-ES", "es"}));
        CHECK((l10n::fallbacks("pl") == std::vector<std::string>{"pl"}));
        CHECK(l10n::language_name("pt-BR") == "Brazilian Portuguese" && l10n::language_name("xx") == "xx");
        // a catalogue named with the region translation tools add is kept under the game's tag
        for (const char* t : {"pl-PL", "de-DE", "fr-FR", "it-IT", "ja-JP", "ko-KR", "cs-CZ", "hu-HU", "ru-RU", "nl-NL", "en-US"})
            CHECK(l10n::catalogue_tag(l10n::normal_tag(t)) == std::string(t, 2));
        CHECK(l10n::catalogue_tag("zh-TW") == "zh-Hant" && l10n::catalogue_tag("zh-HK") == "zh-Hant" && l10n::catalogue_tag("zh-MO") == "zh-Hant" &&
              l10n::catalogue_tag(l10n::normal_tag("zh_Hant_TW")) == "zh-Hant" && l10n::catalogue_tag("zh-Hant") == "zh-Hant");
        // the tags the game uses stay as they are, and so do others
        for (const char* t : {"pl", "pt-BR", "pt", "es-ES", "es-MX", "es", "en-GB", "en", "zh", "de-AT", "pt-PT", "zh-CN", "es-419", "sv"})
            CHECK(l10n::catalogue_tag(t) == t);
        // which the game's languages look for, and the name to use for those they don't
        for (const char* t : {"en", "en-GB", "pl", "pt-BR", "pt", "de", "fr", "es-ES", "es-MX", "es", "it", "ja", "ko", "zh-Hant", "zh", "cs", "hu",
                              "ru", "nl"})
            CHECK(l10n::never_shown(t).empty());
        CHECK(l10n::never_shown("de-AT") == "none of the game's languages looks for de-AT text (its German is de), so name the file de.po");
        CHECK(l10n::never_shown("es-419").find("so name the file es_ES.po, es_MX.po or es.po") != std::string::npos);
        CHECK(l10n::never_shown("pt-PT").find("so name the file pt_BR.po or pt.po") != std::string::npos);
        CHECK(l10n::never_shown("en-AU").find("so name the file en.po or en_GB.po") != std::string::npos);
        CHECK(l10n::never_shown("zh-CN").find("it has no Simplified Chinese), so name a Traditional Chinese file zh_Hant.po") != std::string::npos);
        CHECK(l10n::never_shown("sv") == "none of the game's languages looks for sv text, as the game has no such language");
        CHECK(!l10n::never_shown("qps").empty() && !l10n::never_shown("pl-PL").empty());   // pl-PL only after catalogue_tag
    }
    // plural forms (CLDR, numbered as gettext's Plural-Forms)
    {
        auto forms = [](const char* tag, std::initializer_list<int64_t> ns) {
            std::string s;
            for (int64_t n : ns) s += std::to_string(l10n::plural_form(tag, n));
            return s;
        };
        CHECK(forms("en", {0, 1, 2, 5, 11, 21, 101}) == "1011111");
        CHECK(forms("en-GB", {1, 2}) == "01");
        CHECK(forms("pl", {0, 1, 2, 3, 4, 5, 11, 12, 14, 21, 22, 25, 101, 102, 112, 122}) == "2011122222122121");
        CHECK(forms("pt-BR", {0, 1, 2, 100}) == "0011" && forms("pt", {0, 1, 2}) == "001");
        CHECK(forms("ru", {0, 1, 2, 4, 5, 11, 12, 21, 22, 25, 101, 111}) == "201122201202");
        CHECK(forms("de", {0, 1, 2}) == "101" && forms("ja", {0, 1, 2, 100}) == "0000");
        CHECK(forms("pl", {-1, -2, -5}) == "012");
    }
    // the first launch's guess, from the game's own table of Windows regional formats
    CHECK(l10n::guess_from_windows(L"pl-PL") == "pl" && l10n::guess_from_windows(L"pl") == "pl");
    CHECK(l10n::guess_from_windows(L"pt-PT") == "pt-BR" && l10n::guess_from_windows(L"pt-BR") == "pt-BR");
    CHECK(l10n::guess_from_windows(L"es-ES") == "es-ES" && l10n::guess_from_windows(L"es") == "es-ES" &&
          l10n::guess_from_windows(L"es-AR") == "es-MX" && l10n::guess_from_windows(L"es-MX") == "es-MX");
    CHECK(l10n::guess_from_windows(L"en-US") == "en" && l10n::guess_from_windows(L"en-CA") == "en" &&
          l10n::guess_from_windows(L"en-GB") == "en-GB" && l10n::guess_from_windows(L"en-IE") == "en-GB");
    CHECK(l10n::guess_from_windows(L"fr-CA") == "fr" && l10n::guess_from_windows(L"de-AT") == "de" &&
          l10n::guess_from_windows(L"ja-JP") == "ja" && l10n::guess_from_windows(L"ru-RU") == "ru");
    CHECK(l10n::guess_from_windows(L"zh-TW") == "zh-Hant" && l10n::guess_from_windows(L"zh-Hant-HK") == "zh-Hant" &&
          l10n::guess_from_windows(L"zh-CN") == "en");
    CHECK(l10n::guess_from_windows(L"ko-KR") == "en" && l10n::guess_from_windows(L"nl-NL") == "en" &&
          l10n::guess_from_windows(L"cs-CZ") == "en" && l10n::guess_from_windows(L"") == "en");
    // numbers and dates
    {
        CHECK(num("en", 1234.5, 1) == "1,234.5" && num("en", 1234567, 0) == "1,234,567" && num("en", 999, 2) == "999.00");
        // Polish and Spain's Spanish group only from five digits (CLDR): 1234,5 but 12 345
        CHECK(num("pl", 1234.5, 1) == "1234,5" && num("pl", 12345, 0) == "12\xC2\xA0" "345" &&
              num("pl", 1234567.891, 2) == "1\xC2\xA0" "234\xC2\xA0" "567,89" && num("pl", -1234, 0) == "-1234");
        CHECK(num("pt-BR", 1234.5, 1) == "1.234,5" && num("de", 1234567, 0) == "1.234.567" && num("es-ES", 1234.5, 1) == "1234,5" &&
              num("es-ES", 12345.5, 1) == "12.345,5" && num("es", 1234, 0) == "1234");
        CHECK(num("es-MX", 1234.5, 1) == "1,234.5" && num("fr", 1234.5, 1) == "1\xC2\xA0" "234,5" && num("ru", 12345, 0) == "12\xC2\xA0" "345");
        CHECK(num("en", -1234.5, 1) == "-1,234.5" && num("pl", -0.04, 1) == "0,0" && num("en", 0.125, 7) == "0.125000");
        CHECK(num("en", INFINITY, 1) == "<none>" && num("en", NAN, 1) == "<none>");
        char small[5];
        CHECK(l10n::format_number("en", 12345, 0, small, sizeof small) == 0 && small[0] == '\0');
        CHECK(l10n::format_number("en", 123, 0, small, sizeof small) == 3 && std::string(small) == "123");
        CHECK(date("pl", 2026, 10, 24) == "24.10.2026" && date("de", 2026, 1, 2) == "02.01.2026" && date("ru", 2026, 10, 24) == "24.10.2026");
        CHECK(date("pt-BR", 2026, 10, 24) == "24/10/2026" && date("fr", 2026, 10, 24) == "24/10/2026" && date("es-MX", 2026, 10, 24) == "24/10/2026");
        CHECK(date("it", 2026, 10, 24) == "24/10/2026" && date("nl", 2026, 10, 24) == "24-10-2026" && date("ja", 2026, 10, 24) == "2026/10/24");
        CHECK(date("en", 2026, 10, 24) == "2026-10-24" && date("en-GB", 2026, 10, 24) == "2026-10-24");
        CHECK(date("en", 2026, 13, 1) == "<none>" && date("en", 0, 1, 1) == "<none>");
    }
    // the test language: letters in the fonts, brackets, placeholders kept, padded by the growth
    {
        const std::string a = l10n::pseudo("Off");
        CHECK(a.front() == '[' && a.back() == ']' && in_letter_set(a));
        const std::string b = l10n::pseudo("Restart now to load {name}, {count} times");
        CHECK(b.find("{name}") != std::string::npos && b.find("{count}") != std::string::npos && in_letter_set(b));
        auto chars = [](const std::string& s) { return MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0); };
        CHECK(chars(l10n::pseudo("Off")) == 6);                                  // 3 x 2
        CHECK(chars(l10n::pseudo("Breakable trees")) == 26);                     // 15 x 1.7, up
        CHECK(chars(l10n::pseudo(std::string(40, 'x'))) == 60);
        CHECK(chars(l10n::pseudo(std::string(100, 'x'))) == 135);
        CHECK(in_letter_set(l10n::pseudo("Zażółć gęślą jaźń: ÁÉÍÓÚ ãõ ç\nNew line")));
        CHECK(l10n::pseudo("") == "[]");
    }

    // ---- the state: the language last time is Polish, then the game picks its own
    write(g_dir / "fh2-mods" / "platform-language.ini", "; written by hand\r\nlast=pl\r\ntest=0\r\n");
    l10n_start();
    CHECK(logged("Language: mods' text starts in Polish (pl), the game's language last time"));
    CHECK(std::strcmp(l10n_language(), "pl") == 0);
    FH2Mod mod;
    mod.name = "TestMod";
    CHECK(l10n_add_translation(&mod, "pl", kPolish, sizeof kPolish - 1) == 6);
    CHECK(logged("[TestMod] translation: Polish (pl), 6 strings"));
    CHECK(logged("[TestMod] translation pl: line 42, \"row.Bad.label\": its placeholders ({liczba}) don't match the English's ({count}), "
                 "so it's left out and English shows there"));
    CHECK(l10n_add_translation(&mod, "Polish", kPolish, 10) == 0 && logged("\"Polish\" isn't a language tag such as pl or pt-BR"));
    const char* trees = l10n_tr(&mod, "row.Trees.label", "Breakable trees");
    CHECK(std::string(trees) == "Łamliwe drzewa");
    CHECK(l10n_tr(&mod, "row.Trees.label", "Breakable trees") == trees);   // the same copy each time
    CHECK(std::string(l10n_tr(&mod, "row.Fuzzy.label", "Not sure yet")) == "Not sure yet");
    CHECK(std::string(l10n_tr(&mod, "row.Bad.label", "{count} unlocked")) == "{count} unlocked");
    CHECK(std::string(l10n_tr(&mod, nullptr, "No context here")) == "Tu bez kontekstu");
    CHECK(std::string(l10n_tr(&mod, "row.Missing.label", "Nobody translated me")) == "Nobody translated me");
    // stale: the English changed since it was translated
    CHECK(std::string(l10n_tr(&mod, "row.Stale.label", "New English")) == "New English");
    CHECK(logged("[TestMod] the pl text for \"row.Stale.label\" was translated from different English, so it isn't used until it's translated again"));
    CHECK(std::string(l10n_tr_plural(&mod, "count.trees", "One tree broken", "{n} trees broken", 1)) == "Jedno drzewo złamane");
    CHECK(std::string(l10n_tr_plural(&mod, "count.trees", "One tree broken", "{n} trees broken", 3)) == "{n} drzewa złamane");
    CHECK(std::string(l10n_tr_plural(&mod, "count.trees", "One tree broken", "{n} trees broken", 25)) == "{n} drzew złamanych");
    CHECK(std::string(l10n_tr_plural(&mod, "count.trees", "One tree broken", "{n} trees changed", 25)) == "{n} trees changed");
    CHECK(std::string(l10n_tr(nullptr, "x", "No mod")) == "No mod" && std::string(l10n_tr(&mod, "x", nullptr)).empty());
    char b[32];
    CHECK(l10n_format_number(1234.5, 1, b, sizeof b) == 6 && std::string(b) == "1234,5");
    CHECK(l10n_format_number(12345.5, 1, b, sizeof b) == 9 && std::string(b) == "12\xC2\xA0" "345,5");
    CHECK(l10n_format_date(2026, 11, 8, b, sizeof b) == 10 && std::string(b) == "08.11.2026");
    // Brazilian Portuguese, with a Portuguese catalogue and a Brazilian one; English for the rest
    CHECK(add(&mod, "pt", "msgctxt \"row.Trees.label\"\nmsgid \"Breakable trees\"\nmsgstr \"Árvores quebráveis\"\n\n"
                                           "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"Olá, {name}!\"\n") == 2);
    CHECK(add(&mod, "pt_BR", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"Oi, {name}!\"\n") == 1);
    CHECK(logged("[TestMod] translation: Brazilian Portuguese (pt-BR), 1 string") && !logged("1 strings"));
    CHECK(add(&mod, "es-ES", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"¡Hola, {name}!\"\n") == 1);
    CHECK(add(&mod, "en", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"Hi, {name}!\"\n") == 1);

    // ---- following: text noted while Polish, shown in the game's language once it has one
    l10n_note(&mod, "row.Trees.label", trees);
    l10n_note(&mod, "row.Other.label", "Breakable trees");                        // not tr()'s text: not followed
    l10n_note_key(&mod, "mod.description", "row.Trees.label", "Breakable trees"); // as FH2ModInfo.description
    const char* hello = l10n_tr(&mod, "greeting", "Hello, {name}!");
    CHECK(std::string(hello) == "Cześć, {name}!");
    l10n_note(&mod, "notice", hello);
    std::wstring title = L"PICK  1/2";
    l10n_note_w(&mod, title.c_str(), trees, L"  1/2");
    std::string s = "old", other = "kept", desc = "?";
    l10n_follow(&mod, "row.Trees.label", s);
    l10n_follow(&mod, "row.Other.label", other);
    l10n_follow(&mod, "mod.description", desc);
    CHECK(s == "Łamliwe drzewa" && other == "kept" && desc == "Łamliwe drzewa");
    // each language's string table has its own text, whatever the language now
    CHECK(l10n_table_text(&mod, "row.Trees.label", L"Breakable trees", "EN") == L"Breakable trees");
    CHECK(l10n_table_text(&mod, "row.Trees.label", L"Breakable trees", "PL") == L"Łamliwe drzewa");
    CHECK(l10n_table_text(&mod, "row.Trees.label", L"Breakable trees", "br") == L"Árvores quebráveis");   // pt-BR -> pt
    CHECK(l10n_table_text(&mod, "row.Trees.label", L"Breakable trees", "DEV") == L"Breakable trees");
    CHECK(l10n_text_in(&mod, "notice", "BR", s) && s == "Oi, {name}!");
    CHECK(l10n_text_in(&mod, "notice", "MX", s) && s == "¡Hola, {name}!");                           // es-MX -> es-ES
    CHECK(l10n_text_in(&mod, "notice", "GB", s) && s == "Hi, {name}!");                              // en-GB -> en
    CHECK(!l10n_text_in(&mod, "row.Other.label", "PL", s));
    // set_string_tr's text: the game formats its string tables itself, so a translation whose braces don't fit the
    // English's goes in as the English, said once per language and key; pages (tr) still get the translation
    CHECK(add(&mod, "pl", "msgctxt \"chat.joined\"\nmsgid \"{0} joined\"\nmsgstr \"{0} dołączył {\"\n\n"
                          "msgctxt \"chat.left\"\nmsgid \"{{font_pop}}{0} left\"\nmsgstr \"{{font_pop}}{0} wyszedł\"\n\n"
                          "msgctxt \"chat.ready\"\nmsgid \"{{font_pop}}Ready\"\nmsgstr \"{font_pop}Gotowe\"\n") == 3);
    CHECK(l10n_table_text(&mod, "chat.joined", L"{0} joined", "PL") == L"{0} joined");
    CHECK(l10n_table_text(&mod, "chat.joined", L"{0} joined", "PL") == L"{0} joined");
    {
        int said = 0;
        for (const std::string& l : g_log)
            said += l == "[TestMod] the pl text for \"chat.joined\" has braces that don't match the English's ({0}, {{...}}), which the game's own "
                         "text formatting could misread, so the PL string table gets the English there";
        CHECK(said == 1);
    }
    CHECK(l10n_table_text(&mod, "chat.left", L"{{font_pop}}{0} left", "PL") == L"{{font_pop}}{0} wyszedł");
    CHECK(l10n_table_text(&mod, "chat.ready", L"{{font_pop}}Ready", "PL") == L"{{font_pop}}Ready");
    CHECK(std::string(l10n_tr(&mod, "chat.joined", "{0} joined")) == "{0} dołączył {");
    // the game picks English: the platform says so, records it, and the noted text follows
    g_game = "EN";
    std::string ini;
    for (int i = 0; i < 50 && ini.find("last=en\r") == std::string::npos; ++i) {
        Sleep(100);
        std::ifstream in{g_dir / "fh2-mods" / "platform-language.ini", std::ios::binary};
        ini.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(ini.find("last=en\r") != std::string::npos && ini.find("test=0") != std::string::npos);
    CHECK(logged("Language: the game's text is in English (en), not Polish as last time; mods' text follows it on every page written from now on"));
    CHECK(std::strcmp(l10n_language(), "en") == 0);
    s = "old";
    l10n_follow(&mod, "row.Trees.label", s);
    l10n_follow(&mod, "mod.description", desc);
    CHECK(s == "Breakable trees" && desc == "Breakable trees");
    CHECK(std::string(trees) == "Łamliwe drzewa");   // what was handed out stays as it was
    const wchar_t* w = l10n_follow_w(title.c_str());
    CHECK(w && std::wstring(w) == L"Breakable trees  1/2");
    CHECK(l10n_follow_w(L"not noted") == nullptr);
    // then Brazilian Portuguese (the game's language read live)
    g_game = "br";
    l10n_follow(&mod, "row.Trees.label", s);
    CHECK(s == "Árvores quebráveis" && std::wstring(l10n_follow_w(title.c_str())) == L"Árvores quebráveis  1/2");
    CHECK(std::string(l10n_tr_plural(&mod, "count.trees", "One tree broken", "{n} trees broken", 0)) == "{n} trees broken");
    // a count shown live: counts that take the same forms in every language share a copy, so there are a handful, not one per n
    CHECK(l10n_tr_plural(&mod, "count.live", "One car", "{n} cars", 2) == l10n_tr_plural(&mod, "count.live", "One car", "{n} cars", 3));
    {
        std::set<const char*> copies;
        for (int64_t n = 0; n < 5000; ++n) copies.insert(l10n_tr_plural(&mod, "count.live", "One car", "{n} cars", n));
        std::printf("tr_plural: %zu copies for 5000 counts\n", copies.size());
        CHECK(copies.size() <= 16);   // one per set of forms across the languages' rules
    }
    CHECK(std::string(l10n_tr_plural(&mod, "count.live", "One car", "{n} cars", -1)) == "One car");   // English, as for 1
    // a title-screen list's title cut as its label was: in characters, as the title screen cuts the label
    std::wstring cut_title = L"Árvor  1/2";
    l10n_note_w(&mod, cut_title.c_str(), trees, L"  1/2", 5);
    CHECK(std::wstring(l10n_follow_w(cut_title.c_str())) == L"Árvor  1/2");   // "Árvores quebráveis" cut to 5 characters, not 5 bytes
    std::wstring cut_one = L"x";
    l10n_note_w(&mod, cut_one.c_str(), trees, L"", 1);
    CHECK(std::wstring(l10n_follow_w(cut_one.c_str())) == L"Á");             // a 2-byte letter is one character
    // Russian, 21 characters in 41 bytes, cut at 32 characters as kMaxTitleLabel: all of it (16 characters when cut in bytes)
    CHECK(add(&mod, "ru", "msgctxt \"title.adv\"\nmsgid \"Advanced settings\"\n"
                          "msgstr \"\xD0\xA0\xD0\xB0\xD1\x81\xD1\x88\xD0\xB8\xD1\x80\xD0\xB5\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5 "
                          "\xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8\"\n") == 1);
    g_game = "RU";
    const char* adv = l10n_tr(&mod, "title.adv", "Advanced settings");
    CHECK(std::strlen(adv) == 41);
    std::wstring adv_title = L"ADVANCED SETTINGS";
    l10n_note_w(&mod, adv_title.c_str(), adv, L"", 32);
    CHECK(std::wstring(l10n_follow_w(adv_title.c_str())) == L"Расширенные "
                                                            L"настройки");
    g_game = "br";
    // other text noted in the same slot replaces it
    l10n_note(&mod, "row.Trees.label", "something of the mod's own");
    s = "own";
    l10n_follow(&mod, "row.Trees.label", s);
    CHECK(s == "own");

    // ---- fh2-mods\lang\<mod>\: a file wins over the DLL's, and adds languages the DLL hasn't got
    write(g_dir / "fh2-mods" / "lang" / "FileMod" / "pt_BR.po", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"E aí, {name}!\"\n");
    write(g_dir / "fh2-mods" / "lang" / "FileMod" / "de.po", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"Hallo, {name}!\"\n");
    write(g_dir / "fh2-mods" / "lang" / "FileMod" / "notes.po", "msgid \"x\"\nmsgstr \"y\"\n");
    FH2Mod file_mod;
    file_mod.name = "FileMod";
    CHECK(add(&file_mod, "pt-BR", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"Oi, {name}!\"\n") == 1);
    CHECK(logged("[FileMod] translation: Brazilian Portuguese (pt-BR) from fh2-mods\\lang\\FileMod\\pt_BR.po, 1 string; it's used instead of "
                 "any the mod has built in"));
    CHECK(logged("[FileMod] add_translation: pt-BR comes from fh2-mods\\lang\\FileMod\\pt_BR.po this time, not from the mod"));
    CHECK(logged("[FileMod] fh2-mods\\lang\\FileMod\\notes.po isn't named after a language (pl.po, pt-BR.po...), so it isn't read"));
    CHECK(std::string(l10n_tr(&file_mod, "greeting", "Hello, {name}!")) == "E aí, {name}!");
    g_game = "DE";
    CHECK(std::string(l10n_tr(&file_mod, "greeting", "Hello, {name}!")) == "Hallo, {name}!");

    // ---- tags the game doesn't use: pl_PL is Polish, ja-JP Japanese, zh_TW Traditional Chinese; es_AR and de_AT are
    // never looked for, so they're refused with the name to use
    write(g_dir / "fh2-mods" / "lang" / "FoldMod" / "zh_TW.po", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"你好，{name}！\"\n");
    write(g_dir / "fh2-mods" / "lang" / "FoldMod" / "de_AT.po", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"Servus, {name}!\"\n");
    FH2Mod fold_mod;
    fold_mod.name = "FoldMod";
    CHECK(add(&fold_mod, "pl_PL", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"Cześć, {name}!\"\n") == 1);
    CHECK(logged("[FoldMod] translation: Polish (pl), 1 string"));
    CHECK(logged("[FoldMod] translation: Traditional Chinese (zh-Hant) from fh2-mods\\lang\\FoldMod\\zh_TW.po, 1 string"));
    CHECK(logged("[FoldMod] fh2-mods\\lang\\FoldMod\\de_AT.po isn't read: none of the game's languages looks for de-AT text (its German is de), "
                 "so name the file de.po"));
    CHECK(add(&fold_mod, "ja-JP", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"こんにちは、{name}！\"\n") == 1);
    CHECK(add(&fold_mod, "es_AR", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"¡Hola, {name}!\"\n") == 0);
    CHECK(logged("[FoldMod] add_translation: nothing was added for es-AR: none of the game's languages looks for es-AR text (its Spanish is es-ES, "
                 "for Spain, or es-MX, for Latin America), so name the file es_ES.po, es_MX.po or es.po"));
    g_game = "PL";
    CHECK(std::string(l10n_tr(&fold_mod, "greeting", "Hello, {name}!")) == "Cześć, {name}!");
    g_game = "JP";
    CHECK(std::string(l10n_tr(&fold_mod, "greeting", "Hello, {name}!")) == "こんにちは、{name}！");
    g_game = "CHT";
    CHECK(std::string(l10n_tr(&fold_mod, "greeting", "Hello, {name}!")) == "你好，{name}！");
    g_game = "MX";
    CHECK(std::string(l10n_tr(&fold_mod, "greeting", "Hello, {name}!")) == "Hello, {name}!");
    // two files for one language: the one named for the language itself is read, whichever the folder lists first
    static FH2Mod two_mods[2];   // each its own: the platform knows mods by their address
    for (int k = 0; k < 2; ++k) {
        const std::string other_name = k ? "pl_PL.po" : "pl-PL.po", who = "TwoMod" + std::to_string(k);
        write(g_dir / "fh2-mods" / "lang" / who / "pl.po", "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"Cześć, {name}!\"\n");
        write(g_dir / "fh2-mods" / "lang" / who / other_name, "msgctxt \"greeting\"\nmsgid \"Hello, {name}!\"\nmsgstr \"Hej, {name}!\"\n");
        two_mods[k].name = who;
        g_game           = "PL";
        CHECK(std::string(l10n_tr(&two_mods[k], "greeting", "Hello, {name}!")) == "Cześć, {name}!");
        CHECK(logged("[" + who + "] fh2-mods\\lang\\" + who + "\\" + other_name + " is Polish (pl) too, and pl.po is the one used, so it isn't read"));
        // and the one not used is never logged as read, whichever the folder lists first (NTFS puts pl-PL.po before pl.po)
        CHECK(!logged("[" + who + "] translation: Polish (pl) from fh2-mods\\lang\\" + who + "\\" + other_name));
    }

    // ---- a mod that passes ever-changing English: past 20000 texts, tr() makes no more copies and gives the English
    // back as it was passed, as before API 10; what it had already handed out still works
    {
        FH2Mod many;
        many.name         = "ManyMod";
        const char* first = l10n_tr(&many, "speed", "Speed 0");
        char buf[32];
        const char* got = nullptr;
        for (int i = 1; i < 20000; ++i) {
            std::snprintf(buf, sizeof buf, "Speed %d", i);
            got = l10n_tr(&many, "speed", buf);
        }
        CHECK(got != buf && std::string(got) == "Speed 19999");   // the 20000th is still the platform's copy
        CHECK(logged("[ManyMod] tr has handed out 20000 different texts, and the platform keeps each one for the session, so it stops there: "
                     "from now on, English it hasn't been given before comes back as it is, untranslated and not copied, as before API 10. "
                     "Pass the same English each time, and fill numbers and names in afterwards"));
        std::snprintf(buf, sizeof buf, "Speed %d", 20000);
        CHECK(l10n_tr(&many, "speed", buf) == buf);
        std::snprintf(buf, sizeof buf, "Speed %d", 20001);
        CHECK(l10n_tr(&many, "speed", buf) == buf);
        CHECK(l10n_tr(&many, "speed", "Speed 0") == first);       // one it had already is still its copy
        static const char one[] = "One lap", other[] = "{n} laps";
        CHECK(l10n_tr_plural(&many, "laps", one, other, 3) == other && l10n_tr_plural(&many, "laps", one, other, 1) == one);
        CHECK(l10n_tr_plural(&many, "laps", one, nullptr, 3) == one && std::strcmp(l10n_tr(&many, "x", nullptr), "") == 0);
        CHECK(std::string(l10n_tr(&mod, "greeting", "Hello, {name}!")) == "Cześć, {name}!");   // other mods aren't affected
    }

    // ---- the test language, and a first launch, each in a session of its own
    CHECK(run_phase(L"qps") == 0);
    CHECK(run_phase(L"first") == 0);
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails;
}
