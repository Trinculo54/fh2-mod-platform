#include "text.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace fh2platform {
namespace {

// The game's text languages (the zips in media\Stripped\StringTables) and their Windows locales. Czech and
// Hungarian show English text, but capitals the Czech or Hungarian way do no harm to it.
struct Locale {
    const char* code;
    const wchar_t* name;
};
const Locale kLocales[] = {{"EN", L"en-US"}, {"GB", L"en-GB"}, {"BR", L"pt-BR"}, {"DE", L"de-DE"}, {"FR", L"fr-FR"},
                           {"ES", L"es-ES"}, {"MX", L"es-MX"}, {"IT", L"it-IT"}, {"JP", L"ja-JP"}, {"KO", L"ko-KR"},
                           {"CHT", L"zh-TW"}, {"PL", L"pl-PL"}, {"RU", L"ru-RU"}, {"NL", L"nl-NL"}, {"CZ", L"cs-CZ"},
                           {"HU", L"hu-HU"}};

// The UTF-8 sequence at s[i]: its length, and in `units` how many UTF-16 units it is; 0 when it isn't one
// (a stray continuation byte, a cut-off sequence, an overlong form, a surrogate or past U+10FFFF).
std::size_t utf8_at(const std::string& s, std::size_t i, std::size_t& units) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    units = 1;
    if (c < 0x80) return 1;
    std::size_t n = 0;
    std::uint32_t cp = 0, least = 0;
    if ((c & 0xE0) == 0xC0) n = 2, cp = c & 0x1F, least = 0x80;
    else if ((c & 0xF0) == 0xE0) n = 3, cp = c & 0x0F, least = 0x800;
    else if ((c & 0xF8) == 0xF0) n = 4, cp = c & 0x07, least = 0x10000;
    else return 0;
    if (i + n > s.size()) return 0;
    for (std::size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    units = cp >= 0x10000 ? 2 : 1;
    return n;
}

bool is_utf8(const std::string& s) {
    std::size_t units = 0;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = utf8_at(s, i, units);
        if (!n) return false;
        i += n;
    }
    return true;
}

std::string ascii_upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// later()'s marks: a message starts with kMessage, and each argument with kArg. In an argument that is a
// message itself they're kInnerMessage and kInnerArg, so they don't split the outer one.
constexpr char kMessage = '\x1E', kArg = '\x1F', kInnerMessage = '\x1C', kInnerArg = '\x1D';

bool is_mark(char c) { return c == kMessage || c == kArg || c == kInnerMessage || c == kInnerArg; }

} // namespace

std::wstring locale_of(const char* code) {
    if (code)
        for (const Locale& l : kLocales)
            if (!std::strcmp(l.code, code)) return l.name;
    return L"";
}

std::string upper_text(const std::string& s, const char* code) {
    if (s.empty()) return s;
    if (!is_utf8(s)) return ascii_upper(s);
    const int wn = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wn <= 0) return ascii_upper(s);
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), wn);
    const std::wstring locale = locale_of(code);
    const wchar_t* name       = locale.empty() ? LOCALE_NAME_INVARIANT : locale.c_str();
    const DWORD flags         = LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING;
    const int un = LCMapStringEx(name, flags, w.data(), wn, nullptr, 0, nullptr, nullptr, 0);
    if (un <= 0) return ascii_upper(s);
    std::wstring u(static_cast<std::size_t>(un), L'\0');
    if (LCMapStringEx(name, flags, w.data(), wn, u.data(), un, nullptr, nullptr, 0) != un) return ascii_upper(s);
    const int n = WideCharToMultiByte(CP_UTF8, 0, u.data(), un, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return ascii_upper(s);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, u.data(), un, out.data(), n, nullptr, nullptr);
    return out;
}

std::size_t text_chars(const std::string& s) {
    std::size_t chars = 0, units = 0;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = utf8_at(s, i, units);
        i += n ? n : 1;
        chars += units;
    }
    return chars;
}

std::string cut_text(const std::string& s, std::size_t chars) {
    std::size_t i = 0, used = 0, units = 0;
    while (i < s.size()) {
        std::size_t n = utf8_at(s, i, units);
        if (!n) n = 1;
        if (used + units > chars) break;
        used += units;
        i += n;
    }
    return s.substr(0, i);
}

std::string join_sentences(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    // 。 ！ ？ ） 」 and ． in UTF-8: Japanese and Chinese put no space after them
    static const char* const kNoSpace[] = {"\xE3\x80\x82", "\xEF\xBC\x81", "\xEF\xBC\x9F", "\xEF\xBC\x89", "\xE3\x80\x8D", "\xEF\xBC\x8E"};
    for (const char* e : kNoSpace) {
        const std::size_t n = std::strlen(e);
        if (a.size() >= n && a.compare(a.size() - n, n, e) == 0) return a + b;
    }
    return a + " " + b;
}

std::string fill(const std::string& tmpl, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(tmpl.size());
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            std::size_t j = i + 1, n = 0;
            while (j < tmpl.size() && j - i <= 3 && tmpl[j] >= '0' && tmpl[j] <= '9') n = n * 10 + static_cast<std::size_t>(tmpl[j++] - '0');
            if (j > i + 1 && j < tmpl.size() && tmpl[j] == '}' && n < args.size()) {
                out += args[n];
                i = j + 1;
                continue;
            }
        }
        out += tmpl[i++];
    }
    return out;
}

std::string later(const char* english_template, std::initializer_list<std::string> args) {
    std::string m(1, kMessage);
    for (const char* c = english_template ? english_template : ""; *c; ++c)
        if (!is_mark(*c)) m += *c;
    for (const std::string& a : args) {
        m += kArg;
        const bool inner = !a.empty() && a[0] == kMessage;
        for (char c : a) {
            if (inner && c == kMessage) m += kInnerMessage;
            else if (inner && c == kArg) m += kInnerArg;
            else if (!is_mark(c)) m += c;   // a message two levels deep loses its marks: it's shown as its text
        }
    }
    return m;
}

bool split_later(const std::string& message, std::string& english_template, std::vector<std::string>& args) {
    english_template.clear();
    args.clear();
    if (message.empty() || message[0] != kMessage) return false;
    std::string* to = &english_template;
    for (std::size_t i = 1; i < message.size(); ++i) {
        const char c = message[i];
        if (c == kArg) {
            to = &args.emplace_back();
        } else {
            *to += c == kInnerMessage ? kMessage : c == kInnerArg ? kArg : c;
        }
    }
    return true;
}

std::string english(const std::string& message) {
    std::string t;
    std::vector<std::string> args;
    if (!split_later(message, t, args)) return message;
    for (std::string& a : args) a = english(a);
    return fill(t, args);
}

} // namespace fh2platform
