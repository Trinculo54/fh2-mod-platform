# Pad layers (preview)

A pad layer lets your mod use some controller buttons while the player holds a modifier, without the game also acting on them. While LB is held, say, the d-pad is yours: the game doesn't change the radio station or open telemetry, and your mod is told about each press. Let go of LB and the d-pad is the game's again.

This is a preview. Platform 2.9.0 is the first release with it, as the interface `FH2Platform.PadLayer` version 1. It isn't part of an API version yet, and its shape can still change before it becomes one, so never make it the only way your mod can be used.

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
- **Menus.** A layer works wherever the game reads the controller, menus too. If your mod only means it for driving, check that yourself before acting on a press.

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

If the game's code doesn't match, `platform.log` says "pad layers are off: the game code doesn't match at ...", `add_layer` still succeeds, and no presses come. If no mod adds a layer, nothing is hooked.

## Tests

`tests/pad_engine_test.cpp` checks the rules; `tests/run_pad_test.sh <forza_durango_release_final.exe>` also runs `tests/pad_hook_test.cpp`, which maps the real exe and calls the game's own function through the hook with a controller slot of its own.
