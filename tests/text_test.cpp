// The platform's text in any language (src/text.cpp), with no game: capitals, lengths in characters,
// sentences put together, and messages kept until the game has its language. Exits 0 when every check passes.
#include "text.hpp"

#include <cstdio>
#include <string>
#include <vector>

// A stand-in for lang.cpp's tr(), so shown() can be checked with no game: three templates come back marked, the way
// a translation would, and any other text as it is, as lang.cpp's tr() does for text its table hasn't got.
std::string fh2platform::tr(const char* english) {
    const std::string e = english ? english : "";
    if (e == "{0}, {1} and {2} more") return "<{0}|{1}|+{2}>";
    if (e == "Its SQL adds rows by Ids ({0}); from {1} up.") return "[SQL from {1}: {0}]";
    if (e == "{0} asks for {1}; it's {1} elsewhere") return "{1}, {1} ({0})";
    return e;
}

// And for tr_in(), the title screen's: two templates and one line marked in "PL", English for any other.
std::string fh2platform::tr_in(const char* english, std::string_view code) {
    const std::string e = english ? english : "";
    if (code != "PL") return e;
    if (e == "{0}, {1} and {2} more") return "pl<{0}|{1}|+{2}>";
    if (e == "Its SQL adds rows by Ids ({0}); from {1} up.") return "pl[{1}: {0}]";
    if (e == "Made for offline play.") return "pl: Made for offline play.";
    return e;
}

using namespace fh2platform;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

void same(const std::string& got, const std::string& want, const char* what) {
    check(got == want, what);
    if (got != want) std::printf("     got  [%s]\n     want [%s]\n", got.c_str(), want.c_str());
}

} // namespace

int main() {
    // capitals: every letter, not only a-z
    same(upper_text("Achievements", "EN"), "ACHIEVEMENTS", "English as before");
    same(upper_text("Osi\xC4\x85gni\xC4\x99" "cia", "PL"), "OSI\xC4\x84GNI\xC4\x98" "CIA", "Polish: Osiągnięcia -> OSIĄGNIĘCIA");
    same(upper_text("\xC5\xBC\xC3\xB3\xC5\x82w", "PL"), "\xC5\xBB\xC3\x93\xC5\x81W", "Polish: żółw -> ŻÓŁW");
    same(upper_text("Conquistas e configura\xC3\xA7\xC3\xB5" "es", "BR"), "CONQUISTAS E CONFIGURA\xC3\x87\xC3\x95" "ES",
         "Brazilian Portuguese: ç and õ");
    same(upper_text("\xD0\x94\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F", "RU"),
         "\xD0\x94\xD0\x9E\xD0\xA1\xD0\xA2\xD0\x98\xD0\x96\xD0\x95\xD0\x9D\xD0\x98\xD0\xAF", "Russian: Достижения -> ДОСТИЖЕНИЯ");
    same(upper_text("Stra\xC3\x9F" "e", "DE"), "STRA\xC3\x9F" "E", "German: Windows keeps ß as it is (no SS), the length stays");
    same(upper_text("\xE5\xAE\x9F\xE7\xB8\xBE abc", "JP"), "\xE5\xAE\x9F\xE7\xB8\xBE ABC", "Japanese: kanji stay, the Latin letters go up");
    same(upper_text("caf\xC3\xA9", nullptr), "CAF\xC3\x89", "no language yet: the invariant capitals, still beyond a-z");
    same(upper_text("caf\xC3\xA9", "XX"), "CAF\xC3\x89", "a code the game doesn't have: invariant too");
    same(upper_text("Caf\xE9 & <x>", "FR"), "CAF\xE9 & <X>", "not UTF-8 (Windows-1252 from a mod): only a-z, so it looks as before");
    same(upper_text("", "PL"), "", "empty stays empty");
    same(upper_text("\xF0\x9F\x8F\x81 go", "EN"), "\xF0\x9F\x8F\x81 GO", "a character beyond U+FFFF survives");
    check(locale_of("PL") == L"pl-PL" && locale_of("BR") == L"pt-BR" && locale_of("CHT") == L"zh-TW" && locale_of("MX") == L"es-MX",
          "the game's codes name their locales");
    check(locale_of("XX").empty() && locale_of(nullptr).empty(), "an unknown code has none");

    // lengths in characters (UTF-16 units), not bytes
    check(text_chars("Drive the test garage") == 21, "ASCII: one a byte");
    check(text_chars("\xC5\xBC\xC3\xB3\xC5\x82w") == 4, "żółw is 4 characters (7 bytes)");
    check(text_chars("\xE5\xAE\x9F\xE7\xB8\xBE") == 2, "two kanji are 2 characters (6 bytes)");
    check(text_chars("\xF0\x9F\x8F\x81") == 2, "beyond U+FFFF: 2 (a surrogate pair, as the game holds it)");
    check(text_chars("Caf\xE9") == 4, "a byte that isn't UTF-8 counts as one");
    same(cut_text("\xC5\xBC\xC3\xB3\xC5\x82w", 3), "\xC5\xBC\xC3\xB3\xC5\x82", "cut to 3 characters: whole letters");
    same(cut_text("ab\xF0\x9F\x8F\x81", 3), "ab", "never half a surrogate pair");
    same(cut_text("abc", 10), "abc", "short text stays as it is");
    same(cut_text("Caf\xE9 x", 4), "Caf\xE9", "bytes that aren't UTF-8 are cut one at a time");
    {
        // 40 Polish characters, 47 bytes: the old 32-byte cut left 27 characters, now 32
        const std::string label = "\xC5\xBB\xC3\xB3\xC5\x82ta trasa z bardzo d\xC5\x82ug\xC4\x85 nazw\xC4\x85 testow\xC4\x85";
        check(text_chars(label) == 40 && label.size() == 47, "the Polish label: 40 characters, 47 bytes");
        same(cut_text(label, 32), "\xC5\xBB\xC3\xB3\xC5\x82ta trasa z bardzo d\xC5\x82ug\xC4\x85 nazw\xC4\x85",
             "cut at 32 characters, not 32 bytes");
    }

    // sentences one after the other
    same(join_sentences("One.", "Two."), "One. Two.", "a space between sentences");
    same(join_sentences("", "Two."), "Two.", "nothing before: no space");
    same(join_sentences("One.", ""), "One.", "nothing after: no space");
    same(join_sentences("\xE3\x81\x82\xE3\x80\x82", "\xE3\x81\x84\xE3\x80\x82"), "\xE3\x81\x82\xE3\x80\x82\xE3\x81\x84\xE3\x80\x82",
         "after a Japanese full stop: no space");
    same(join_sentences("\xE4\xB8\x8D\xEF\xBC\x81", "x"), "\xE4\xB8\x8D\xEF\xBC\x81x", "after a full-width ! : no space");

    // placeholders
    same(fill("Version {0} ({1} is out)", {"1.2", "1.3"}), "Version 1.2 (1.3 is out)", "{0} and {1}");
    same(fill("{1} \xE2\x80\x94 {0}", {"a", "b"}), "b \xE2\x80\x94 a", "in another order");
    same(fill("{0} asks for {1}; it's {1} elsewhere", {"car", "4000"}), "car asks for 4000; it's 4000 elsewhere", "one used twice");
    same(fill("{car:{0}}", {"PACK_A"}), "{car:PACK_A}", "other braces are left alone");
    same(fill("{0}", {"{1}", "x"}), "{1}", "an argument holding {1} isn't filled again");
    same(fill("{2} {x} {", {"a"}), "{2} {x} {", "a number with no argument, or no number, stays");

    // messages kept until the game has its language
    {
        const std::string m = later("Windows couldn't load it (error {0}).", {"126"});
        std::string t;
        std::vector<std::string> args;
        check(split_later(m, t, args) && t == "Windows couldn't load it (error {0})." && args == std::vector<std::string>{"126"},
              "a message keeps its English template and its arguments");
        same(english(m), "Windows couldn't load it (error 126).", "and is English in the log");
        check(!split_later("Made for offline play.", t, args), "a mod's own text isn't a message");
        same(english("Made for offline play."), "Made for offline play.", "and stays as it is");
        same(english(later("No arguments.")), "No arguments.", "a message with no arguments");
        const std::string inner = later("{0}, {1} and {2} more", {"Data_Car 3000", "Data_Engine 3000", "2"});
        const std::string outer = later("Its SQL adds rows by Ids ({0}); from {1} up.", {inner, "9000"});
        check(split_later(outer, t, args) && args.size() == 2 && args[0] == inner, "a message as an argument comes back whole");
        same(english(outer), "Its SQL adds rows by Ids (Data_Car 3000, Data_Engine 3000 and 2 more); from 9000 up.",
             "and is filled in too");
        const std::string odd = later("{0}", {std::string("a\x1F" "b\x1E" "c")});
        same(english(odd), "abc", "stray marks in an argument are dropped, so they can't split it");

        // a message with a message among its arguments, as an argument: two levels deep, kept as English text
        const std::string two   = later("Problem: {0}", {outer});
        check(split_later(two, t, args) && args.size() == 1 && args[0].find_first_of("\x1C\x1D\x1E\x1F") == std::string::npos,
              "two levels deep: the argument is plain text, with no marks left");
        same(english(two), "Problem: Its SQL adds rows by Ids (Data_Car 3000, Data_Engine 3000 and 2 more); from 9000 up.",
             "and reads as its English sentence, filled in");
        const std::string three = later("{0}", {later("{0}", {later("{0}, {1} and {2} more", {"x", "y", "2"})})});
        same(english(three), "x, y and 2 more", "three levels: no template or placeholder left over");
    }

    // messages shown in the game's language (with the stand-in tr() above)
    {
        const std::string inner = later("{0}, {1} and {2} more", {"Data_Car 3000", "Data_Engine 3000", "2"});
        const std::string outer = later("Its SQL adds rows by Ids ({0}); from {1} up.", {inner, "9000"});
        same(shown(outer), "[SQL from 9000: <Data_Car 3000|Data_Engine 3000|+2>]",
             "the message and the message in it are both translated, every placeholder filled");
        same(shown(inner), "<Data_Car 3000|Data_Engine 3000|+2>", "a message on its own");
        same(shown(later("Not in the table: {0}.", {"x"})), "Not in the table: x.", "a template with no translation: English, filled in");
        same(shown(later("Its SQL adds rows by Ids ({0}); from {1} up.", {later("Not in the table: {0}", {"x"}), "1"})),
             "[SQL from 1: Not in the table: x]", "an inner message with no translation, in a translated one");
        same(shown(later("{0} asks for {1}; it's {1} elsewhere", {"car", "4000"})), "4000, 4000 (car)",
             "a translation that uses a placeholder twice gets it both times");
        same(shown(later("{0} asks for {1}; it's {1} elsewhere", {"{1}", "x"})), "x, x ({1})",
             "an argument holding {1} isn't filled again");
        same(shown("Made for offline play."), "Made for offline play.", "a mod's own text is shown as it is");
        same(shown(later("Problem: {0}", {outer})),
             "Problem: Its SQL adds rows by Ids (Data_Car 3000, Data_Engine 3000 and 2 more); from 9000 up.",
             "two levels deep: the argument is shown as its English sentence");
    }
    {
        // the title screen's lines in one table's language (shown_in, with the stand-in tr_in() above): a message and the
        // message in it translated, a plain argument (a pack's name) kept as it is, a plain line looked up as before
        const std::string inner = later("{0}, {1} and {2} more", {"Data_Car 3000", "Data_Engine 3000", "2"});
        const std::string outer = later("Its SQL adds rows by Ids ({0}); from {1} up.", {inner, "9000"});
        same(shown_in(outer, "PL"), "pl[9000: pl<Data_Car 3000|Data_Engine 3000|+2>]", "a message in Polish, with the one in it");
        same(shown_in(outer, "EN"), "Its SQL adds rows by Ids (Data_Car 3000, Data_Engine 3000 and 2 more); from 9000 up.",
             "the same in English");
        same(shown_in(later("Its SQL adds rows by Ids ({0}); from {1} up.", {"Made for offline play.", "1"}), "PL"),
             "pl[1: Made for offline play.]", "a plain argument isn't looked up");
        same(shown_in("Made for offline play.", "PL"), "pl: Made for offline play.", "a plain line is, as before");
    }

    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
