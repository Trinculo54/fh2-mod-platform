// Mods' text in the platform's XAML pages (src/xml_text.cpp), with no game. Exits 0 when every check passes.
#include "xml_text.hpp"

#include <cstdint>
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

std::string utf8(std::uint32_t cp) {
    std::string s;
    if (cp < 0x800) s += static_cast<char>(0xC0 | cp >> 6), s += static_cast<char>(0x80 | (cp & 0x3F));
    else s += static_cast<char>(0xE0 | cp >> 12), s += static_cast<char>(0x80 | (cp >> 6 & 0x3F)), s += static_cast<char>(0x80 | (cp & 0x3F));
    return s;
}

// An attribute value as the game's XAML reader decodes it, from its code (NextChar game+0x152F960, DecodeEntity
// game+0x153BEB0): & to ; is one of the five named entities, or "&#x" and hex digits of either case, or else a space.
// Any other byte is its own value (the platform writes no BOM, so nothing is read as UTF-8).
std::u32string game_reads(const std::string& s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += static_cast<unsigned char>(s[i]);
            continue;
        }
        const std::size_t end = s.find(';', i);
        const std::string e   = s.substr(i, end == std::string::npos ? end : end - i + 1);
        i = end == std::string::npos ? s.size() : end;
        char32_t cp = ' ';
        if (e == "&amp;") cp = '&';
        else if (e == "&apos;") cp = '\'';
        else if (e == "&quot;") cp = '"';
        else if (e == "&gt;") cp = '>';
        else if (e == "&lt;") cp = '<';
        else if (e.size() > 3 && e[1] == '#' && e[2] == 'x') {
            cp = 0;
            for (std::size_t k = 3; k + 1 < e.size(); ++k) {
                const char c = e[k];
                cp = cp << 4 | static_cast<char32_t>(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
            }
        }
        out += cp;
    }
    return out;
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
    same("{0}", "{0}&#xa0;", "a placeholder left unfilled stays text");
    same("{local:Loc ScreenTitles.IDS_MyProfile}", "{local:Loc ScreenTitles.IDS_MyProfile}&#xa0;", "a mod's text can't be markup");
    same("{}", "{}&#xa0;", "{} alone stays text too");
    same("{0} joined", "{0} joined", "a { at the start alone isn't markup to the game, so it's left as it is");
    same("Score {0}", "Score {0}", "nor is a } at the end alone");
    same("{0}\n", "{0} ", "nor a value that ends in a line break (a space here)");
    same("{x} & {y}", "{x} &amp; {y}&#xa0;", "escaped as before, then the space");

    // UTF-8, as fh2mod.h says the text is
    same("Circle \xC2\xB7 closer", "Circle &#xb7; closer", "a middle dot (2 bytes) is one reference, not \"\xC3\x82\xC2\xB7\"");
    same("Pr\xC3\xA9" "f\xC3\xA9r\xC3\xAA" "ncias", "Pr&#xe9;f&#xe9;r&#xea;ncias", "accented letters (Brazilian Portuguese)");
    same("Zak\xC5\x82" "adki \xC5\xBC\xC3\xB3\xC5\x82w", "Zak&#x142;adki &#x17c;&#xf3;&#x142;w", "Polish letters outside Latin-1");
    same("\xE2\x82\xAC" "5 \xE2\x80\x93 \xE2\x80\x9Cok\xE2\x80\x9D", "&#x20ac;5 &#x2013; &#x201c;ok&#x201d;", "3-byte characters: euro, dash, quotes");
    // beyond U+FFFF (emoji): the game's fonts have none, and a reference past 16 bits may not load, so a ?
    same("\xF0\x9F\x8F\x81", "?", "a 4-byte character (U+1F3C1) is a ?");
    same("\xF0\x9F\x9A\x97 Breaks trees", "? Breaks trees", "an emoji in a description is a ?, and the rest stays");
    same("\xF0\x90\x80\x80 \xF4\x8F\xBF\xBF", "? ?", "U+10000 and U+10FFFF, the ends of the range, are ? too");
    same("\xEF\xBF\xBE\xEF\xBF\xBF", "??", "U+FFFE and U+FFFF aren't characters: ?");
    same("\xEF\xBF\xBD \xEF\xBF\xBC", "&#xfffd; &#xfffc;", "U+FFFD and U+FFFC are characters, kept as references");

    // control characters: XML has none but tab, LF and CR, not even as references
    same("a\tb", "a b", "a tab becomes a space");
    same("A\x01" "B", "AB", "U+0001 is left out");
    same(std::string("A\x01" "B\0C", 5), "ABC", "a NUL is left out, and the text after it kept");
    same("\x1B[1mBold\x1F", "[1mBold", "escape and unit separator are left out");
    same("a\rb", "a\rb", "a CR is written as it is, as XML allows and 2.8.0 did");
    same("{0}\x01", "{0}&#xa0;", "a control character after the braces doesn't hide them: still kept as text");
    same("\x02{0}", "{0}&#xa0;", "nor one before them");

    // Not UTF-8: read as Windows-1252, which is what the game showed for those bytes before
    same("Caf\xE9", "Caf&#xe9;", "a lone Latin-1 byte (\xE9 as Windows-1252) stays an e-acute");
    same("\x80 \x93x\x94", "&#x20ac; &#x201c;x&#x201d;", "Windows-1252's 0x80-0x9F map to their letters");
    same("\x81\x8D\x8F\x90\x9D", "?????", "the five bytes Windows-1252 leaves out become ?");
    same("\xC2", "&#xc2;", "a sequence cut off at the end falls back to Windows-1252");
    same("\xC3(", "&#xc3;(", "a lead byte without its continuation falls back, and the next byte is kept");
    same("\xC0\xAF", "&#xc0;&#xaf;", "an overlong form isn't decoded (no hidden ASCII)");
    same("\xED\xA0\x80", "&#xed;&#xa0;&#x20ac;", "a UTF-16 surrogate isn't a character (its bytes fall back, 0x80 is the euro)");
    same("\xF4\x90\x80\x80", "&#xf4;?&#x20ac;&#x20ac;", "past U+10FFFF isn't a character (0x90 falls back to ?, 0x80 to the euro)");

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
        bool controls = false;
        for (unsigned char c : got) controls = controls || (c < 0x20 && c != '\r');
        check(!controls, "no control character but CR survives");
        check(got.find("&#x1;") == std::string::npos && got.find("&#x0;") == std::string::npos, "nor one as a reference");
    }
    // every code point of the 16-bit range that XML allows is one reference; nothing past it is
    {
        bool ok = true;
        for (std::uint32_t cp : {0xA0u, 0x7FFu, 0x800u, 0xD7FFu, 0xE000u, 0xFFFDu}) {
            char ref[16];
            std::snprintf(ref, sizeof ref, "&#x%x;", cp);
            ok = ok && xml_escape(utf8(cp)) == ref;
        }
        check(ok, "U+00A0 to U+FFFD stay references (the ends of each length, around the surrogates)");
        check(xml_escape("a" + utf8(0x80) + utf8(0x9F) + "b") == "ab", "U+0080 to U+009F are control characters: left out");
    }
    // What the game makes of it: its reader decodes hex references with a lowercase x and the five named ones, and
    // turns every other reference into a space (2.9.0 rc1 wrote decimal ones, so every letter beyond ASCII was a space).
    {
        const std::u32string samples[] = {
            U"Wybór testowy Śńęź ŻÓŁTA",                // Polish
            U"Příliš žluťoučký kůň",              // Czech
            U"Árvíztűrő tükörfúrógép",            // Hungarian
            U"Привет Ёж",                              // Russian
            U"設定 カーパック",                                    // Japanese
            U"A & B <C> \"D\" · €5 – {0}",
        };
        bool round = true, form = true;
        for (const auto& text : samples) {
            std::string in;
            for (char32_t cp : text) in += cp < 0x80 ? std::string(1, static_cast<char>(cp)) : utf8(cp);
            const std::string out = xml_escape(in);
            round = round && game_reads(out) == text;
            for (std::size_t i = 0; i + 2 < out.size(); ++i)
                form = form && !(out[i] == '&' && out[i + 1] == '#' && out[i + 2] != 'x');
        }
        check(round, "the game's reader gets back the text it was given (Polish, Czech, Hungarian, Russian, Japanese)");
        check(form, "and no reference is decimal or has a capital X");
        check(game_reads("{0}&#xa0;") == U"{0} " && game_reads("&#243;") == U" ", "the model: hex is read, decimal is a space");
    }

    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
