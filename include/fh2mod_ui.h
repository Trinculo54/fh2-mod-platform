/*
 * fh2mod_ui.h -- FH2 Mod Platform: the UI templates (API 10), as calls that
 * do nothing on an older platform. Header-only, MIT licensed (see LICENSE).
 *
 * Each fh2ui_<call> takes the API member <call>'s own arguments, without
 * api and mod, and returns what it returns. On a platform whose FH2ModApi
 * hasn't got the member (checked by api->size), or before fh2_helpers_init,
 * it does nothing and returns 0, so a mod built against API 10 runs on API 9
 * without its templates and needs no version checks of its own:
 *
 *     #include "fh2mod_ui.h"
 *
 *     g_mod = api->register_mod(&info);
 *     fh2_helpers_init(api, g_mod);
 *     fh2ui_ui_uses(FH2_UI_USES_TOASTS | FH2_UI_USES_TIPS);
 *     ...
 *     if (!fh2ui_notify("Snapshot written", "WreckingBall-defs.txt, next to the game"))
 *         fh2ui_play_ui_sound(FH2_SOUND_SELECT);
 *
 * What to do instead when a call returns 0 is yours to choose. What docs/api.md
 * suggests: for a pop-up, your own path or a line in the log
 * (fh2ui_notify_or_log does the second); for a tip, a title-screen notice;
 * for a message box, a page row with a description; for page help and the
 * hint line, the help fragment and the row description you have today.
 */
#ifndef FH2MOD_UI_H
#define FH2MOD_UI_H

#include "fh2mod_helpers.h"

#include <string.h>

/* What works now, as FH2_UI_ bits; 0 on an older platform. */
static inline uint32_t fh2ui_ui_status(void) {
    return FH2_API_HAS(ui_status) ? FH2_API->ui_status(FH2_MOD) : 0;
}
/* What you'll use, as FH2_UI_USES_ bits. From fh2mod_init only. */
static inline int fh2ui_ui_uses(uint32_t uses) {
    return FH2_API_HAS(ui_uses) ? FH2_API->ui_uses(FH2_MOD, uses) : 0;
}
static inline int fh2ui_play_ui_sound(uint32_t sound) {
    return FH2_API_HAS(play_ui_sound) ? FH2_API->play_ui_sound(FH2_MOD, sound) : 0;
}

/* ---- Pop-ups ---------------------------------------------------------- */
static inline int fh2ui_show_toast(const FH2Toast* toast) {
    return FH2_API_HAS(show_toast) ? FH2_API->show_toast(FH2_MOD, toast) : 0;
}
static inline void fh2ui_clear_toast(const char* key) {
    if (FH2_API_HAS(clear_toast)) FH2_API->clear_toast(FH2_MOD, key);
}
/* One call for the commonest case: a pop-up with a title and a line (which
   may be NULL), with no key, icon or sound. */
static inline int fh2ui_notify(const char* title, const char* line) {
    FH2Toast t;
    memset(&t, 0, sizeof t);
    t.size     = sizeof t;
    t.title    = title;
    t.subtitle = line;
    return fh2ui_show_toast(&t);
}
/* fh2ui_notify, and when it isn't shown (pop-ups off, an older platform),
   "<title>: <line>" in platform.log instead. Returns what fh2ui_notify did. */
static inline int fh2ui_notify_or_log(const char* title, const char* line) {
    char text[256];
    size_t n = 0, i;
    const char* parts[3];
    if (fh2ui_notify(title, line)) return 1;
    if (FH2_API == NULL || FH2_API->log == NULL) return 0;
    parts[0] = title ? title : "";
    parts[1] = line && *line ? ": " : "";
    parts[2] = line ? line : "";
    for (i = 0; i < 3; ++i) {
        size_t len = strlen(parts[i]);
        if (len > sizeof text - 1 - n) len = sizeof text - 1 - n;
        memcpy(text + n, parts[i], len);
        n += len;
    }
    text[n] = '\0';
    FH2_API->log(FH2_MOD, text);
    return 0;
}

/* ---- Tips ------------------------------------------------------------- */
static inline int fh2ui_show_tip(const FH2Tip* tip) {
    return FH2_API_HAS(show_tip) ? FH2_API->show_tip(FH2_MOD, tip) : 0;
}
/* 1 if the tip `key` has been seen; 0 on an older platform. */
static inline int fh2ui_tip_seen(const char* key) {
    return FH2_API_HAS(tip_seen) ? FH2_API->tip_seen(FH2_MOD, key) : 0;
}
static inline void fh2ui_set_tip_seen(const char* key, int seen) {
    if (FH2_API_HAS(set_tip_seen)) FH2_API->set_tip_seen(FH2_MOD, key, seen);
}

/* ---- Message boxes ---------------------------------------------------- */
/* Only from a callback of your pages, settings or title entries. */
static inline int fh2ui_show_message_box(const FH2MessageBox* box) {
    return FH2_API_HAS(show_message_box) ? FH2_API->show_message_box(FH2_MOD, box) : 0;
}

/* ---- Pages: help bar and hint line ------------------------------------ */
static inline int fh2ui_set_page_help(const char* page_key, const FH2Prompt* prompts, uint32_t count) {
    return FH2_API_HAS(set_page_help) ? FH2_API->set_page_help(FH2_MOD, page_key, prompts, count) : 0;
}
static inline int fh2ui_set_page_hints(const char* page_key, const char* const* texts, uint32_t count) {
    return FH2_API_HAS(set_page_hints) ? FH2_API->set_page_hints(FH2_MOD, page_key, texts, count) : 0;
}
static inline int fh2ui_show_page_hint(const char* page_key, int index, uint32_t kind) {
    return FH2_API_HAS(show_page_hint) ? FH2_API->show_page_hint(FH2_MOD, page_key, index, kind) : 0;
}

#endif /* FH2MOD_UI_H */
