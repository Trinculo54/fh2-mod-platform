// Mods' text in the platform's XAML pages (src/xml_text.cpp), with no game. Exits 0 when every check passes.
#include "xml_text.hpp"

#include <cstdio>
#include <string>

using fh2platform::xml_escape;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

void same(const std::string& in, const std::string& want, const char* what) {
    const std::string got = xml_escape(in);
    check(got == want, what);
    if (got != want) std::printf("     got  [%s]\n     want [%s]\n", got.c_str(), want.c_str());
}

} // namespace

int main() {
    // ASCII as before
    same("Breakable trees", "Breakable trees", "plain ASCII is left as it is");
    same("A & B <C> \"D\"", "A &amp; B &lt;C&gt; &quot;D&quot;", "& < > and \" are escaped");
    same("line one\nline two", "line one line two", "a line break becomes a space");
    same("", "", "empty text stays empty");

    // the game reads a value that starts with { and ends with } as a markup extension, and one it doesn't know
    // stops the page loading: a no-break space on the end keeps it text (the game has no "{}" escape)
    same("{0}", "{0}&#160;", "a placeholder left unfilled stays text");
    same("{local:Loc ScreenTitles.IDS_MyProfile}", "{local:Loc ScreenTitles.IDS_MyProfile}&#160;", "a mod's text can't be markup");
    same("{}", "{}&#160;", "{} alone stays text too");
    same("{0} joined", "{0} joined", "a { at the start alone isn't markup to the game, so it's left as it is");
    same("Score {0}", "Score {0}", "nor is a } at the end alone");
    same("{0}\n", "{0} ", "nor a value that ends in a line break (a space here)");
    same("{x} & {y}", "{x} &amp; {y}&#160;", "escaped as before, then the space");

    // UTF-8, as fh2mod.h says the text is
    same("Circle \xC2\xB7 closer", "Circle &#183; closer", "a middle dot (2 bytes) is one reference, not \"\xC3\x82\xC2\xB7\"");
    same("Pr\xC3\xA9" "f\xC3\xA9r\xC3\xAA" "ncias", "Pr&#233;f&#233;r&#234;ncias", "accented letters (Brazilian Portuguese)");
    same("Zak\xC5\x82" "adki \xC5\xBC\xC3\xB3\xC5\x82w", "Zak&#322;adki &#380;&#243;&#322;w", "Polish letters outside Latin-1");
    same("\xE2\x82\xAC" "5 \xE2\x80\x93 \xE2\x80\x9Cok\xE2\x80\x9D", "&#8364;5 &#8211; &#8220;ok&#8221;", "3-byte characters: euro, dash, quotes");
    same("\xF0\x9F\x8F\x81", "&#127937;", "a 4-byte character is one reference");

    // Not UTF-8: read as Windows-1252, which is what the game showed for those bytes before
    same("Caf\xE9", "Caf&#233;", "a lone Latin-1 byte (\xE9 as Windows-1252) stays an e-acute");
    same("\x80 \x93x\x94", "&#8364; &#8220;x&#8221;", "Windows-1252's 0x80-0x9F map to their letters");
    same("\x81\x8D\x8F\x90\x9D", "&#63;&#63;&#63;&#63;&#63;", "the five bytes Windows-1252 leaves out become ?");
    same("\xC2", "&#194;", "a sequence cut off at the end falls back to Windows-1252");
    same("\xC3(", "&#195;(", "a lead byte without its continuation falls back, and the next byte is kept");
    same("\xC0\xAF", "&#192;&#175;", "an overlong form isn't decoded (no hidden ASCII)");
    same("\xED\xA0\x80", "&#237;&#160;&#8364;", "a UTF-16 surrogate isn't a character (its bytes fall back, 0x80 is the euro)");
    same("\xF4\x90\x80\x80", "&#244;&#63;&#8364;&#8364;", "past U+10FFFF isn't a character (0x90 falls back to ?, 0x80 to the euro)");

    // the escaped text never holds a byte the game would read as something else
    {
        std::string all;
        for (int b = 1; b < 256; ++b) all += static_cast<char>(b);
        const std::string got = xml_escape(all);
        bool ascii = true;
        for (unsigned char c : got) ascii = ascii && c < 0x80;
        check(ascii, "every byte value comes out as ASCII");
        check(got.find("<") == std::string::npos && got.find(">") == std::string::npos && got.find('"') == std::string::npos,
              "no < > or \" survives");
    }

    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
