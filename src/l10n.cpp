// Mods' text in the game's language (API 10, platform 2.9.0).
//
// A mod embeds its translations as gettext PO files, one per language, and hands each to
// add_translation; fh2-mods\lang\<mod>\<lang>.po, if there is one, wins over the DLL's, so a
// translator can try a file without a build. tr() looks a key (the PO msgctxt) up in the language
// now, falling back along fallbacks() and then to the English the mod passed, which is the msgid.
// An entry whose msgid isn't that English was translated from older English, so it's passed over
// rather than shown with the wrong meaning.
//
// The language: the game's own once it has one (lang.cpp's game_language, mapped to a BCP-47 tag).
// The game picks it seconds after the mods' fh2mod_init, so until then it's the one the game had
// last time, which fh2-mods\platform-language.ini remembers, or English. That file's test=1 turns on
// the test language, "qps": every text padded and in brackets, to find what's cut off or never goes
// through tr().
//
// Following the language (l10n.hpp): every text tr() hands out is the platform's own copy, kept for
// the session, and g_handed says which key and English it came from. The places mods pass text into
// note it (l10n_note), and the pages ask for the text again in the language now as they're written.
// String tables don't need that: set_string_tr writes each language's table with its own text, and
// the game picks its table itself.
#include "l10n.hpp"
#include "lang.hpp"
#include "platform.hpp"
#include "text.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace fh2platform {
namespace fs = std::filesystem;

// ---- the parts without state ------------------------------------------------------------------
namespace l10n {
namespace {

struct Lang {
    const char* code;   // the game's: its string-table zips' names
    const char* tag;
    const char* name;   // for log lines
};
constexpr Lang kLangs[] = {
    {"EN", "en", "English"},       {"GB", "en-GB", "British English"},  {"PL", "pl", "Polish"},
    {"BR", "pt-BR", "Brazilian Portuguese"}, {"DE", "de", "German"},    {"FR", "fr", "French"},
    {"ES", "es-ES", "Spanish (Spain)"}, {"MX", "es-MX", "Spanish (Mexico)"}, {"IT", "it", "Italian"},
    {"JP", "ja", "Japanese"},      {"KO", "ko", "Korean"},              {"CHT", "zh-Hant", "Traditional Chinese"},
    {"CZ", "cs", "Czech"},         {"HU", "hu", "Hungarian"},           {"RU", "ru", "Russian"},
    {"NL", "nl", "Dutch"},
};

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string primary(const std::string& tag) { return tag.substr(0, tag.find('-')); }

bool one_of(const std::string& s, std::initializer_list<const char*> list) {
    for (const char* x : list)
        if (s == x) return true;
    return false;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// A PO string, "..." with C escapes, as UTF-8; `ok` is false when `s` isn't one.
std::string po_string(std::string_view s, bool& ok) {
    ok = false;
    s  = trim(s);
    if (s.size() < 2 || s.front() != '"') return {};
    std::string o;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            ok = trim(s.substr(i + 1)).empty();
            return o;
        }
        if (c != '\\') {
            o += c;
            continue;
        }
        if (++i >= s.size()) return {};
        const char e = s[i];
        switch (e) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'a': o += '\a'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'v': o += '\v'; break;
            case 'x': {
                int v = 0, k = 0;
                while (k < 2 && i + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1]))) {
                    const char h = s[++i];
                    v            = v * 16 + (std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : std::tolower(h) - 'a' + 10);
                    ++k;
                }
                if (!k) return {};
                o += static_cast<char>(v);
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    int v = e - '0', k = 1;
                    while (k < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') v = v * 8 + (s[++i] - '0'), ++k;
                    o += static_cast<char>(v);
                } else {
                    o += e;   // \" \\ \' \? and anything else: the character itself
                }
        }
    }
    return {};   // no closing quote
}

// Where `s` stops being UTF-8 (a stray continuation byte, a cut-off sequence, an overlong form, a surrogate or
// past U+10FFFF), or npos when it's UTF-8 throughout.
std::size_t not_utf8_at(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n;
        uint32_t cp, least;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) n = 2, cp = c & 0x1F, least = 0x80;
        else if (c >= 0xE0 && c <= 0xEF) n = 3, cp = c & 0x0F, least = 0x800;
        else if (c >= 0xF0 && c <= 0xF4) n = 4, cp = c & 0x07, least = 0x10000;
        else return i;
        if (i + n > s.size()) return i;
        for (std::size_t k = 1; k < n; ++k) {
            const unsigned char b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) return i;
            cp = cp << 6 | (b & 0x3F);
        }
        if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return i;
        i += n;
    }
    return std::string_view::npos;
}

// Why a translation's text can't be used as it is, or "" when it can: a control character other than a line
// break or a tab (from an escape such as \0, \a or \x01), which the platform's pages can't hold and the game
// can't show, or bytes that aren't UTF-8 (from an escape such as \xE9).
std::string bad_text(const std::string& s) {
    for (const char c : s)
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') {
            char u[8];
            std::snprintf(u, sizeof u, "U+%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
            return std::string("its text has a control character (") + u + "), which the game can't show";
        }
    if (not_utf8_at(s) != std::string_view::npos) return "its text has an escape that isn't UTF-8 (such as \\xE9)";
    return "";
}

// The {placeholders}, {{tokens}} and braces on their own in `s`, sorted: what the game's own text formatting
// reads in a string table's text.
std::vector<std::string> brace_parts(std::string_view s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '{' && s[i] != '}') continue;
        if (s[i] == '{' && i + 1 < s.size() && s[i + 1] == '{') {   // {{font_pop}}
            const std::size_t j = s.find("}}", i + 2);
            if (j != std::string_view::npos && j > i + 2 && s.substr(i + 2, j - i - 2).find_first_of("{}") == std::string_view::npos) {
                out.emplace_back(s.substr(i, j - i + 2));
                i = j + 1;
                continue;
            }
        }
        if (s[i] == '{') {   // {0}, {name}: as placeholders() finds them
            std::size_t j = i + 1;
            while (j < s.size() && s[j] != '}' && s[j] != '{' && s[j] != ' ' && s[j] != '\t' && s[j] != '\n') ++j;
            if (j < s.size() && s[j] == '}' && j > i + 1) {
                out.emplace_back(s.substr(i, j - i + 1));
                i = j;
                continue;
            }
        }
        out.emplace_back(1, s[i]);   // a brace on its own
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    if (v.empty()) return "none";
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

// The test language's letters for ASCII ones: Polish and Portuguese, so they're in the game's fonts.
const char* pseudo_letter(char c) {
    switch (c) {
        case 'a': return "\xC3\xA3";       // ã
        case 'c': return "\xC3\xA7";       // ç
        case 'e': return "\xC4\x99";       // ę
        case 'i': return "\xC3\xAD";       // í
        case 'l': return "\xC5\x82";       // ł
        case 'n': return "\xC5\x84";       // ń
        case 'o': return "\xC3\xB3";       // ó
        case 's': return "\xC5\x9B";       // ś
        case 'u': return "\xC3\xBA";       // ú
        case 'z': return "\xC5\xBC";       // ż
        case 'A': return "\xC3\x83";       // Ã
        case 'C': return "\xC3\x87";       // Ç
        case 'E': return "\xC4\x98";       // Ę
        case 'I': return "\xC3\x8D";       // Í
        case 'L': return "\xC5\x81";       // Ł
        case 'N': return "\xC5\x83";       // Ń
        case 'O': return "\xC3\x93";       // Ó
        case 'S': return "\xC5\x9A";       // Ś
        case 'U': return "\xC3\x9A";       // Ú
        case 'Z': return "\xC5\xBB";       // Ż
        default: return nullptr;
    }
}
// What the padding is made of.
const char* const kPseudoPad[] = {"\xC4\x85", "\xC4\x87", "\xC4\x99", "\xC5\x82", "\xC5\x84", "\xC3\xB3", "\xC5\x9B", "\xC5\xBA",
                                  "\xC5\xBC", "\xC3\xA3", "\xC3\xA7", "\xC3\xA9", "\xC3\xAA", "\xC3\xAD", "\xC3\xB5", "\xC3\xBA"};

} // namespace

std::vector<std::string> placeholders(std::string_view s) {
    std::set<std::string> found;
    for (std::size_t i = s.find('{'); i != std::string_view::npos; i = s.find('{', i + 1)) {
        std::size_t j = i + 1;
        while (j < s.size() && s[j] != '}' && s[j] != '{' && s[j] != ' ' && s[j] != '\t' && s[j] != '\n') ++j;
        if (j < s.size() && s[j] == '}' && j > i + 1) found.emplace(s.substr(i, j - i + 1));
    }
    return {found.begin(), found.end()};
}

bool placeholders_fit(const Entry& e, const std::string& text, bool only_for_one) {
    const auto t   = placeholders(text);
    const auto one = placeholders(e.msgid);
    if (e.msgid_plural.empty()) return t == one;
    const auto other = placeholders(e.msgid_plural);
    std::vector<std::string> any, both;
    std::set_union(one.begin(), one.end(), other.begin(), other.end(), std::back_inserter(any));
    std::set_intersection(one.begin(), one.end(), other.begin(), other.end(), std::back_inserter(both));
    // a form only for 1 may leave placeholders out ("Jedno drzewo" for "{n} tree broken"), as gettext's own check allows
    return std::includes(any.begin(), any.end(), t.begin(), t.end()) &&
           (only_for_one || std::includes(t.begin(), t.end(), both.begin(), both.end()));
}

bool braces_fit(std::string_view english, std::string_view text) {
    const std::vector<std::string> e = brace_parts(english), t = brace_parts(text);
    std::ptrdiff_t open = 0, close = 0;
    for (const std::string& x : t) {
        if (x == "{") ++open;
        else if (x == "}") ++close;
        else if (!std::binary_search(e.begin(), e.end(), x)) return false;
    }
    return open <= std::count(e.begin(), e.end(), "{") && close <= std::count(e.begin(), e.end(), "}");
}

int plural_forms(const std::string& tag) {
    int most = 0;
    for (int64_t n = 0; n < 200; ++n) most = std::max(most, plural_form(tag, n));   // the rules go by n % 100 at most
    return most + 1;
}

bool form_only_for_one(const std::string& tag, int form) {
    if (plural_form(tag, 1) != form) return false;
    // the rules go by n % 100 and n % 10 at most, so 0 to 199 has every case
    for (int64_t n = 0; n < 200; ++n)
        if (n != 1 && plural_form(tag, n) == form) return false;
    return true;
}

int parse_po(std::string_view po, const std::string& tag, Catalogue& out, std::vector<std::string>& refused) {
    // Notepad's "Unicode" is UTF-16, which would be a line that isn't PO for every line
    if (po.size() >= 2 && (po.substr(0, 2) == "\xFF\xFE" || po.substr(0, 2) == "\xFE\xFF")) {
        refused.push_back("it's saved as UTF-16 (Notepad's \"Unicode\"), so nothing in it is read: save it as UTF-8");
        return 0;
    }
    if (po.size() >= 3 && po.substr(0, 3) == "\xEF\xBB\xBF") po.remove_prefix(3);
    // A file saved as Windows-1250 or -1252 ("ANSI") would show its letters wrongly everywhere, so none of it is used.
    if (const std::size_t bad = not_utf8_at(po); bad != std::string_view::npos) {
        const auto line = std::count(po.begin(), po.begin() + static_cast<std::ptrdiff_t>(bad), '\n') + 1;
        refused.push_back("line " + std::to_string(line) + " isn't UTF-8, so nothing in the file is read: save it as UTF-8");
        return 0;
    }
    std::string header;   // the header entry's text, for its Plural-Forms
    struct Pending {
        bool ctxt_seen = false, id_seen = false, str_seen = false, fuzzy = false;
        std::string ctxt, id, plural;
        std::map<int, std::string> strs;
        int line = 0;
    } p;
    std::string* last = nullptr;
    int taken = 0;
    auto take = [&] {
        if (p.id_seen && p.id.empty() && !p.ctxt_seen && p.strs.count(0)) header = p.strs[0];   // fuzzy or not
        if (!p.id_seen || p.id.empty() || p.fuzzy) return;   // the header, a fuzzy entry
        Entry e;
        e.msgid        = p.id;
        e.msgid_plural = p.plural;
        e.line         = p.line;
        const int forms = p.plural.empty() ? 1 : (p.strs.empty() ? 0 : p.strs.rbegin()->first + 1);
        for (int i = 0; i < forms; ++i) e.msgstr.push_back(p.strs.count(i) ? p.strs[i] : std::string());
        if (std::all_of(e.msgstr.begin(), e.msgstr.end(), [](const std::string& s) { return s.empty(); })) return;   // untranslated
        for (const std::string& s : e.msgstr)
            if (const std::string why = bad_text(s); !why.empty()) {
                refused.push_back("line " + std::to_string(p.line) + ", \"" + (p.ctxt_seen ? p.ctxt : p.id) + "\": " + why +
                                  ", so it's left out and English shows there");
                return;
            }
        for (std::size_t f = 0; f < e.msgstr.size(); ++f) {
            const std::string& s = e.msgstr[f];
            if (s.empty() || placeholders_fit(e, s, !p.plural.empty() && form_only_for_one(tag, static_cast<int>(f)))) continue;
            std::vector<std::string> english = placeholders(e.msgid);
            for (const std::string& x : placeholders(e.msgid_plural))
                if (std::find(english.begin(), english.end(), x) == english.end()) english.push_back(x);
            refused.push_back("line " + std::to_string(p.line) + ", \"" + (p.ctxt_seen ? p.ctxt : p.id) + "\": its placeholders (" +
                              joined(placeholders(s)) + ") don't match the English's (" + joined(english) +
                              "), so it's left out and English shows there");
            return;
        }
        (p.ctxt_seen ? out.by_key[p.ctxt] : out.by_english[p.id]) = std::move(e);
        ++taken;
    };
    auto flush = [&] {
        take();
        p    = Pending{};
        last = nullptr;
    };
    int lineno = 0;
    std::size_t at = 0;
    while (at <= po.size()) {
        std::size_t end = po.find('\n', at);
        if (end == std::string_view::npos) end = po.size();
        const std::string_view line = trim(po.substr(at, end - at));
        at                          = end + 1;
        ++lineno;
        if (line.empty()) {
            flush();
            if (end == po.size()) break;
            continue;
        }
        if (line[0] == '#') {
            if (p.str_seen) flush();   // a comment starts the next entry
            if (line.substr(0, 2) == "#,") {
                for (std::size_t f = 2; f < line.size();) {
                    std::size_t g = line.find(',', f);
                    if (g == std::string_view::npos) g = line.size();
                    if (trim(line.substr(f, g - f)) == "fuzzy") p.fuzzy = true;
                    f = g + 1;
                }
            }
            continue;
        }
        bool ok = false;
        if (line[0] == '"') {
            const std::string s = po_string(line, ok);
            if (!ok || !last) refused.push_back("line " + std::to_string(lineno) + " isn't PO; it's skipped");
            else *last += s;
            continue;
        }
        const std::size_t sp      = line.find_first_of(" \t");
        const std::string_view kw = line.substr(0, sp);
        const std::string s       = sp == std::string_view::npos ? std::string() : po_string(line.substr(sp), ok);
        if (!ok) {
            refused.push_back("line " + std::to_string(lineno) + " isn't PO; it's skipped");
            continue;
        }
        if (kw == "msgctxt") {
            if (p.id_seen || p.str_seen || p.ctxt_seen) {
                const bool fuzzy = p.fuzzy && !p.id_seen;
                flush();
                p.fuzzy = fuzzy;
            }
            p.ctxt_seen = true;
            p.ctxt      = s;
            last        = &p.ctxt;
        } else if (kw == "msgid") {
            if (p.id_seen || p.str_seen) flush();
            p.id_seen = true;
            p.id      = s;
            p.line    = lineno;
            last      = &p.id;
        } else if (kw == "msgid_plural" && p.id_seen && !p.str_seen) {
            p.plural = s;
            last     = &p.plural;
        } else if (kw == "msgstr" && p.id_seen) {
            p.str_seen = true;
            p.strs[0]  = s;
            last       = &p.strs[0];
        } else if (kw.size() == 9 && kw.substr(0, 7) == "msgstr[" && kw[8] == ']' && kw[7] >= '0' && kw[7] <= '9' && p.id_seen) {
            const int n = kw[7] - '0';
            p.str_seen  = true;
            p.strs[n]   = s;
            last        = &p.strs[n];
        } else {
            refused.push_back("line " + std::to_string(lineno) + " isn't PO; it's skipped");
            last = nullptr;
        }
    }
    flush();
    // Plural forms are numbered by the platform's rule for the language (gettext's usual one), whatever the header
    // says: a file with another count (CLDR's four for Czech, from some tools) has its forms in other places.
    if (const std::size_t at = header.find("nplurals"); at != std::string::npos) {
        std::size_t i = at + 8;
        while (i < header.size() && (header[i] == ' ' || header[i] == '\t')) ++i;
        if (i < header.size() && header[i] == '=') {
            ++i;
            while (i < header.size() && (header[i] == ' ' || header[i] == '\t')) ++i;
            int n = 0, digits = 0;
            while (i < header.size() && digits < 3 && header[i] >= '0' && header[i] <= '9') n = n * 10 + (header[i++] - '0'), ++digits;
            const int forms = plural_forms(tag);
            if (digits && n != forms)
                refused.push_back("its header says nplurals=" + std::to_string(n) + ", but the platform reads " + std::to_string(forms) +
                                  (forms == 1 ? " plural form" : " plural forms") + " for " + language_name(tag) + " (" +
                                  (forms == 1 ? std::string("msgstr[0] alone") : "msgstr[0] to msgstr[" + std::to_string(forms - 1) + "]") +
                                  ", in gettext's usual order for it), so a count may show the wrong form: check its plural texts");
        }
    }
    return taken;
}

const char* tag_for_code(std::string_view code) {
    for (const Lang& l : kLangs)
        if (ieq(code, l.code)) return l.tag;
    return nullptr;
}

std::string normal_tag(std::string_view tag) {
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i <= tag.size(); ++i) {
        if (i == tag.size() || tag[i] == '-' || tag[i] == '_') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += tag[i];
        }
    }
    auto letters = [](const std::string& s) {
        return std::all_of(s.begin(), s.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
    };
    auto digits = [](const std::string& s) {
        return std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    };
    if (parts.empty() || parts[0].size() < 2 || parts[0].size() > 3 || !letters(parts[0])) return "";
    std::string out;
    for (std::size_t k = 0; k < parts.size(); ++k) {
        std::string s = parts[k];
        if (k == 0) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (s.size() == 4 && letters(s)) {   // a script: Hant
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        } else if (s.size() == 2 && letters(s)) {   // a region: BR
            for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else if (!(s.size() == 3 && digits(s))) {  // or 419
            return "";
        }
        out += (k ? "-" : "") + s;
    }
    return out;
}

std::vector<std::string> fallbacks(const std::string& tag) {
    std::vector<std::string> out{tag};
    if (tag == "es-MX") out.push_back("es-ES");
    if (const std::size_t dash = tag.find('-'); dash != std::string::npos) out.push_back(tag.substr(0, dash));
    return out;
}

std::string catalogue_tag(const std::string& tag) {
    // Traditional Chinese, as Taiwan, Hong Kong and Macau write it: the game's CHT
    if (tag == "zh-TW" || tag == "zh-HK" || tag == "zh-MO" || tag.rfind("zh-Hant-", 0) == 0) return "zh-Hant";
    // a language the game has in one form, named with its usual region, as translation tools often name them
    for (const char* t : {"pl-PL", "de-DE", "fr-FR", "it-IT", "ja-JP", "ko-KR", "cs-CZ", "hu-HU", "ru-RU", "nl-NL", "en-US"})
        if (tag == t) return primary(tag);
    return tag;
}

std::string never_shown(const std::string& tag) {
    for (const Lang& l : kLangs)
        for (const std::string& t : fallbacks(l.tag))
            if (t == tag) return "";
    const std::string lang = primary(tag);
    const std::string head = "none of the game's languages looks for " + tag + " text";
    if (lang == "en") return head + " (its English is en, American, or en-GB, British), so name the file en.po or en_GB.po";
    if (lang == "pt") return head + " (its Portuguese is pt-BR, Brazilian), so name the file pt_BR.po or pt.po";
    if (lang == "es") return head + " (its Spanish is es-ES, for Spain, or es-MX, for Latin America), so name the file es_ES.po, es_MX.po or es.po";
    if (lang == "zh") return head + " (its Chinese is zh-Hant, Traditional; it has no Simplified Chinese), so name a Traditional Chinese file zh_Hant.po";
    for (const Lang& l : kLangs)
        if (lang == l.tag) return head + " (its " + l.name + " is " + lang + "), so name the file " + lang + ".po";
    return head + ", as the game has no such language";
}

int plural_form(const std::string& tag, int64_t n) {
    const std::string lang = primary(tag);
    const uint64_t a       = n < 0 ? 0 - static_cast<uint64_t>(n) : static_cast<uint64_t>(n);
    const uint64_t d10 = a % 10, d100 = a % 100;
    const bool few     = d10 >= 2 && d10 <= 4 && (d100 < 10 || d100 >= 20);
    if (lang == "pl") return a == 1 ? 0 : few ? 1 : 2;
    if (one_of(lang, {"ru", "uk", "be"})) return d10 == 1 && d100 != 11 ? 0 : few ? 1 : 2;
    if (one_of(lang, {"cs", "sk"})) return a == 1 ? 0 : a >= 2 && a <= 4 ? 1 : 2;
    if (one_of(lang, {"pt", "fr"})) return a > 1 ? 1 : 0;
    if (one_of(lang, {"ja", "ko", "zh"})) return 0;
    return a == 1 ? 0 : 1;
}

std::string pseudo(std::string_view english) {
    std::string body;
    std::size_t chars = 0;
    for (std::size_t i = 0; i < english.size();) {
        const char c = english[i];
        if (c == '{') {   // a placeholder stays as it is
            const std::size_t j = english.find('}', i);
            const auto inside   = j == std::string_view::npos ? std::string_view() : english.substr(i + 1, j - i - 1);
            if (j != std::string_view::npos && !inside.empty() && inside.find_first_of("{ \t\n") == std::string_view::npos) {
                body += english.substr(i, j - i + 1);
                chars += j - i + 1;
                i = j + 1;
                continue;
            }
        }
        const unsigned char u = static_cast<unsigned char>(c);
        std::size_t n         = u < 0x80 ? 1 : u >= 0xF0 ? 4 : u >= 0xE0 ? 3 : u >= 0xC0 ? 2 : 1;
        if (i + n > english.size()) n = 1;
        if (const char* p = n == 1 ? pseudo_letter(c) : nullptr) body += p;
        else body += english.substr(i, n);
        ++chars;
        i += n;
    }
    // how much longer translations get: twice up to 10 characters, 1.7 times to 20, 1.5 to 50, then 1.35
    const double grow        = chars <= 10 ? 2.0 : chars <= 20 ? 1.7 : chars <= 50 ? 1.5 : 1.35;
    const std::size_t target = static_cast<std::size_t>(std::ceil(static_cast<double>(chars) * grow));
    // the padding: a space and letters, or one letter alone when there's room for no more
    std::string pad;
    const std::size_t room = target > chars + 2 ? target - chars - 2 : 0;
    for (std::size_t k = 0; k < room; ++k)
        pad += k == 0 && room > 1 ? " " : kPseudoPad[k % (sizeof kPseudoPad / sizeof kPseudoPad[0])];
    return "[" + body + pad + "]";
}

int format_number(const std::string& tag, double v, int decimals, char* out, std::size_t n) {
    if (!out || !n) return 0;
    out[0] = '\0';
    if (!std::isfinite(v)) return 0;
    decimals = decimals < 0 ? 0 : decimals > 6 ? 6 : decimals;
    char buf[400];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, std::fabs(v));
    const std::string digits = buf;
    const std::size_t point  = digits.find('.');
    const std::string whole  = digits.substr(0, point);
    const std::string part   = point == std::string::npos ? "" : digits.substr(point + 1);
    const bool zero = digits.find_first_not_of("0.") == std::string::npos;
    // thousands and the decimal point: a no-break space (U+00A0, which the game's fonts have; not
    // U+202F, which they don't) and a comma, a full stop and a comma, or as in English
    const std::string lang = primary(tag);
    const char *group = ",", *dec = ".";
    if (tag == "qps" || one_of(lang, {"pl", "ru", "uk", "cs", "sk", "fr", "hu"})) group = "\xC2\xA0", dec = ",";
    else if (tag != "es-MX" && one_of(lang, {"pt", "de", "it", "nl", "es", "da", "tr"})) group = ".", dec = ",";
    // Polish and Spain's Spanish group only from five digits, as CLDR has them: 1234, but 12 345 / 12.345.
    const bool from_five = lang == "pl" || (lang == "es" && tag != "es-MX");
    const bool grouped   = !(from_five && whole.size() <= 4);
    std::string s = v < 0 && !zero ? "-" : "";
    for (std::size_t i = 0; i < whole.size(); ++i) {
        if (grouped && i && (whole.size() - i) % 3 == 0) s += group;
        s += whole[i];
    }
    if (!part.empty()) s += dec + part;
    if (s.size() + 1 > n) return 0;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

int format_date(const std::string& tag, int y, int m, int d, char* out, std::size_t n) {
    if (!out || !n) return 0;
    out[0] = '\0';
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) return 0;
    // Days in the month, with the Gregorian leap years: 30 February or 31 April isn't a date
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (d > kDays[m - 1] + (m == 2 && leap ? 1 : 0)) return 0;
    const std::string lang = primary(tag);
    char buf[32];
    if (tag == "qps" || one_of(lang, {"pl", "de", "ru", "uk", "cs", "sk"})) std::snprintf(buf, sizeof buf, "%02d.%02d.%04d", d, m, y);
    else if (one_of(lang, {"pt", "fr", "es", "it"})) std::snprintf(buf, sizeof buf, "%02d/%02d/%04d", d, m, y);
    else if (lang == "nl") std::snprintf(buf, sizeof buf, "%02d-%02d-%04d", d, m, y);
    else if (one_of(lang, {"ja", "zh"})) std::snprintf(buf, sizeof buf, "%04d/%02d/%02d", y, m, d);
    else if (one_of(lang, {"ko", "hu"})) std::snprintf(buf, sizeof buf, "%04d. %02d. %02d.", y, m, d);
    else std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);   // English: as the platform writes dates now
    const std::size_t len = std::strlen(buf);
    if (len + 1 > n) return 0;
    std::memcpy(out, buf, len + 1);
    return static_cast<int>(len);
}

std::string language_name(const std::string& tag) {
    for (const Lang& l : kLangs)
        if (tag == l.tag) return l.name;
    if (tag == "pt") return "Portuguese";
    if (tag == "es") return "Spanish";
    if (tag == "zh") return "Chinese";
    if (tag == "qps") return "the test language";
    return tag;
}

std::string guess_from_windows(std::wstring_view locale) {
    std::string name;
    for (wchar_t c : locale) name += c < 128 ? static_cast<char>(std::tolower(static_cast<int>(c))) : '?';
    const std::string lang   = name.substr(0, name.find('-'));
    const std::string region = name.size() > lang.size() + 1 ? name.substr(lang.size() + 1) : "";
    // The game's table (game+0x141245CF0) maps a regional format to one of its locales, which picks its text.
    if (lang == "pl") return "pl";
    if (lang == "pt") return "pt-BR";   // pt-PT too: the game has only the Brazilian text
    if (lang == "de") return "de";
    if (lang == "fr") return "fr";      // fr-CA too: the same French text
    if (lang == "it") return "it";
    if (lang == "ja") return "ja";
    if (lang == "ru") return "ru";
    if (lang == "es") return region.empty() || region == "es" ? "es-ES" : "es-MX";
    if (lang == "zh") return region == "hk" || region == "tw" || region.rfind("hant", 0) == 0 ? "zh-Hant" : "en";
    if (lang == "en") return region.empty() || region == "us" || region == "au" || region == "ca" ? "en" : "en-GB";
    return "en";   // Korean, Czech, Hungarian, Dutch and the rest: not in the game's table, so its English
}

} // namespace l10n

// ---- the state ----------------------------------------------------------------------------------
namespace {
using namespace l10n;

std::string utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring utf16(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One mod's translations.
struct ModText {
    std::map<std::string, Catalogue> langs;     // by tag
    std::map<std::string, std::string> files;   // the tags read from fh2-mods\lang\<mod>\, and their files
    bool scanned = false;
    std::set<std::string> stale_said;           // "<tag> <key>": said in the log already
    std::size_t texts = 0;                      // texts handed out
    bool many_said    = false;
};

// Where a text handed out came from: what tr() or tr_plural() was asked.
struct Ref {
    FH2Mod* mod = nullptr;
    std::string key, one, other;
    int64_t n   = 0;
    bool plural = false;
};

std::mutex g_mu;   // everything below; never held while calling into the rest of the platform
std::map<FH2Mod*, ModText> g_mods;
std::deque<Ref> g_refs;
std::map<std::string, std::size_t> g_ref_ids;                  // what was asked -> index in g_refs
std::deque<std::string> g_texts;                               // every text handed out: never freed
std::map<std::string, const std::string*> g_interned;          // "<ref>\x1f<text>" -> its copy in g_texts
std::unordered_map<const char*, std::size_t> g_handed;         // a text handed out -> its Ref
std::map<std::pair<FH2Mod*, std::string>, std::size_t> g_slots; // the places that hold tr()'s text -> its Ref
struct WideSlot {
    std::size_t ref;
    std::wstring suffix;
    std::size_t max_chars = 0;   // the mod's text cut to this many characters, as text_chars counts them (0: as it is)
};
std::unordered_map<const wchar_t*, WideSlot> g_wslots;
std::deque<std::wstring> g_wtexts;
std::map<std::wstring, const std::wstring*> g_winterned;
constexpr std::size_t kManyTexts = 20000;

// fh2-mods\platform-language.ini, read once.
std::once_flag g_state_once;
std::string g_last;         // the game's language last time ("" if never seen)
std::string g_start_tag;    // the language before the game has one: g_last, or a guess from Windows
std::wstring g_windows;     // Windows' regional format, for that guess ("pl-PL"; "" if not asked or unknown)
bool g_test = false;        // the test language
fs::path state_path() { return azimuth_dir() / L"fh2-mods" / L"platform-language.ini"; }

void read_state() {
    std::call_once(g_state_once, [] {
        std::ifstream in{state_path()};
        std::string line;
        while (std::getline(in, line)) {
            const std::string_view l = trim(line);
            if (l.empty() || l[0] == ';' || l[0] == '#') continue;
            const std::size_t eq = l.find('=');
            if (eq == std::string_view::npos) continue;
            std::string key(trim(l.substr(0, eq)));
            for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const std::string value(trim(l.substr(eq + 1)));
            if (key == "last") g_last = normal_tag(value);
            if (key == "test") g_test = value == "1" || value == "on" || value == "yes" || value == "true";
        }
        g_start_tag = g_last;
        if (g_start_tag.empty()) {   // a first launch: guess as the game will pick, from Windows' regional format
            wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
            if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) g_windows = name;
            g_start_tag = l10n::guess_from_windows(g_windows);
        }
    });
}

void write_state(const std::string& last) {
    std::error_code ec;
    fs::create_directories(state_path().parent_path(), ec);
    std::ofstream out{state_path(), std::ios::binary | std::ios::trunc};
    out << "; The FH2 Mod Platform's language memory. The platform writes this file; you can delete it.\r\n"
           "; last: the game's text language last time (pl, pt-BR, en...). Mods' text starts in it, before the\r\n"
           "; game picks its own a few seconds after it starts.\r\n"
           "last="
        << last
        << "\r\n"
           "; test: 1 shows mods' translatable text in the test language, padded and with Polish and Portuguese\r\n"
           "; letters, in [brackets], to find text that's cut off or doesn't go through a translation. 0 is off.\r\n"
           "test="
        << (g_test ? "1" : "0") << "\r\n";
}

// The game's language as a tag, or nullptr before it has one.
const char* game_tag() {
    char code[8];
    if (!game_language(code, sizeof code)) return nullptr;
    const char* t = tag_for_code(code);
    return t ? t : "en";
}

// "1 string", "6 strings", for log lines.
std::string strings(int n) { return std::to_string(n) + (n == 1 ? " string" : " strings"); }

// The mod's translations, with its folder under fh2-mods\lang read the first time.
ModText& mod_text(FH2Mod* m, std::vector<std::string>& notes) {
    ModText& mt = g_mods[m];
    if (mt.scanned || !m) return mt;
    mt.scanned             = true;
    const std::string name = m->name;
    if (name.empty() || name == "." || name == ".." || name.find_first_of("\\/:*?\"<>|") != std::string::npos) return mt;
    const fs::path dir = azimuth_dir() / L"fh2-mods" / L"lang" / utf16(name);
    std::error_code ec;
    std::vector<fs::path> paths;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && ieq(utf8(e.path().extension().wstring()), ".po")) paths.push_back(e.path());
    // Of two files for one language, the one named for the language itself is read (pl.po, not pl_PL.po or pl-PL.po),
    // whatever order the folder lists them in (NTFS puts pl-PL.po first), so those files go first.
    std::stable_partition(paths.begin(), paths.end(), [](const fs::path& p) {
        const std::string own = normal_tag(utf8(p.stem().wstring()));
        return !own.empty() && catalogue_tag(own) == own;
    });
    for (const fs::path& path : paths) {
        const std::string file = utf8(path.filename().wstring());
        const std::string own  = normal_tag(utf8(path.stem().wstring()));
        if (own.empty()) {
            notes.push_back("fh2-mods\\lang\\" + name + "\\" + file + " isn't named after a language (pl.po, pt-BR.po...), so it isn't read");
            continue;
        }
        const std::string tag = catalogue_tag(own);   // pl_PL.po is Polish, as pl.po
        if (const std::string why = never_shown(tag); !why.empty()) {
            notes.push_back("fh2-mods\\lang\\" + name + "\\" + file + " isn't read: " + why);
            continue;
        }
        if (const auto f = mt.files.find(tag); f != mt.files.end()) {   // two files for one language: the first, pl.po if there is one
            notes.push_back("fh2-mods\\lang\\" + name + "\\" + file + " is " + language_name(tag) + " (" + tag + ") too, and " + f->second +
                            " is the one used, so it isn't read");
            continue;
        }
        std::ifstream in{path, std::ios::binary};
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        Catalogue c;
        std::vector<std::string> refused;
        const int n = parse_po(text, tag, c, refused);
        for (const std::string& r : refused) notes.push_back("translation " + tag + " (" + file + "): " + r);
        mt.langs[tag] = std::move(c);
        mt.files[tag] = file;
        notes.push_back("translation: " + language_name(tag) + " (" + tag + ") from fh2-mods\\lang\\" + name + "\\" + file + ", " +
                        strings(n) + "; it's used instead of any the mod has built in");
    }
    return mt;
}

// Which plural form `n` takes in each set of rules plural_form() knows ("en", "pl", "ru", "cs", "pt" and
// "ja" stand for the rest): two counts with the same forms everywhere show the same text in every
// language. A count shown live (trees broken, metres driven) then needs a handful of Refs, not one per n.
std::string plural_class(int64_t n) {
    std::string s;
    for (const char* t : {"en", "pl", "ru", "cs", "pt", "ja"}) s += static_cast<char>('0' + plural_form(t, n));
    return s;
}

constexpr std::size_t kNoRef = static_cast<std::size_t>(-1);

// What was asked's index in g_refs, made if it's new, unless `add` is false (then kNoRef).
std::size_t ref_of(FH2Mod* m, const std::string& key, const std::string& one, const std::string& other, int64_t n, bool plural,
                   bool add = true) {
    char id[64];
    std::snprintf(id, sizeof id, "%p\x1f%d\x1f", static_cast<void*>(m), plural ? 1 : 0);
    const std::string k = id + (plural ? plural_class(n) : std::string()) + '\x1f' + key + '\x1f' + one + '\x1f' + other;
    if (auto it = g_ref_ids.find(k); it != g_ref_ids.end()) return it->second;
    if (!add) return kNoRef;
    g_refs.push_back({m, key, one, other, plural ? n : 0, plural});   // n: the first count of its class, which stands for them all
    g_ref_ids.emplace(k, g_refs.size() - 1);
    return g_refs.size() - 1;
}

// What `r` shows in `tag`'s language.
std::string text_for(const Ref& r, const std::string& tag, std::vector<std::string>& notes) {
    const std::string english = r.plural && plural_form("en", r.n) != 0 ? r.other : r.one;   // English: singular only for 1 (and -1)
    if (tag == "qps") return pseudo(english);
    if (!r.mod) return english;
    ModText& mt = mod_text(r.mod, notes);
    for (const std::string& t : fallbacks(tag)) {
        const auto c = mt.langs.find(t);
        if (c == mt.langs.end()) continue;
        const auto& map  = r.key.empty() ? c->second.by_english : c->second.by_key;
        const auto found = map.find(r.key.empty() ? r.one : r.key);
        if (found == map.end()) continue;
        const Entry& e = found->second;
        if (e.msgid != r.one || (r.plural && e.msgid_plural != r.other)) {
            // translated from other English: passed over, so it never shows with the wrong meaning
            if (mt.stale_said.insert(t + " " + r.key).second)
                notes.push_back("the " + t + " text for \"" + (r.key.empty() ? r.one : r.key) +
                                "\" was translated from different English, so it isn't used until it's translated again");
            continue;
        }
        const std::size_t form = r.plural ? static_cast<std::size_t>(plural_form(t, r.n)) : 0;
        if (form < e.msgstr.size() && !e.msgstr[form].empty()) return e.msgstr[form];
    }
    return english;
}

// The platform's copy of `text` for `ref`: it stays valid for the session.
const char* hand_out(std::size_t ref, const std::string& text, std::vector<std::string>& notes) {
    const std::string k = std::to_string(ref) + '\x1f' + text;
    if (auto it = g_interned.find(k); it != g_interned.end()) return it->second->c_str();
    g_texts.push_back(text);
    const std::string* p = &g_texts.back();
    g_interned.emplace(k, p);
    g_handed[p->c_str()] = ref;
    ModText& mt          = g_mods[g_refs[ref].mod];
    if (++mt.texts >= kManyTexts && !mt.many_said) {
        mt.many_said = true;
        notes.push_back("tr has handed out " + std::to_string(kManyTexts) +
                        " different texts, and the platform keeps each one for the session, so it stops there: from now on, English it "
                        "hasn't been given before comes back as it is, untranslated and not copied, as before API 10. Pass the same "
                        "English each time, and fill numbers and names in afterwards");
    }
    return p->c_str();
}

// Whether `m` has had kManyTexts texts: tr() and tr_plural() then make nothing new for it.
bool full(FH2Mod* m) {
    const auto it = g_mods.find(m);
    return it != g_mods.end() && it->second.texts >= kManyTexts;
}

void say(FH2Mod* m, const std::vector<std::string>& notes) {
    for (const std::string& n : notes) log_line(m ? m->name : std::string(), n);
}

} // namespace

// ---- API 10 -------------------------------------------------------------------------------------
const char* l10n_language() {
    read_state();
    if (g_test) return "qps";
    if (const char* t = game_tag()) return t;
    return g_start_tag.c_str();
}

int l10n_add_translation(FH2Mod* m, const char* lang, const char* po, std::size_t size) {
    if (!m) return 0;
    std::vector<std::string> notes;
    int taken = 0;
    {
        std::scoped_lock lk{g_mu};
        ModText& mt           = mod_text(m, notes);
        const std::string tag = catalogue_tag(normal_tag(lang ? lang : ""));   // "pl_PL" is Polish, as "pl"
        std::string why;
        if (tag.empty()) {
            notes.push_back(std::string("add_translation: \"") + (lang ? lang : "") + "\" isn't a language tag such as pl or pt-BR; nothing was added");
        } else if (!(why = never_shown(tag)).empty()) {
            notes.push_back("add_translation: nothing was added for " + tag + ": " + why);
        } else if (const auto f = mt.files.find(tag); f != mt.files.end()) {
            const Catalogue& c = mt.langs[tag];
            taken              = static_cast<int>(c.by_key.size() + c.by_english.size());
            notes.push_back("add_translation: " + tag + " comes from fh2-mods\\lang\\" + m->name + "\\" + f->second +
                            " this time, not from the mod");
        } else if (!po || !size) {
            notes.push_back("add_translation: " + tag + " has no text; nothing was added");
        } else {
            std::vector<std::string> refused;
            taken = parse_po(std::string_view(po, size), tag, mt.langs[tag], refused);
            for (const std::string& r : refused) notes.push_back("translation " + tag + ": " + r);
            notes.push_back("translation: " + language_name(tag) + " (" + tag + "), " + strings(taken));
        }
    }
    say(m, notes);
    return taken;
}

const char* l10n_tr(FH2Mod* m, const char* key, const char* english) {
    const std::string tag = l10n_language();
    std::vector<std::string> notes;
    const char* out = nullptr;
    {
        std::scoped_lock lk{g_mu};
        // past kManyTexts, English not asked for before is the mod's own, as before API 10 (hand_out logged why)
        const std::size_t r = ref_of(m, key ? key : "", english ? english : "", "", 0, false, !full(m));
        out                 = r == kNoRef ? (english ? english : "") : hand_out(r, text_for(g_refs[r], tag, notes), notes);
    }
    say(m, notes);
    return out;
}

const char* l10n_tr_plural(FH2Mod* m, const char* key, const char* one, const char* other, int64_t n) {
    const std::string tag = l10n_language();
    std::vector<std::string> notes;
    const char* out = nullptr;
    {
        std::scoped_lock lk{g_mu};
        const std::size_t r = ref_of(m, key ? key : "", one ? one : "", other ? other : (one ? one : ""), n, true, !full(m));
        if (r != kNoRef) out = hand_out(r, text_for(g_refs[r], tag, notes), notes);
        else out = plural_form("en", n) != 0 && other ? other : one ? one : "";   // the English form, as text_for picks it
    }
    say(m, notes);
    return out;
}

int l10n_format_number(double v, int decimals, char* out, std::size_t n) { return format_number(l10n_language(), v, decimals, out, n); }

int l10n_format_date(int y, int m, int d, char* out, std::size_t n) { return format_date(l10n_language(), y, m, d, out, n); }

// ---- the platform's side --------------------------------------------------------------------------
void l10n_start() {
    read_state();
    const std::string from = "fh2-mods\\platform-language.ini";
    if (g_test)
        plog("Language: the test language is on (test=1 in " + from +
             "): mods' translatable text shows padded, with Polish and Portuguese letters, in [brackets], to find text that's cut off "
             "or doesn't go through a translation");
    else if (!g_last.empty())
        plog("Language: mods' text starts in " + language_name(g_last) + " (" + g_last +
             "), the game's language last time, and follows the game's own once it has picked it");
    else
        plog("Language: mods' text starts in " + language_name(g_start_tag) + " (" + g_start_tag + "), the game's choice for " +
             (g_windows.empty() ? std::string("an unknown Windows regional format") : "Windows' regional format " + utf8(g_windows)) +
             ", and follows the game's language once it has picked it");
    std::thread([] {
        for (;;) {
            if (const char* t = game_tag()) {
                const std::string tag = t;
                const std::string now = language_name(tag) + " (" + tag + ")";
                const char* test      = g_test ? " (the test language stays on)" : "";
                const char* follows   = "follows it on every page written from now on, and the title screen's lines from the next launch";
                if (tag == g_start_tag)
                    plog("Language: the game's text is in " + now + ", as expected; mods' text is in it too" + test);
                else if (g_last.empty())   // no platform-language.ini yet: there was no last time
                    plog("Language: the game's text is in " + now + "; mods' text started in " + language_name(g_start_tag) +
                         " this first time (from Windows' regional format), and " + follows + test);
                else
                    plog("Language: the game's text is in " + now + ", not " + language_name(g_start_tag) + " as last time; mods' text " +
                         follows + test);
                if (tag != g_last) write_state(tag);
                return;
            }
            if (launched_game() == Launched::other) {   // Azimuth 1.0.1 started another Forza game: it never gets one
                plog("Language: the game that started isn't Forza Horizon 2, so the platform stops watching for its language");
                return;
            }
            Sleep(1000);
        }
    }).detach();
}

std::wstring l10n_table_text(FH2Mod* m, const std::string& key, const std::wstring& english, std::string_view table_code) {
    read_state();
    const char* t         = tag_for_code(table_code);
    const std::string tag = g_test ? "qps" : t ? t : "en";
    std::vector<std::string> notes;
    std::string text;
    {
        std::scoped_lock lk{g_mu};
        const std::string en = utf8(english);
        text                 = text_for(g_refs[ref_of(m, key, en, "", 0, false)], tag, notes);
        // The game formats its string tables' text itself ({0} to {6}, {{font_pop}}), and nobody knows what it does with
        // a brace it can't read: a translation whose braces don't fit the English's goes in as the English.
        if (text != en && !braces_fit(en, text)) {
            if (g_mods[m].stale_said.insert("{} " + tag + " " + key).second)
                notes.push_back("the " + tag + " text for \"" + key + "\" has braces that don't match the English's ({0}, {{...}}), which the "
                                "game's own text formatting could misread, so the " + std::string(table_code) +
                                " string table gets the English there");
            text = en;
        }
    }
    say(m, notes);
    return utf16(text);
}

void l10n_note(FH2Mod* m, const std::string& slot, const char* given) {
    std::scoped_lock lk{g_mu};
    const auto it = given ? g_handed.find(given) : g_handed.end();
    if (it != g_handed.end()) g_slots[{m, slot}] = it->second;
    else g_slots.erase({m, slot});
}

void l10n_note_key(FH2Mod* m, const std::string& slot, const std::string& key, const std::string& english) {
    std::scoped_lock lk{g_mu};
    g_slots[{m, slot}] = ref_of(m, key, english, "", 0, false);
}

void l10n_follow(FH2Mod* m, const std::string& slot, std::string& text) {
    {
        std::scoped_lock lk{g_mu};
        if (!g_slots.count({m, slot})) return;
    }
    l10n_follow(m, slot, text, l10n_language());
}

void l10n_follow(FH2Mod* m, const std::string& slot, std::string& text, const std::string& tag) {
    std::vector<std::string> notes;
    {
        std::scoped_lock lk{g_mu};
        const auto it = g_slots.find({m, slot});
        if (it == g_slots.end()) return;
        text = text_for(g_refs[it->second], tag, notes);
    }
    say(m, notes);
}

bool l10n_text_in(FH2Mod* m, const std::string& slot, std::string_view table_code, std::string& text) {
    read_state();
    const char* t         = tag_for_code(table_code);
    const std::string tag = g_test ? "qps" : t ? t : "en";
    std::vector<std::string> notes;
    {
        std::scoped_lock lk{g_mu};
        const auto it = g_slots.find({m, slot});
        if (it == g_slots.end()) return false;
        text = text_for(g_refs[it->second], tag, notes);
    }
    say(m, notes);
    return true;
}

void l10n_note_w(FH2Mod* m, const wchar_t* shown, const char* given, const std::wstring& suffix, std::size_t max_chars) {
    (void)m;
    if (!shown) return;
    std::scoped_lock lk{g_mu};
    const auto it = given ? g_handed.find(given) : g_handed.end();
    if (it != g_handed.end()) g_wslots[shown] = {it->second, suffix, max_chars};
    else g_wslots.erase(shown);
}

const wchar_t* l10n_follow_w(const wchar_t* shown) {
    if (!shown) return nullptr;
    {
        std::scoped_lock lk{g_mu};
        if (!g_wslots.count(shown)) return nullptr;
    }
    const std::string tag = l10n_language();
    std::vector<std::string> notes;
    const wchar_t* out = nullptr;
    FH2Mod* m          = nullptr;
    {
        std::scoped_lock lk{g_mu};
        const auto it = g_wslots.find(shown);
        if (it == g_wslots.end()) return nullptr;
        const Ref& r       = g_refs[it->second.ref];
        m                  = r.mod;
        std::string text   = text_for(r, tag, notes);
        // cut as the mod's text was: in characters (a Polish or Russian letter is one, as an English one), never inside one
        if (const std::size_t max = it->second.max_chars; max) text = cut_text(text, max);
        const std::wstring w = utf16(text) + it->second.suffix;
        auto in            = g_winterned.find(w);
        if (in == g_winterned.end()) {
            g_wtexts.push_back(w);
            in = g_winterned.emplace(w, &g_wtexts.back()).first;
        }
        out = in->second->c_str();
    }
    say(m, notes);
    return out;
}

} // namespace fh2platform
