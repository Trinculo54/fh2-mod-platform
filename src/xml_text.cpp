#include "xml_text.hpp"

#include <cstdint>

namespace fh2platform {
namespace {

// Windows-1252's 0x80-0x9F (the rest of 0xA0-0xFF is Latin-1, the same code points). The five bytes it
// leaves out are shown as "?".
constexpr std::uint16_t kCp1252High[32] = {
    0x20AC, '?',    0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, '?',    0x017D, '?',    '?',    0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, '?',    0x017E, 0x0178,
};

std::uint32_t from_cp1252(unsigned char b) { return b >= 0x80 && b < 0xA0 ? kCp1252High[b - 0x80] : b; }

// The code point of the UTF-8 sequence at s[i] and its length, or 0 when it isn't one (a stray
// continuation byte, a cut-off sequence, an overlong form, a surrogate or past U+10FFFF).
std::size_t utf8_at(const std::string& s, std::size_t i, std::uint32_t& cp) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[i + k]); };
    const unsigned char c = b(0);
    std::size_t n;
    std::uint32_t min;
    if (c >= 0xC2 && c <= 0xDF) n = 2, cp = c & 0x1F, min = 0x80;
    else if (c >= 0xE0 && c <= 0xEF) n = 3, cp = c & 0x0F, min = 0x800;
    else if (c >= 0xF0 && c <= 0xF4) n = 4, cp = c & 0x07, min = 0x10000;
    else return 0;
    if (i + n > s.size()) return 0;
    for (std::size_t k = 1; k < n; ++k) {
        if ((b(k) & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (b(k) & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

} // namespace

std::string xml_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '&': o += "&amp;"; break;
                case '<': o += "&lt;"; break;
                case '>': o += "&gt;"; break;
                case '"': o += "&quot;"; break;
                case '\n': o += ' '; break;   // FZTextBlock doesn't break lines (not even at &#10;)
                default: o += static_cast<char>(c);
            }
            ++i;
            continue;
        }
        std::uint32_t cp = 0;
        std::size_t n = utf8_at(s, i, cp);
        if (!n) cp = from_cp1252(c), n = 1;
        o += "&#" + std::to_string(cp) + ";";
        i += n;
    }
    // The game's XAML reader takes a value that starts with { and ends with } as a markup extension
    // (the two property setters at game+0x156B1F0 and game+0x156C080 check exactly that, with no trimming, then
    // call the parser at game+0x1558A40), and one
    // it doesn't know, such as "{0}" from a pop-up whose placeholder was never filled, stops the page from
    // loading. It has no "{}" escape (WPF's), so a no-break space goes on the end instead: every font the game
    // picks has one, and it can't be seen.
    if (s.size() >= 2 && s.front() == '{' && s.back() == '}') o += "&#160;";
    return o;
}

} // namespace fh2platform
