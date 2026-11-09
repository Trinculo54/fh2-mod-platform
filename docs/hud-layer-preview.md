# The HUD layer (preview)

**This is a preview, and it will change.** Platform 2.9.0 is the first release with the HUD layer, which it publishes as a preview interface; before that it was only in a test build, "2.8.0 + HUD layer preview 3", for the testers of a mod that needed it. It still isn't part of the API in `fh2mod.h`: it's an interface the platform publishes, and when it moves into the API its names, limits and behaviour may all change, and this interface may go. Never make it the only way your mod works.

The layer is a heads-up display of your own, drawn by the game's own UI inside its free-roam HUD page, in the game's look and fonts: a bar along the bottom of the screen with tabs, two status lines, a strip of marks and button prompts; small labels (chips) at the top; a tip card; a panel of rows; a countdown in the middle of the screen; and a white blink across the picture. You fill the parts with text and numbers; the platform owns the layout, so nothing you set can break the game's HUD page. The header, [`include/fh2mod_hud.h`](../include/fh2mod_hud.h), is the reference for every member; this page explains how it behaves. Testers of the old test build: see [Which builds have it](#which-builds-have-it) at the end.

## What the preview is, and isn't

- **Mods reach it through an interface, not the API:** `FH2Platform.HudLayer`, from `get_interface` (API 2). Platform 2.9.0 publishes version 2, which adds [the feed](#the-feed-290); the 2.8.0 test build had version 1. The platform publishes it itself, before any mod starts, so unlike other mods' interfaces it's there in `fh2mod_init`, and no mod can publish that name in its place.
- **Without a mod that adds a layer, it does nothing.** No HUD page is written for it, none of its hooks or threads is made, and `platform.log` has no `[HUD layer]` line.
- **It isn't for every mod.** One layer is on screen at a time, so two mods with bars would keep taking the screen from each other. If your mod could use it, ask first, and expect the interface to be replaced by an API in a later platform.

## Getting the interface

```cpp
#include "fh2mod.h"
#include "fh2mod_hud.h"   // from the preview's source; it includes fh2mod.h

static const FH2HudInterface* g_hud;   // null: no HUD layer, and the mod carries on without it

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    // ... register_mod first ...
    g_hud = api->api_version >= 2
                ? static_cast<const FH2HudInterface*>(api->get_interface(FH2_HUD_INTERFACE, FH2_HUD_INTERFACE_VERSION))
                : nullptr;
    if (!g_hud || g_hud->size < sizeof(FH2HudInterface)) {
        g_hud = nullptr;
        api->log(g_mod, "no HUD layer on this platform; carrying on without the bar");
    }
    // ... add_hud_layer here, if g_hud ...
    return 0;
}
```

- **On platform 2.8.0 and older**, `get_interface` returns null. Nothing in the header is linked, so a mod built with it still loads everywhere; it only has to work without the bar. If one of your features can't work without it, say so in your log or with `set_notice`.
- **Don't look at `api_version` to find the layer:** the 2.8.0 test build said 9, as 2.8.0 does. Only the interface tells. Asking for `FH2_HUD_INTERFACE_VERSION` (2) needs 2.9.0; ask for 1 instead, and check `version` before using the feed, if your mod should also run on the old test build.
- **Three checks before you rely on it:** the interface isn't null, `add_hud_layer` returned 1, and `hud_status` has `FH2_HUD_AVAILABLE`. The last one comes later in the session (see [Status](#status-on_status-and-the-lease)), and can go away again if the layer is turned off.

## Adding a layer

```c
int add_hud_layer(FH2Mod* mod, const FH2HudLayer* layer);

typedef struct FH2HudLayer {
    uint32_t size;               /* sizeof(FH2HudLayer) */
    uint32_t parts;              /* the FH2_HUD_PART_ bits you'll use */
    uint32_t lease_ms;           /* 250 to 60000, or 0 for the default of 5000 */
    FH2HudStatusFn on_status;    /* may be NULL */
    void* user;                  /* passed to on_status */
} FH2HudLayer;
```

- **From `fh2mod_init` only,** on the thread the platform calls it on. The HUD page is written once every mod's `fh2mod_init` has returned, so a call after that (or from a thread of your own) returns 0 and the log says why.
- **One layer per mod, four from every mod together.** `parts` says which parts you'll show; unknown bits are dropped with a log line, and a layer with none is refused. A `size` smaller than `sizeof(FH2HudLayer)` is refused too.
- **The page gets one copy of the layer,** with the parts of every mod's layer together. You can only show the parts you asked for here, even if another mod asked for more.
- **`lease_ms`:** every layer has a lease (see [the lease](#the-lease)). 0 means 5000 ms; a value outside 250 to 60000 is brought into that range, with a log line.
- **The first mod to add a layer also gets the "Button prompts" row** on its page under MODS > YOUR MODS (see [Button prompts](#button-prompts-and-the-device)). A second layer mod doesn't get another.
- It returns 1 if the layer was added (`[<mod>] HUD layer added: BAR, CHIPS; lease 2000 ms` in the log), 0 if not. Even then, wait for `FH2_HUD_AVAILABLE`: the platform leaves the layer out for the session when the game's code or UI files aren't as expected, for one session after the last one crashed between start-up and the HUD page's first 10 s on screen (whatever the crash was; see [the crash marker](#when-something-goes-wrong)), or when the game didn't read the platform's HUD page, and the log says which.

## The parts

Places are in the HUD page's own 1920 x 1080 layout, which the game scales to the screen (by 1.333 at 2560 x 1440). The game's HUD keeps 108 px from the sides and 56 px from the top; so does the layer.

| Part | Where | What's in it |
|---|---|---|
| `FH2_HUD_PART_BAR` | The bottom 84 px of the screen, full width, on a dark backing. A second row of 56 px sits on top of it, on the same backing, while the strip uses it. | Tabs on the left, two status lines in the middle, up to five button prompts on the right; the strip of marks on line 2 or on its own row. |
| `FH2_HUD_PART_CHIPS` | Four at the top left, in a row from (108, 56); one at the top right, ending 108 px from the edge. Each is 36 px tall, on dark glass. The top centre is left to the game's skill chain. | A word or two in capitals, and up to five small dots after it. |
| `FH2_HUD_PART_TIP` | A 640 x 44 card, centred, 16 px above the bar (above the strip's row while it shows, or 16 px above the bottom of the screen without the bar). | Text, one button, text: "Press (A) to drop a checkpoint". |
| `FH2_HUD_PART_PANEL` | 520 px wide at the left (x 108 to 628), its foot 16 px above the bar like the tip's (or 16 px above the bottom of the screen without the bar), so the two sit side by side. | Up to eight rows of 40 px, a label on the left and a value between `<` and `>` on the right, and a footer line. |
| `FH2_HUD_PART_COUNTDOWN` | A 220 px orange ring round the middle of the screen. | A big number inside it, and a label on dark glass just under the ring. |
| The shutter | The whole picture, over everything. | A white blink, from `pulse_hud`. It's always there and needs no part. |

The tip and the panel sit on top of the bar, so they move with its height: up by 56 px when the strip's own row comes, and down to the bottom of the screen when the bar goes while they stay. While the tip or the panel is fading out, the platform keeps the bar's height as it is (a change to the strip's row, the bar coming in, or the bar collapsing after its own fade waits until that fade is over, at most 250 ms), so a card that's leaving never jumps.

### The bar

- **Tabs** (`tabs`, up to 8): between the LB and RB pictures, the selected one (`tab`) as a light pill with dark letters; `tab` past the last tab selects none. With key caps showing (see [Button prompts](#button-prompts-and-the-device)) and both `tab_keys` given, those two keys show as key caps instead of LB and RB; give both or neither.
- **The middle**, centred between the tabs and the prompts and never wider than 760 px: `line1` in the HUD's medium text, with `line1_after` after it in light grey (`FH2_HUD_TEXT_DIM`), the HUD's yellow (`_WARN`) or the menus' orange (`_ACCENT`); and `line2` under it, smaller. `centre = FH2_HUD_CENTRE_ALERT` puts the middle on amber, for a warning.
- **The strip**, from `marks`:
  - `FH2_HUD_STRIP_DOTS`: up to 8 dots at the end of line 2, one for each of the first marks, in its style (white, orange for `FH2_HUD_MARK_CURRENT`, faded for `_DIM`), then `strip_after`. With `strip_row` set, up to 16 dots go on the strip's own row instead, centred on the screen (x 960), with `strip_after` at the row's right end.
  - `FH2_HUD_STRIP_RAIL`: a timeline on the strip's own row, 1104 px long (from x 204 to 1308), with `strip_left` and `strip_right` at its ends and `strip_after` to the right. Each mark sits at its `at` (0 to 1), as a dot or a diamond, with its `number` (1 to 999) under it. With `playhead` set, a thin white playhead sits at `FH2_HUD_PLAYHEAD` (see `set_hud_value`).
  - Mark numbers and diamonds are for the timeline only: dots are always plain dots.
- **Prompts** (`prompts`, up to 5): from the right end of the bar leftwards, so `prompts[0]` is the rightmost, as in the game's own help bars. Each is a controller picture (`glyph`, one of the game's own 24) or key caps (`key`), then its `label`. `chord` shows the picture as LB + RB + the button, like the platform's hotkey chords. `hold` puts "HOLD" before it, and `FH2_HUD_HOLD` fills an orange ring round its picture, or a bar under its key caps. A prompt with `glyph` 0 always shows its key caps; one with no `key` always shows its picture.
- **Key caps:** a `key` is split into two caps at the first `+` or `/` with text on both sides, so `"Ctrl+Z"` shows as two caps and `"Left/Right"` as two arrows. `Up`, `Down`, `Left` and `Right` (in any case) are drawn as arrows; other names are shown in capitals, 12 characters a cap.

### Chips, the tip and the panel

- **Chips** (`chips[0]` to `[3]` at the top left, `corner` at the top right): `text` in capitals, then `dots` small dots (0 to 5), the first `dots_filled` of them filled. A chip with no text and no dots isn't shown. At the top left about 15 capitals fit before the ellipsis, so the four chips can never reach the corner; the corner chip shows all 24.
- **The tip**: `tip_before`, then `tip_button` (its `glyph`, `chord` and `key`, as a prompt's; its `label` and `hold` are ignored), then `tip_after`, with the button in the middle of the card and each text trimmed to its half. A button with no glyph and no key isn't shown.
- **The panel**: `rows` (up to 8, `row_count`), each with a `label` in capitals and a `value`, in `FH2_ROW_NORMAL` white, `FH2_ROW_DIM` grey, or `FH2_ROW_HEADING` (the label in orange: a heading over the rows after it). `row_selected` fills that row in orange; a heading is never filled, and `row_count` or more selects none. `panel_footer` is a line under the rows.

### The countdown

`countdown` is the number in the ring, 1 to 999 (two digits fit inside it; three are wider than the ring), and 0 shows no number while the part stays shown: hide it with `show_hud_layer`. Each new number is meant to pop in, from a third bigger to its size; a number set together with the `show_hud_layer` that brings the countdown in pops as the ring fades in. In previews 1 and 2 no number popped in game; preview 3 scales the box round the number instead of the number's text, and that hasn't been seen in game yet. If it doesn't run, every number still shows at its full size, just without the pop. `countdown_label` is the line under the ring.

### The feed (2.9.0)

`FH2_HUD_PART_FEED` is a few lines of text at the left of the screen, over the game's minimap, newest at the bottom, like a chat log: each line a name in white with a colon after it, then the text in white, green, orange or yellow. It needs interface version 2.

- **It's shared.** Every mod whose layer has the feed posts lines with `post_hud_feed`, and they go into one feed of five lines; the oldest makes room. The feed is on screen while it has lines, whichever layer owns the screen, and it's never shown with `show_hud_layer`: `FH2_HUD_PART_ALL` leaves it out.
- **A layer with only the feed** never takes the screen, has no lease to keep, and doesn't get the Button prompts row: that goes to the first layer with a bar or a tip.
- **Each line stays** for its `ms` (8 s if 0, 2 to 30 s) of time with the HUD page on screen, so a line posted while the pause menu or the map is open waits for the HUD.
- **It stays out of the way.** It's hidden while a layer shows its bar or its panel (they cover the bottom left), while the game's own HUD is held hidden, and from a `show_hud_layer` with `FH2_HUD_PART_INSTANT` (a clean frame: it goes in that frame) until the next show without it. Its lines keep their time meanwhile.
- **Place:** the left edge is the HUD's 108 px margin and the bottom line's foot 360 px above the bottom of the 1920 x 1080 page, lines 36 px tall, the text trimmed at 720 px.

### Limits and text

Every text is UTF-8, converted once when you set it. Each is cut to its limit (counted in UTF-16 units, as Windows counts them, and never between the two halves of a surrogate pair), and the game also trims it with an ellipsis where its place is too narrow:

| Field | Most characters | In capitals | Trimmed at |
|---|---|---|---|
| `tabs[i]` | 16 | yes | 200 px |
| `tab_keys[i]` | 12, one cap each | yes | |
| `line1` | 40 | yes | 520 px |
| `line1_after` | 24 | | 240 px |
| `line2` | 64 | | 600 px |
| `strip_left`, `strip_right` | 12 | | 84 px |
| `strip_after` | 48 | | 384 px on the strip's row, 240 px on line 2 |
| a prompt's `key` | 12 a cap | yes | |
| a prompt's `label` | 24 | | 260 px |
| a chip's `text` | 24 | yes | 200 px at the top left, 320 px in the corner |
| `tip_before`, `tip_after` | 48 each | | 280 px each |
| a row's `label` | 32 | yes | 300 px |
| a row's `value` | 32 | | 200 px |
| `panel_footer` | 96 | | 488 px |
| `countdown_label` | 48 | | 600 px |
| a feed line's `name` | 24 | | 300 px |
| a feed line's `text` | 64 | | 720 px |

- **Line breaks, tabs and other control characters** (U+0000 to 001F, 007F to 009F, 2028 and 2029) become spaces: a line break would make a line twice as tall and push it off the bar.
- **Out of range:** counts over the limits use the first ones (`mark_count` 16, `prompt_count` 5, `row_count` 8); numbers over 999 show as 999; more than 5 chip dots show 5, and `dots_filled` past `dots` fills them all; and a value that isn't one of its field's (an unknown `strip` or `glyph`, say) counts as 0. Each kind of oddity is logged once for each kind of field, whatever the index (a long `tabs[1]` after a long `tabs[0]` is cut too, but not logged again), and so is a `reserved` member that isn't zero.
- **Room.** The tabs and prompts take the width they need first, and the middle gets what's left of the 1704 px between the side margins, trimming its text. Five short tabs and five prompts on key caps leave it about 300 px. Keep tabs and labels short, or the middle has little room; eight long tabs would push the prompts off the bar, and no layout can stop that.

**Letters.** In testing, the HUD's fonts showed English, the Western European letters (Latin-1: é, ü, ß, ø, å and the rest), Š š Ž ž, Polish (Ą ą Ć ć Ę ę Ł ł Ń ń Ó ó Ś ś Ź ź Ż ż) and Danish, plus curly quotes, the dashes and `…`. Cyrillic, Japanese and some other Latin letters (ĝ, ħ, ķ, ŕ, ţ and others) came out as boxes, never blanks. Czech, Slovak, Hungarian and Romanian letters (ř, ů, ě, ő, ű, ă, ș, ț) and Greek haven't been tried: expect boxes. The log says once when a text of yours has letters outside the set that was seen working (`set_hud_state: <field> has letters outside the set seen working in the HUD's fonts (U+xxxx); they may show as boxes`).

## The calls

Every call but `add_hud_layer` is quick and can be made from any thread. Each one copies what it's given before it returns, so a struct on your stack is fine. A null `mod`, or a mod with no layer, gets 0. Nothing reaches the game until the platform's next frame (see [Threads and cost](#threads-and-cost)).

### set_hud_state

```c
int set_hud_state(FH2Mod* mod, const FH2HudState* state);
```

Everything your layer shows apart from the two per-frame numbers. Zero the struct, set `size = sizeof(FH2HudState)`, and fill what you use; a null or `""` text leaves it out. Each call replaces the whole state, so pass all of it every time, and only when something changed. Only what changed reaches the page: a value set again costs nothing. A part that isn't shown keeps what you set for when it is (and costs nothing meanwhile), and a part fading out keeps showing what it had until it has gone. Fields of parts your layer doesn't have are ignored.

**Mind the order with `show_hud_layer`.** When parts go out, call `show_hud_layer` first and `set_hud_state` after it: a part keeps its words from the frame the platform sees it going out, so a new state that arrives a frame earlier (one without the tip's text, say) can empty the part before it fades. When parts come in, set the state first, so they have their words from their first frame. It returns 1, or 0 if `state` is null or `state->size` is too small (logged once).

### set_hud_value

```c
int set_hud_value(FH2Mod* mod, int which, float value);
```

A number that may change every frame, 0 to 1 (NaN counts as 0, and anything else is brought into range):

- `FH2_HUD_PLAYHEAD`: the playhead's place along the timeline, with `playhead` set and `strip = FH2_HUD_STRIP_RAIL`.
- `FH2_HUD_HOLD`: how full a held prompt's ring (or its bar, under key caps) is. There's one value for every held prompt, so show one at a time.

The newest value each frame wins. It returns 1, or 0 for an unknown `which`.

### show_hud_layer

```c
int show_hud_layer(FH2Mod* mod, uint32_t parts);
```

Which of your parts are on screen; 0 hides the whole layer.

- **Fades.** Parts fade in and out over 0.15 s. A part coming in waits until all of its contents have reached the page, so it never shows half filled, and always fades in. A part going out keeps its contents until it has faded, from the frame the platform sees the hide (so call `show_hud_layer` before `set_hud_state` when parts go out; see [set_hud_state](#set_hud_state)). Add `FH2_HUD_PART_INSTANT` to take the parts you're not asking for away at once, with no fade, for a clean frame.
- **One layer on screen at a time.** The newest `show_hud_layer` with parts, from any mod, owns the screen. Showing yours hides another mod's layer (it gets `FH2_HUD_TAKEN`), and that layer doesn't come back by itself: its mod has to show it again. If the mod on screen hides its layer, the screen stays empty.
- **What it clears:** your `FH2_HUD_TAKEN` and `FH2_HUD_LAPSED`, when `parts` isn't 0.
- Bits for parts your layer doesn't have are dropped, logged once. While the HUD page isn't on screen (a menu, the map), the change waits for it.
- It returns 1, or 0 if the layer is off this session (logged once: `show_hud_layer: the HUD layer is off this session (see the lines above)`). Before the platform has finished starting up, the request is kept and it returns 1.

### pulse_hud

```c
int pulse_hud(FH2Mod* mod, int pulse);
```

A one-off effect while your layer is shown. The preview has one, `FH2_HUD_PULSE_SHUTTER`: a white blink across the whole picture, starting nearly white and gone in a tenth of a second, to confirm a change (a checkpoint placed, say). Play `FH2_SOUND_SHUTTER` with it for the sound. "Shown" means your newest `show_hud_layer` had parts, no other mod's came after it, and the platform hasn't hidden it since (your lease running out, or `on_status` crashing), so a pulse right after `show_hud_layer` is fine. A pulse while the last one is still on restarts it a frame later. A pulse made while the HUD page is off screen is dropped if the page isn't back within 0.4 s. It returns 1, or 0 if the layer isn't shown, the pulse is unknown or the layer is off (each logged once).

### hud_status

```c
uint32_t hud_status(FH2Mod* mod);
```

Your layer's `FH2_HUD_` bits now (see [Status](#status-on_status-and-the-lease)). Cheap, from any thread. Call it from the code that drives your layer: like every call, it keeps your lease.

### hold_game_hud

```c
int hold_game_hud(FH2Mod* mod, int hold);
```

Hides the game's own HUD while you hold it (1): its speedometer, map, prompts and notifications, but not your layer, and not the Drivatars' name tags. Read [The game's own HUD](#the-games-own-hud) for what that holds back.

- **Holds are counted across mods:** the game's HUD comes back when the last one lets go (0), or when a holder's lease runs out.
- **It's put back.** The game's own flow resumes its HUD at times (after the rewind, and as driving starts again after an activity), so while a hold is on, the platform hides it again on any frame it has come back.
- **It works from the next frame;** `FH2_HUD_GAME_HIDDEN` says when it's in force. After a world is loaded, the hold applies to the new world's HUD once it's found.
- **Your hold stays while another mod's layer has the screen** (`FH2_HUD_TAKEN`): show yours again or let go, or the player has neither HUD.
- **Pop-ups and tips wait while it's held.** Every mod's pop-ups and tips (API 10's `show_toast` and `show_tip`) go on the game's notification, which is part of the HUD you hide, so they wait until the last hold is let go, as the game's own notifications do. Their `ui_status` says `FH2_UI_GAME_HUD_HIDDEN` meanwhile, and the time held doesn't count towards a pop-up's expiry or a tip's driving time, so a pop-up sent while you hold (a reply to a key press included) shows once you let go. Hold only while your bar is up.
- `hold_game_hud(mod, 1)` also clears `FH2_HUD_LAPSED`, so a mod that only holds (to clear the screen, with no layer shown) can hold again after a lapse.
- It returns 1, or 0 if the layer is off this session, if the game's HUD code isn't as expected, or if hiding or showing the game's HUD crashed earlier this session (logged once; the game's HUD then stays, and a hold that was on is let go). Each change is logged: `[<mod>] hold_game_hud: held` and `let go`.

You need a layer to hold the game's HUD, even if you never show it.

### play_ui_sound

```c
int play_ui_sound(FH2Mod* mod, uint32_t sound);
```

Plays one of the game's own UI sounds once, at the next frame. Never make a sound the only cue. Platform 2.9.0's API has the same call for every mod, with no layer needed, all eleven sounds and the same player (`play_ui_sound` in [api.md](api.md)): use that one in new code. This one has its own gate and its own repeat rule. It needs the MODS tab's hooks, so in a session where they didn't go in it plays nothing (the log says the sound player isn't ready) while the API's still plays; and its 100 ms between repeats is counted apart from the API's, so a sound sent through both can play twice in one frame. Use one of the two for a sound, not both.

| Sound | What it is | Where it plays |
|---|---|---|
| `FH2_SOUND_ERROR` | the menus' "can't do that" | anywhere; only a low thump in the world |
| `FH2_SOUND_NOTIFY` | the game's notification chime | in the world only |
| `FH2_SOUND_INFO` | a pop-up opening | anywhere; very faint in the world |
| `FH2_SOUND_TICK` | the race countdown's beep | in the world only |
| `FH2_SOUND_TICK_LOW` | its lower beep | in the world only |
| `FH2_SOUND_TICK_END` | its last beep | in the world only |
| `FH2_SOUND_SHUTTER` | the photo shutter | anywhere |

- **The world-only four** are silent under the game's menus, so they're refused while the free-roam HUD page isn't on screen (a menu, the map, photo mode), with a log line once for each. ERROR, INFO and SHUTTER were heard under the pause menu too.
- **The same sound at most once per 100 ms** from your mod; a repeat returns 0, unlogged.
- **At most one sound a frame** is played, oldest first, and four can wait for each mod (a fifth drops the oldest). A sound still waiting after 0.5 s, or a world-only one whose page went off screen before it could play, is dropped with a log line once.
- It returns 1 if the sound is on its way, 0 if not: an unknown sound, a repeat, a world-only sound off the world, the layer off this session, or the game's sound code not as expected (each logged once, the repeat apart). Played sounds are logged as `[<mod>] sound: SHUTTER played`. The sounds play through the same player as the MODS tab's, so they need its hooks to be in.

## Status, on_status and the lease

`hud_status` returns these bits, worked out once a frame by the platform. Before the platform has finished starting up (a few seconds after the game's code is ready), it returns 0.

| Bit | Set while |
|---|---|
| `FH2_HUD_AVAILABLE` | the platform's hooks for the layer are on, the layer isn't off this session, and either no HUD page has been shown yet or the page shown has the layer |
| `FH2_HUD_SHOWN` | your layer owns the screen with parts (your newest `show_hud_layer` had parts and no other mod's came after it) |
| `FH2_HUD_ON_SCREEN` | the free-roam HUD page is on screen, so a shown layer is seen |
| `FH2_HUD_CLEAN` | nothing of the HUD layer has been drawn for 3 frames: a capture is clean of it. Set from start-up until the layer is first shown. The same for every layer |
| `FH2_HUD_KEYBOARD` | the prompts show key caps |
| `FH2_HUD_GAME_HIDDEN` | the game's own HUD is held hidden now, by any mod |
| `FH2_HUD_TAKEN` | another mod's layer took the screen; until your next `show_hud_layer` with parts |
| `FH2_HUD_DEVICE_FORCED` | the player chose Controller or Keyboard and mouse for the prompts |
| `FH2_HUD_LAPSED` | your lease ran out: the layer was hidden and your hold let go; until your next `show_hud_layer` with parts, or `hold_game_hud(mod, 1)` |

**`on_status(user, bits)`** is called when your layer's bits change, on the game's UI thread at the platform's frame, with no platform lock held. Make it quick (store the bits, set a flag) and don't call into the game from it; this interface's calls are fine. If it crashes, it isn't called again, your layer is hidden at once and your hold is let go (a later `show_hud_layer` or `hold_game_hud(mod, 1)` still works). When the layer is turned off for the session, the bits change once more (`AVAILABLE` and `SHOWN` go) and then stay as they are.

### The lease

Every layer has a lease, `lease_ms` (5000 by default), so that code of yours that stopped can't leave the player without the game's HUD, or with your bar frozen on screen. While your layer is shown (`FH2_HUD_SHOWN`) or your hold is on, and the HUD page is on screen, the platform counts the time since your last call. Any of the calls counts, `hud_status` and `set_hud_value` included. When the count reaches the lease, your layer is hidden (with a fade), your hold is let go, `FH2_HUD_LAPSED` is set and the log says `[<mod>] HUD layer: no call for 2.0 s while on screen, so it's hidden and its hold on the game's HUD is let go`. Time with the HUD page off screen (a menu, the map, a loading screen) doesn't count. Call `hud_status` or `set_hud_value` a few times a second from the code that drives your layer, not from a thread of its own whose only job is to keep the lease.

## Where it shows, and clean frames

The layer is part of the free-roam HUD page, so it's only seen while that page is on screen (`FH2_HUD_ON_SCREEN`). In testing:

- **The pause menu, the map, photo mode, the rewind (with its loading screen) and the telemetry HUD** each put another page in the HUD page's place, and the layer went with it in the same frame, coming back with the HUD page's own fade-in. Whether a photo-mode picture could ever contain the layer wasn't checked; the bar left the screen as photo mode opened.
- **The LimitedHud pages,** cut-down HUD pages for some of the game's own moments, which are expected to be built from the same file, never show the layer: the platform gives them nothing to show. None was seen in testing.
- **Replays, event intros and the game's other cutscenes** haven't been checked. `FH2_HUD_ON_SCREEN` is the one to trust.

A frame that must be clean of the layer (a capture, say) is yours to clear: `show_hud_layer(mod, FH2_HUD_PART_INSTANT)`, then wait for `FH2_HUD_CLEAN`, which comes 3 frames after the hide reached the page (in testing a hide reached the screen within two game frames). For the game's own HUD, hold it as well and wait for `FH2_HUD_GAME_HIDDEN`. The Drivatars' name tags, and anything another program draws over the game, stay.

## The game's own HUD

The bar covers the bottom of the screen, where the game puts its speedometer, map and notifications, so show the bar with the game's HUD held hidden (`hold_game_hud`). Hold it while your bar is up, and no longer. In testing, while it was held:

- the game's small notifications waited, and showed in full with their chime once it was let go; other mods' pop-ups that use the game's notifications (Achievements' unlocks) should wait the same way, though that hasn't been seen yet, and the platform's own pop-ups and tips wait by design (see [hold_game_hud](#hold_game_hud));
- its big banners waited too: one waited over two minutes;
- the autosave spinner waited;
- the prompts and markers the game puts in the world ("Press (A) to start an event" among them) that came up while it was held were gone when it was let go, rather than kept for later, and a Rivals message that was waiting was dropped;
- a few of the game's skill messages that were waiting flashed for about a frame each as it was let go;
- the Drivatars' name tags stayed.

Letting go brings the speedometer and map back in one frame.

## Button prompts and the device

Prompts follow the device the player used last: the game's own controller pictures, or key names on key caps.

- **Automatic** (the default): the newest input wins. A key going down, a mouse button or the mouse moving (in two readings running and by 3 px or more, so the window placing the pointer doesn't count) picks key caps; a button, a trigger or a stick on an Xbox-style (XInput) pad picks pad pictures. The modifiers, the Windows keys, any key while a Windows key is down (Win+Alt+R starts a recording, say) and the platform's hotkeys' own keys don't count. It switches at most every 150 ms, and only watches while the game is in front. At start-up it shows pad pictures if an XInput pad is connected, otherwise key caps; until it knows, key caps.
- **The "Button prompts" row:** Automatic, Controller or Keyboard and mouse, on the first layer mod's page under Pause > MODS > YOUR MODS (Pause > Settings > MODS opens the same list). A change applies straight away and is kept in `fh2-mods\platform-hudlayer.ini`. Controller or Keyboard and mouse there also decides how every mod's tips (API 10's `show_tip`) write their buttons, and what `ui_status`'s `FH2_UI_KEYBOARD` says; Automatic leaves both following the device used last.
- **Pads that aren't XInput** (a DualSense without Steam Input, or a keyboard that Azimuth reads as a controller) aren't seen: choose Controller.
- `FH2_HUD_KEYBOARD` and `FH2_HUD_DEVICE_FORCED` say what the prompts show and whether the player chose.

To follow the keyboard, the platform reads each key's state (`GetAsyncKeyState`) every 30 ms while the game is in front, as its hotkeys do. It keeps no record of what was typed: the log only says when the prompts change, with the input that changed them (`[HUD layer] device: keyboard and mouse (key 0x41) at 52310 ms`), and stops after 200 such lines a session.

## Threads and cost

- **Your calls only stage.** They convert and copy what you give them under a short lock of your layer's own, and return.
- **The platform's frame** runs once a frame on the game's UI thread, inside the game's UI lock (at the AchievementsModule's tick, `game+0x3FCD84`, which Achievements hooks too: instruction hooks there chain). It reads the newest of what each mod set, works out what changed, and tells the HUD page. If your call holds your layer's lock at that very moment, the change waits a frame.
- **A big change arrives over a few frames:** at most 24 values a frame, oldest first, with hides, the playhead and the hold ring outside that budget. A whole new bar of about 90 values needs about 4 frames, and hiding is never held back.
- **One text changes at most 10 times a second;** a faster one shows its newest every tenth of a second. So set text only when it changes.
- **Cost.** Values that don't change cost nothing. In testing, a frame that changes anything cost the game's UI about 0.15 ms, plus about 0.04 ms for each value told (measured as 0.035 ms with a smaller page; this one has more bindings, so the figure is an estimate). A playhead or a hold ring moving every frame costs about 0.2 ms a frame.
- **`on_status`** runs at the platform's frame, as above.

## A worked example

A time-trial editor: the player drives a route and drops checkpoints along it, then tries it out with a test run. Its bar has three tabs, the checkpoint being edited, a timeline of the route with its checkpoints, and four prompts.

```cpp
static const FH2HudInterface* g_hud;     // from get_interface, as above
static std::atomic<uint32_t> g_hud_bits{0};

// in fh2mod_init, after get_interface
if (g_hud) {
    FH2HudLayer layer{};
    layer.size      = sizeof layer;
    layer.parts     = FH2_HUD_PART_BAR | FH2_HUD_PART_CHIPS | FH2_HUD_PART_TIP | FH2_HUD_PART_COUNTDOWN;
    layer.lease_ms  = 2000;
    layer.on_status = [](void*, uint32_t bits) { g_hud_bits = bits; };
    if (!g_hud->add_hud_layer(g_mod, &layer)) g_hud = nullptr;
}

// the editor's bar, set again whenever something on it changes
void set_editor_bar() {
    FH2HudState s{};
    s.size    = sizeof s;
    s.tabs[0] = "Route";
    s.tabs[1] = "Checkpoints";
    s.tabs[2] = "Test";
    s.tab     = 1;
    s.tab_keys[0] = "Q";
    s.tab_keys[1] = "E";
    s.line1             = "Checkpoint 2 of 3";   // shown in capitals
    s.line1_after       = "MOVED";
    s.line1_after_style = FH2_HUD_TEXT_WARN;
    s.line2             = "Drive to where it should be, then Place";
    s.strip       = FH2_HUD_STRIP_RAIL;          // the route as a timeline
    s.strip_left  = "Start";
    s.strip_right = "Finish";
    s.strip_after = "3 checkpoints, 4.2 km";
    s.marks[0]    = {0.30f, FH2_HUD_MARK_DIAMOND, FH2_HUD_MARK_NORMAL, 1};
    s.marks[1]    = {0.62f, FH2_HUD_MARK_DIAMOND, FH2_HUD_MARK_CURRENT, 2};
    s.marks[2]    = {0.88f, FH2_HUD_MARK_DIAMOND, FH2_HUD_MARK_NORMAL, 3};
    s.mark_count  = 3;
    s.playhead    = 1;                           // where the car is along the route
    s.prompts[0]  = {FH2_GLYPH_B, 0, "Backspace", "Leave", 0};   // the rightmost
    s.prompts[1]  = {FH2_GLYPH_Y, 0, "Enter", "Test run", 0};
    s.prompts[2]  = {FH2_GLYPH_X, 0, "Ctrl+Z", "Undo", 0};
    s.prompts[3]  = {FH2_GLYPH_A, 0, "Space", "Place", 0};
    s.prompt_count = 4;
    s.chips[0]   = {"Editing"};
    s.chips[1]   = {"Ghost on"};
    s.tip_before = "Press";
    s.tip_button = {FH2_GLYPH_A, 0, "Space"};
    s.tip_after  = "to drop a checkpoint";
    g_hud->set_hud_state(g_mod, &s);
}

// opening the editor
if (g_hud && (g_hud->hud_status(g_mod) & FH2_HUD_AVAILABLE)) {
    set_editor_bar();
    g_hud->show_hud_layer(g_mod, FH2_HUD_PART_BAR | FH2_HUD_PART_CHIPS | FH2_HUD_PART_TIP);
    g_hud->hold_game_hud(g_mod, 1);
}   // else: the editor works without its bar, from its own keys and log

// every frame while it's open, from the code that runs the editor (this also keeps the lease)
g_hud->set_hud_value(g_mod, FH2_HUD_PLAYHEAD, metres_driven / route_metres);

// a checkpoint placed
g_hud->pulse_hud(g_mod, FH2_HUD_PULSE_SHUTTER);
g_hud->play_ui_sound(g_mod, FH2_SOUND_SHUTTER);

// a test run: 3, 2, 1 in the ring, a beep each, then go
//   s.countdown = 3; s.countdown_label = "Test run"; set_hud_state, then
g_hud->show_hud_layer(g_mod, FH2_HUD_PART_BAR | FH2_HUD_PART_COUNTDOWN);
g_hud->play_ui_sound(g_mod, FH2_SOUND_TICK);
//   ... 2, 1 a second apart, then FH2_SOUND_TICK_END and the countdown hidden again

// leaving the editor
g_hud->show_hud_layer(g_mod, 0);
g_hud->hold_game_hud(g_mod, 0);
```

## When something goes wrong

The platform checks everything it relies on before it uses it, and turns off only what doesn't match, with a line in `platform.log`; the game itself is never affected.

- **Left out for the session** (`FH2_HUD_AVAILABLE` never set, `show_hud_layer` and `hold_game_hud` return 0): the page check failed (the game's `UI.zip` isn't as expected), the page couldn't be written, the game's code for the layer isn't as expected (or the game exe couldn't be read to check it), a hook was refused, the platform's menu hooks didn't go in, or the crash marker left it out.
- **Off from then on** (`FH2_HUD_AVAILABLE` cleared): the HUD page was shown without the layer's values (the game didn't read the platform's page, for example with mediaoverride off), the game used the HUD page from another thread than the platform's frame, or the game's code faulted while the layer was being updated. The hold is let go, and the layer is hidden where that's still safe; in the rare case that telling the page anything isn't, what it showed last can stay on screen for the session. None of these has happened in testing.
- **Only one piece off:** the game-HUD hold (`hold_game_hud` returns 0, and a hold that was on is let go) or the sounds (`play_ui_sound` returns 0) when their code isn't as expected, or after one of them faulted.
- **The crash marker.** From the moment the page with the layer is written at start-up until the HUD page has been on screen for 10 s, the platform keeps `fh2-mods\platform-hudlayer-marker.txt`. If the next launch finds a crash report newer than the marker (whatever the crash was), or the page started loading in two sessions running and never reached 10 s on screen in either, that launch leaves the layer out, and the title screen says "HUD layer: left out after a crash. See platform.log." Only that one session goes without it: the launch after tries again.

## Which builds have it

- **Platform 2.9.0 and newer** have the layer (interface version 2), so every mod zip that ships 2.9.0 brings it. Line 1 of `fh2-mods\platform.log` says which platform runs.
- **The "2.8.0 + HUD layer preview 3" test build** had version 1, without the feed. It reported itself as 2.8.0, so line 1 of its `platform.log` ended "this is the 2.8.0 + HUD layer preview 3 build, not a release", and its Product version (Properties > Details) said the same. Testers who still have it: replace it with 2.9.0, as both `d3dcompiler_47.dll` and `DWrite.dll`.
- **An older platform put back** by a mod zip that ships 2.8.0 or older takes the layer away, and the mod that needs it finds no interface: reinstall a mod that ships 2.9.0. Going back to 2.8.0 on purpose, delete `fh2-mods\notices\HUD layer.txt` if it's there (it only is after a session the crash marker left the layer out of, and 2.8.0 wouldn't clear it); the layer's other files can stay, and 2.8.0 removes its HUD page again at the next launch.

What the platform writes while a mod has a layer:

| File | What it is |
|---|---|
| `UI\Scenes\Anthem\A_Hud.xaml` | the HUD page with the layer, written where the platform writes its other pages (the game's `mediaoverride` folder, or from 2.9.0 on Azimuth 1.0 or later without Extra Options, the Azimuth folder's own layer), every launch. It's taken out again at the first launch without a layer. |
| `fh2-mods\platform-hudlayer-A_Hud.xaml` | a copy of that page, for checking it |
| `fh2-mods\platform-hudlayer-props.txt` | the page's properties, one a line: name, type, default and values |
| `fh2-mods\platform-hudlayer-marker.txt` | the crash marker, gone 10 s after the HUD page is first on screen |
| `fh2-mods\platform-hudlayer.ini` | the Button prompts choice, made the first time the row is changed; `Debug=1` under `[HudLayer]` (added by hand) puts the layer's diagnostics in the log |
| `fh2-mods\notices\HUD layer.txt` | the title-screen line, after a session the crash marker left the layer out of |

`fh2-mods\platform-session.json` gets `"hud_layer": "2.9.0 preview: <mods with a layer>"` (the build's own name in place of 2.9.0, for a build that isn't a release), and every crash report a line `platform: FH2 Mod Platform <build>` after the CRASH line, plus a `HUD layer:` line saying what the layer was doing.

**Diagnostics.** The layer's stats every 10 s, each page its getters meet, and every time the HUD page is shown or hidden go to `platform.log` only with `Debug=1` under `[HudLayer]` in `fh2-mods\platform-hudlayer.ini`, or `Developer=1` under `[popups]` in `fh2-mods\platform-popups.ini`, read as the game starts. Without either, the log has the first time the HUD page is shown and hidden, and the rest of the lines below. Turn it on for a session you'll send back about the layer.

**What to send back:** `fh2-mods\platform.log` from the session (the platform also keeps each session's copy in `fh2-mods\platform-logs\`, so an older one isn't lost when the game starts again), any `fh2-mods\crashes\crash-*.txt` from it, and the log of the mod that uses the layer.

### Log lines worth searching for

The layer's own lines start with `[HUD layer]`; lines about one mod's layer carry that mod's name.

| Line | What it says |
|---|---|
| `[HUD layer] 1 mod(s) asked for a HUD layer: ...; the HUD page gets BAR, CHIPS, ...` | a mod added a layer, and which parts the page gets |
| `[HUD layer] crash marker: ...` | what the last session left: `none from last session`, `the layer stays in`, or why it's left out |
| `[HUD layer] page: wrote A_Hud.xaml with the layer (...)` / `page: the layer is left out: ...` | the page was checked and written, or why not |
| `[HUD layer] code: <group>: as expected` / `doesn't match the exe on disk; ...` | the game's code for the layer, group by group |
| `[HUD layer] the HUD layer is off: the game exe couldn't be read from the game folder ...` | there was no exe to check the layer's code against, so none of it was used |
| `[HUD layer] hooks: 4 of 4 made and on; the HUD layer is ready` | start-up is done |
| `[HUD layer] HUD page 0x... (Hud) shown at frame ...` / `hidden` / `destroyed` | the HUD page coming and going (shown and hidden the first time in a session, and every time with `Debug=1`) |
| `[HUD layer] the HUD page was shown without the layer's values ...` | the game didn't read the platform's page; the layer is off |
| `[<mod>] HUD layer shown: ...` / `HUD layer hidden ...` / `... showed its layer, so this one is hidden` | showing and hiding, and the screen changing hands |
| `[<mod>] HUD layer: no call for ... s while on screen ...` | a lease ran out |
| `[HUD layer] hold: the game's HUD is hidden (...) for <mods>` / `hold: let go` | the game-HUD hold |
| `[HUD layer] button prompts: ...` / `device: ...` | the Button prompts choice, and the device as it changes |
| `[<mod>] sound: ... played` / `dropped: ...` | sounds |
| `... crashed in the game's code ...` | a fault, and what it turned off |
| `[HUD layer] stats: ...`, then `Strings: ...` | with `Debug=1`, every 10 s: frames, cost, values told, and the texts in use. The `Strings` line's "left with the game" should stay near 0, and its references steady while the car is parked. "left with the page while off screen" may grow by one for each text that changed during a visit of more than 2 s to the pause menu or the map: the game frees those once the HUD page is back, so only "left with the game" growing points to a leak |
