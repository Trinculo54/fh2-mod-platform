# Pad layers (preview)

A pad layer lets your mod use some controller buttons while the player holds a modifier, without the game also acting on them. While LB is held, say, the d-pad is yours: the game doesn't change the radio station or open telemetry, and your mod is told about each press. Let go of LB and the d-pad is the game's again.

This is a preview. Platform 2.9.0 is the first release with it, as the interface `FH2Platform.PadLayer` version 1; 2.9.1 has version 2, which adds [layers for the driving view only](#layers-for-the-driving-view-only-291) and [layers your mod opens itself](#layers-your-mod-opens-itself-291). It isn't part of an API version yet, and its shape can still change before it becomes one, so never make it the only way your mod can be used.

## Getting it

Include `fh2mod_pad.h` and ask for the interface in `fh2mod_init`:

```cpp
const FH2PadInterface* pad = (const FH2PadInterface*)api->get_interface(FH2_PAD_INTERFACE, 1);
if (!pad) {
    api->log(mod, "this platform has no pad layers; the d-pad controls are off");
    // carry on without them
}
```

It's there from `fh2mod_init` on. On platform 2.8.0 and older you get `NULL`, so your mod still loads.

## Adding a layer

```cpp
FH2PadLayer chat{sizeof chat};
chat.key      = "QuickChat";
chat.modifier = FH2_PAD_LB;
chat.buttons  = FH2_PAD_DPAD_UP | FH2_PAD_DPAD_DOWN | FH2_PAD_DPAD_LEFT | FH2_PAD_DPAD_RIGHT;
chat.on_press = [](void*, const char*, uint32_t button) { queue_message(button); };
pad->add_layer(mod, &chat);
```

- **`add_layer`** only works from `fh2mod_init`, like settings and pages. It returns 1, or 0 with a line in the log saying why: a bad key, bits that aren't `FH2_PAD_` buttons, a modifier that's one of the layer's own buttons, or a clash with another layer.
- **Clashes.** Two layers clash when both could be open at once and they take a button in common. LB with the d-pad and LB+RB with Up clash (holding LB+RB opens both); LB and RB with the same buttons don't. The second layer is refused, and the log names the first. 16 layers at most, across all mods.
- **`on_press`** runs on a platform thread, not the game's, once per press of one of the layer's buttons while its modifier is held. Keep it short. A press only counts when the button goes down while the layer is open: a button already held when the modifier goes down isn't one, because the game has had it.
- **Diagonals.** The game turns two d-pad directions held together into one diagonal control. A layer that takes either direction takes the diagonal too, and both directions count as presses.
- **The d-pad as one control.** Some of the game (the radio, for one) reads the d-pad as a single control with a direction rather than four buttons. While a layer has every d-pad direction held, that control is idle too. A layer that takes only some directions leaves it to the game whenever a direction it doesn't take is held, so for the d-pad, take all four.

## What the player sees

- **The modifier always reaches the game.** Only the layer's buttons are kept from it. Pick a modifier that does nothing harmful while held. LB is the clutch for players who drive manual with clutch, so give them a choice.
- **Letting go of the modifier first** doesn't hand the button to the game: it stays the layer's until it goes up, so the game never sees a press nobody made.
- **Menus.** A layer is on in every game state: the title screen, the pause menu and the MODS tab, the map and photo mode as well as driving. While its modifier is held there, the menu doesn't get the layer's buttons and your `on_press` still comes, so a player holding LB (the menus' tab-left) in the pause menu and pressing the d-pad would send your action from the menu. If your layer is for driving, ignore presses while a menu is open: `FH2_HUD_ON_SCREEN` in `hud_status` (with a [HUD layer](hud-layer-preview.md) of your own), or `FH2_UI_ON_SCREEN` in `ui_status` (API 10, in a session with pop-ups or tips: declare them with `ui_uses`), is set while the free-roam HUD page is on screen. Without `FH2_HUD_AVAILABLE`, or `FH2_UI_TOASTS` (in a session where no mod declared pop-ups or tips, say), the platform may not be following that page and the bit may never come, so then let presses through rather than drop them all. From 2.9.1 the platform can do that for you: see [Layers for the driving view only](#layers-for-the-driving-view-only-291).

## Layers for the driving view only (2.9.1)

A layer for driving shouldn't take its buttons in a menu: LB is the menus' tab-left and the d-pad moves the selection, so held together for a moment they would send your action from the pause menu, and the menu wouldn't get the press. From platform 2.9.1 (interface version 2), set `FH2_PAD_LAYER_WORLD` in the layer's `flags`:

```cpp
const FH2PadInterface* pad = (const FH2PadInterface*)api->get_interface(FH2_PAD_INTERFACE, 1);   // 1: 2.9.0 has it too
FH2PadLayer chat{sizeof chat};
chat.key      = "QuickChat";
chat.modifier = FH2_PAD_LB;
chat.buttons  = FH2_PAD_DPAD_UP | FH2_PAD_DPAD_DOWN | FH2_PAD_DPAD_LEFT | FH2_PAD_DPAD_RIGHT;
chat.flags    = pad->version >= 2 ? FH2_PAD_LAYER_WORLD : 0;   // 2.9.0 refuses a layer with any flag
chat.on_press = on_press;
pad->add_layer(mod, &chat);
```

- **The layer is open only while the free-roam HUD page is on screen.** In the pause menu, the MODS tab, the map, photo mode, the rewind and on the title screen, its buttons are the game's and no `on_press` comes, whether the modifier is held or not.
- **A press that straddles the change is safe.** A button the layer had taken when a menu opens stays hidden until it goes up, so the menu never gets half a press; pressed again, it's the menu's.
- **The platform has to be following that page.** It does in a session where some mod has pop-ups or tips (`ui_uses`, API 10) or a [HUD layer](hud-layer-preview.md). In a session where none has, the layer is open everywhere, as a layer without the flag is, and `platform.log` says once: "the platform isn't following the game's HUD page this session ...". A mod whose layer must stay out of the menus can declare tips or pop-ups to make sure of it.
- **On 2.9.0** `version` is 1 and `add_layer` refuses a layer with any flag, so set it only when `version >= 2`, and keep ignoring presses in menus yourself there (the paragraph above).

## Layers your mod opens itself (2.9.1)

A modifier suits a quick action. A mode of your own that lasts (an editor, a photo tool, flying) can't ask the player to hold a button for every press, and may need buttons the game uses all the time, like RB (the camera) or Y (the rewind). A **latched** layer has no modifier: your mod opens and closes it, and while it's open the game doesn't see its buttons.

```cpp
FH2PadLayer flight{sizeof flight};
flight.key      = "Flight";
flight.buttons  = FH2_PAD_RB | FH2_PAD_Y | FH2_PAD_DPAD_UP | FH2_PAD_DPAD_DOWN | FH2_PAD_DPAD_LEFT | FH2_PAD_DPAD_RIGHT;
flight.flags    = FH2_PAD_LAYER_LATCH | FH2_PAD_LAYER_WORLD;   // version 2: check pad->version first
flight.on_press = on_press;
pad->add_layer(mod, &flight);

// while the mode is on, a few times a second, from any thread
pad->set_open(mod, "Flight", 1);
// when it ends
pad->set_open(mod, "Flight", 0);
```

- **`set_open(mod, key, 1)` opens it for 2 seconds.** Repeat the call (every half second, say) for as long as your mode is on. If your code stops, the lease runs out and the player's buttons go back to the game by themselves. `set_open(mod, key, 0)` closes it at once.
- **Presses work as with a modifier.** A button already held as the layer opens isn't a press (the game has had it), and one held as it closes stays hidden until it goes up, so the game never gets half a press.
- **Leave the player a way out.** Don't take `FH2_PAD_START`: the pause menu has to open. With `FH2_PAD_LAYER_WORLD` as well, the layer is only open in the driving view however long you hold it open, so the pause menu, the map and photo mode keep every button; use both for a mode that belongs to driving.
- **No modifier, and no clash check.** `add_layer` refuses a latched layer with a `modifier`, and `set_modifier` refuses to give it one. A latched layer isn't checked against other layers: if two layers that are open at once take the same button, both get its press. Take only the buttons your mode needs.
- **It returns** 1, or 0 for an unknown layer, one that isn't latched, or pad layers being off this session (the log says which). On platform 2.9.0 the interface is version 1 and has no `set_open`: check `version >= 2`.

## Changing the modifier

Give the player a choice row (`FH2_SETTING_CHOICE`) and pass their pick on:

```cpp
pad->set_modifier(mod, "QuickChat", FH2_PAD_RB);   // 0 turns the layer off
```

It works any time, from any thread, and takes effect on the next frame. It returns 0 for an unknown layer, bad bits or a clash, and the layer then keeps the modifier it had. The platform doesn't remember modifiers, so save the player's choice in your settings and pass it to `add_layer` next time.

## Reading the controller

```cpp
uint32_t held;
if (pad->buttons(&held) && (held & FH2_PAD_LB)) ...
```

`buttons` gives the `FH2_PAD_` bits held on the controller the game is using now, from the game's own reading of it, so your mod doesn't need XInput or game addresses of its own. It returns 0 until the game has a controller (in the first seconds of the title screen, say). It works with or without layers, from any thread.

## How it works

Each frame the game builds a reading of every controller: a mask of the controls held, and the pad's raw buttons. The platform hooks the function the game calls straight after that (game+0x10768D0), and takes the held buttons of each open layer out of the reading, so every part of the game that reads the controller sees them as not pressed. It also marks them in the game's own mask of taken controls, the one that stops a button that closed a menu from acting in the game behind it. A button the layer has taken stays out of the reading until it goes up. The game keeps the same pad in more than one slot, so presses only count from the slot the game is using. Everything runs on the game's own thread, in the game's own order.

If the game's code doesn't match, `platform.log` says "pad layers are off: the game code doesn't match at ...", `add_layer` still succeeds, and no presses come. If no mod adds a layer, nothing is hooked, but `buttons` rests on the same code and is checked the same way: if it doesn't match, the log says "the pad's buttons() is off: ..." and `buttons` returns 0. The game's memory is read through a guarded copy, so a controller manager that isn't there or isn't what the platform expects means no presses and no buttons, never a crash.

## Tests

`tests/pad_engine_test.cpp` checks the rules; `tests/run_pad_test.sh <forza_durango_release_final.exe>` also runs `tests/pad_hook_test.cpp`, which maps the real exe and calls the game's own function through the hook with a controller slot of its own, and then with a manager pointer that can't be read and a slot that can't be written.
