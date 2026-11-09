/*
 * fh2mod_helpers.h -- FH2 Mod Platform: what the header-only helpers share.
 * Header-only, MIT licensed (see LICENSE).
 *
 * Call fh2_helpers_init once in fh2mod_init, straight after register_mod:
 *
 *     g_mod = api->register_mod(&info);
 *     fh2_helpers_init(api, g_mod);
 *
 * It keeps the two pointers that the helpers in fh2mod_ui.h use, so their
 * calls take neither (fh2ui_notify("Snapshot written", NULL)). Before it's
 * called, or on a platform without the member a helper wraps, every helper
 * does nothing and returns 0.
 *
 * One copy of the two pointers per DLL. In C++ that's automatic (inline
 * variables, C++17). In C, define FH2_HELPERS_IMPLEMENTATION in exactly one
 * .c file before including this header (or fh2mod_ui.h); the others see the
 * pointers as extern.
 */
#ifndef FH2MOD_HELPERS_H
#define FH2MOD_HELPERS_H

#include "fh2mod.h"

#include <stddef.h>

#ifdef __cplusplus
inline const FH2ModApi* fh2_helpers_api = nullptr;
inline FH2Mod* fh2_helpers_mod          = nullptr;
#elif defined(FH2_HELPERS_IMPLEMENTATION)
const FH2ModApi* fh2_helpers_api = NULL;
FH2Mod* fh2_helpers_mod          = NULL;
#else
extern const FH2ModApi* fh2_helpers_api;
extern FH2Mod* fh2_helpers_mod;
#endif

/* The API and your handle, as fh2_helpers_init was given them (NULL before). */
#define FH2_API fh2_helpers_api
#define FH2_MOD fh2_helpers_mod

/* 1 if this platform's FH2ModApi has member `m` and it's set. It checks
   api->size rather than api_version, so it works the same for every block. */
#define FH2_API_HAS(m) \
    (FH2_API != NULL && FH2_API->size >= offsetof(FH2ModApi, m) + sizeof(FH2_API->m) && FH2_API->m != NULL)

/* Keeps `api` and `mod` for the helpers. Once, from fh2mod_init. */
static inline void fh2_helpers_init(const FH2ModApi* api, FH2Mod* mod) {
    fh2_helpers_api = api;
    fh2_helpers_mod = mod;
}

#endif /* FH2MOD_HELPERS_H */
