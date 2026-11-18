# FH2 Mod Platform 2.9.1: release notes

A small release on top of 2.9.0, mostly for mod makers: a mod can now draw a heads-up display entirely of its own, put its text into one of the game's own pages, and keep its controller buttons out of the menus. For players, long lists of choices are easier to pick from. API 11, which only adds to API 10.

## For players

### What's new

- **Long lists of choices are easier.** A row on a mod's page can now offer up to 100 choices (it was 32), so a mod no longer has to split a long list over two rows. On a row with more than eight choices, press **A** to pick from a list instead of stepping through them with left and right; the row's description says "Press A to pick from a list."
- **Mods can draw their own displays in the game's HUD.** Up to now a mod's heads-up display was made of ready-made pieces: a bar, labels, a feed. A mod can now bring a drawing of its own, such as a set of flight instruments or a gauge, and the game draws it in its own HUD as you drive. The platform checks each drawing before the game loads it, and leaves out one that the game couldn't load rather than let it take the game down.
- **Mods can put their own text on the game's own screens.** The name tag over another car, say, can now carry a line from a mod, a different one for each car.
- **A mod's controller buttons can stay out of the menus.** A mod that takes buttons while you hold another (the d-pad while LB is held, say) can now say they are for driving only. In the pause menu, the map, photo mode and on the title screen those buttons are then the game's again, so holding LB and moving through a menu no longer sets the mod off. Mods have to be updated to use it.

### What to do

Install as always: extract the zip into your Azimuth folder, with Azimuth closed. Every mod's zip ships the platform it was built with, so if you use several mods, extract the one with the newest platform last. Nothing in 2.9.1 needs your mods to be updated: a mod built for 2.9.0 or older works as it did.

## For mod makers

### New in API 11

- **Page texts** (`add_page_text`, `page_text_changed`): a text of yours in one of the game's own pages, for the pages the platform's templates don't reach. You replace the page with `set_file`, bind a text block in it to `{Binding FH2Mod_Text_<name>}`, and the platform asks your callback for the text each time a page object reads it, so each name tag over a car can have its own. [api.md](api.md#page-texts-api-11) has the rules; guard it with `api_version >= 11`.
- **Choice rows** take up to `FH2_SETTING_MAX_CHOICES` (100) choices. An older platform refuses a longer list (`add_setting` returns 0), so on one, add the choices as two rows.

### New in the previews

The HUD layer and the pad layer are still previews, reached through `get_interface`, not the API. Each has a new interface version, and each new thing needs it:

- **The HUD canvas** (`FH2Platform.HudLayer` version 3): `FH2_HUD_PART_CANVAS` is a part whose XAML is yours. `add_hud_canvas` takes it in `fh2mod_init`, with how many numbers, texts and switches it binds (`{Binding N0}`, `{Binding T0}`, `{Binding B0}` and so on), and `set_hud_canvas_numbers`, `set_hud_canvas_text` and `set_hud_canvas_switches` set them while it shows. It is shown and hidden with `show_hud_layer` like the bar, with the same fade, lease and one-layer-at-a-time rule. Numbers reach the page in the frame after they change, outside the budget for other values. [hud-layer-preview.md](hud-layer-preview.md#the-canvas-291) says what the check lets through and what is known to work in the game's HUD page.
- **Pad layers for the driving view only** (`FH2Platform.PadLayer` version 2): `FH2_PAD_LAYER_WORLD` in `FH2PadLayer.flags`. The layer is open only while the free-roam HUD page is on screen. [pad-layer-preview.md](pad-layer-preview.md#layers-for-the-driving-view-only-291) has the details.

### What changed for existing mods

Nothing a mod built for 2.9.0 has to change. Two things to know when you rebuild against the 2.9.1 headers:

- **`FH2_HUD_INTERFACE_VERSION` is 3 and `FH2_PAD_INTERFACE_VERSION` is 2.** A mod that passes those to `get_interface` now asks for 2.9.1's interfaces, and gets `NULL` on 2.9.0. If your mod should keep its bar or its layer on 2.9.0, ask for the version it needs (2 for the HUD layer with the feed, 1 for the pad layer) and look at the interface's `version` before using what is newer.
- **`FH2_HUD_PART_ALL` is unchanged** (the five parts 2.9.0 had), so a canvas has to be named in `show_hud_layer`, as the feed is never shown by it.

## Known limitations

- **A canvas can't have pictures or resources of its own yet**, and a number can only be bound to a transform's values, an `ed:Arc`'s end angle or an opacity. The check keeps the rest out because a HUD page the game can't load stops the game as the HUD opens.
- **A canvas that passes the check can still be drawn differently from WPF.** The game's XAML reader is its own. Look at your canvas in game, one thing at a time.
- **A pad layer for the driving view needs the platform to be following the HUD page**, which it does in a session where some mod has pop-ups, tips or a HUD layer. In a session where none has, such a layer is open everywhere, as on 2.9.0, and `platform.log` says so once.
- **The pick list opens on its first choice**, not on the one the row has now.
- **The HUD layer and pad layer are still previews.** Their names, limits and behaviour may change before they become part of an API version.
- Everything in [2.9.0's known limitations](release-notes-2.9.0.md#known-limitations) still holds.
