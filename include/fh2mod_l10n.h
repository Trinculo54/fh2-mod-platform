/*
 * fh2mod_l10n.h -- FH2 Mod Platform: helpers for your text in the player's
 * language (API 10). Header-only, MIT licensed (see LICENSE).
 *
 * The workflow (docs/translating.md):
 *   1. Write your text with FH2_TR / FH2_TR_PLURAL, each with a key and its
 *      English: FH2_TR(api, mod, "row.Trees.label", "Breakable trees").
 *   2. tools/l10n/extract.py lists them in l10n/<domain>.pot, which
 *      translators turn into l10n/pl.po, l10n/pt_BR.po...
 *   3. tools/l10n/po2inc.py builds those into a .inc of FH2Translation
 *      entries; include it and call FH2_ADD_TRANSLATIONS after register_mod.
 *
 * Everything here works on an older platform too: FH2_TR then gives the
 * English, and FH2_ADD_TRANSLATIONS does nothing.
 */
#ifndef FH2MOD_L10N_H
#define FH2MOD_L10N_H

#include "fh2mod.h"

#include <stddef.h>
#include <string.h>

/* The platform has the API 10 member `m` (api_version, and the table that big). */
#define FH2_L10N_HAS(api, m) \
    ((api) && (api)->api_version >= 10 && (api)->size >= offsetof(FH2ModApi, m) + sizeof((api)->m) && (api)->m)

/* `english` in the game's language; `english` itself before API 10. `key`
   and `english` are string literals, so extract.py can find them. Text you
   pass on to the platform as it is (a row's label, say) follows the game's
   language by itself. */
#define FH2_TR(api, mod, key, english) fh2_tr((api), (mod), (key), (english))
/* The form of `one` / `other` for `n`, with {n} still in it: fill it with
   fh2_fill. Before API 10, `one` for 1 and `other` for anything else. */
#define FH2_TR_PLURAL(api, mod, key, one, other, n) fh2_tr_plural((api), (mod), (key), (one), (other), (n))
/* Marks FH2ModInfo.description for extract.py: it goes in the catalogue
   under the key "mod.description", which the platform looks up itself.
     info.description = FH2_TR_DESCRIPTION("Breakable trees and fences."); */
#define FH2_TR_DESCRIPTION(english) (english)

static inline const char* fh2_tr(const FH2ModApi* api, FH2Mod* mod, const char* key, const char* english) {
    return FH2_L10N_HAS(api, tr) ? api->tr(mod, key, english) : english;
}

static inline const char* fh2_tr_plural(const FH2ModApi* api, FH2Mod* mod, const char* key, const char* one, const char* other,
                                        int64_t n) {
    return FH2_L10N_HAS(api, tr_plural) ? api->tr_plural(mod, key, one, other, n) : (n == 1 ? one : other);
}

/* One language's PO file, built into the DLL by tools/l10n/po2inc.py. */
typedef struct FH2Translation {
    const char* lang; /* "pl", "pt-BR" */
    const char* po;   /* the PO file's text, UTF-8 */
    size_t size;      /* its length in bytes */
} FH2Translation;

/* Hands each of `count` translations to the platform (API 10). Returns how
   many it took; 0 on an older platform. */
static inline int fh2_add_translations(const FH2ModApi* api, FH2Mod* mod, const FH2Translation* list, size_t count) {
    int taken = 0;
    size_t i;
    if (!FH2_L10N_HAS(api, add_translation)) return 0;
    for (i = 0; i < count; ++i)
        if (list[i].lang && api->add_translation(mod, list[i].lang, list[i].po, list[i].size) > 0) ++taken;
    return taken;
}
/* The same for an array po2inc.py wrote: FH2_ADD_TRANSLATIONS(api, mod, mymod_translations). */
#define FH2_ADD_TRANSLATIONS(api, mod, list) fh2_add_translations((api), (mod), (list), sizeof(list) / sizeof((list)[0]))

/* `tmpl` into `out` (at most n bytes, with its NUL) with each {name} in
   `names` replaced by the same entry of `values` ("count", not "{count}").
   Other braces are copied as they are. Returns the length the whole text
   needs, as snprintf does: `out` is cut short (but ends in a NUL) when
   that's n or more. */
static inline size_t fh2_fill_c(char* out, size_t n, const char* tmpl, const char* const* names, const char* const* values,
                                size_t count) {
    size_t len = 0, i;
    const char* p = tmpl ? tmpl : "";
    while (*p) {
        const char* text = NULL;
        size_t skip = 1, tl = 1;
        if (*p == '{') {
            for (i = 0; i < count; ++i) {
                const size_t nl = strlen(names[i]);
                if (strncmp(p + 1, names[i], nl) == 0 && p[1 + nl] == '}') {
                    text = values[i] ? values[i] : "";
                    skip = nl + 2;
                    break;
                }
            }
        }
        if (!text) text = p;
        else tl = strlen(text);
        if (out && n && len < n - 1) {
            const size_t room = n - 1 - len;
            memcpy(out + len, text, tl < room ? tl : room);
        }
        len += tl;
        p += skip;
    }
    if (out && n) out[len < n ? len : n - 1] = '\0';
    return len;
}

#ifdef __cplusplus
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>

/* C++: fh2_fill(tr_text, {{"count", "12"}, {"name", name}}) */
inline std::string fh2_fill(const char* tmpl, std::initializer_list<std::pair<const char*, std::string>> values) {
    std::string out = tmpl ? tmpl : "";
    for (const auto& v : values) {
        const std::string mark = std::string("{") + v.first + "}";
        for (std::size_t at = out.find(mark); at != std::string::npos; at = out.find(mark, at + v.second.size()))
            out.replace(at, mark.size(), v.second);
    }
    return out;
}
/* A number in the game's language for fh2_fill (format_number; plain digits
   before API 10). */
inline std::string fh2_number(const FH2ModApi* api, double v, int decimals = 0) {
    char b[64];
    if (FH2_L10N_HAS(api, format_number) && api->format_number(v, decimals, b, sizeof b) > 0) return b;
    std::snprintf(b, sizeof b, "%.*f", decimals < 0 ? 0 : decimals > 6 ? 6 : decimals, v);
    return b;
}
#endif

#endif /* FH2MOD_L10N_H */
