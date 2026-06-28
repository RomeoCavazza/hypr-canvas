# hyprcanvas

Native C++ infinite canvas mode for Hyprland.

hyprcanvas turns one Hyprland workspace into a spatial plane: windows become persistent cards in virtual canvas coordinates, and the monitor becomes a camera controlled by `offset + zoom`.

It is not a Niri clone, not a PaperWM layout, and not a full window manager. The scope is deliberately small: a clean canvas mode for Hyprland with native input hooks, no Python daemon, no `/dev/input` polling, and no IPC spam.

## Design

- One source of truth: every managed window has `canvasPos` and `canvasSize`.
- Mouse movement is immediate: drag, resize, pan, and wheel zoom commit with `setValueAndWarp`.
- Keyboard navigation is animated: `home`, `center`, and `nav` assign Hyprland animated variables and let the compositor move the world.
- Focus is intentional: navigation picks a target, recenters the viewport, then asks Hyprland's native `focuswindow` dispatcher to focus it.
- The mode is explicit: enter canvas, work spatially, exit back to the original tiling/floating state.

The current implementation follows the VXWM/Pedrito model: it physically moves windows as cards instead of trying to hook the entire renderer as a true camera transform.

## Dispatchers

```conf
canvas:toggle
canvas:enter
canvas:exit
canvas:reset
canvas:home
canvas:center
canvas:nav left|right|up|down|next|prev
canvas:swap left|right|up|down
canvas:pan left|right|up|down
canvas:zoom in|out|reset
canvas:pin
canvas:float
canvas:overview
```

`canvas:reset` is kept as an alias for exit/reset compatibility.
`canvas:float` is a canvas-aware wrapper around Hyprland's `togglefloating`: outside canvas mode it behaves like the native dispatcher; inside canvas mode it exits the workspace canvas cleanly, refocuses the active card, then toggles that window's floating state.
`canvas:swap` is canvas-aware too: outside canvas mode it delegates to Hyprland's native `swapwindow`; inside canvas mode it swaps the active card's `canvasPos` and `canvasSize` with the best directional target, then recenters the active card.

## Suggested Binds

```conf
bind = SUPER, Z, canvas:toggle
bind = SUPER, F, canvas:float
bind = SUPER, R, canvas:home
bind = SUPER, X, canvas:center

bind = SUPER, left,  canvas:nav, left
bind = SUPER, right, canvas:nav, right
bind = SUPER, up,    canvas:nav, up
bind = SUPER, down,  canvas:nav, down

bind = SUPER SHIFT, left,  canvas:swap, left
bind = SUPER SHIFT, right, canvas:swap, right
bind = SUPER SHIFT, up,    canvas:swap, up
bind = SUPER SHIFT, down,  canvas:swap, down

bind = SUPER ALT SHIFT, left,  canvas:pan, left
bind = SUPER ALT SHIFT, right, canvas:pan, right
bind = SUPER ALT SHIFT, up,    canvas:pan, up
bind = SUPER ALT SHIFT, down,  canvas:pan, down

bind = SUPER, minus, canvas:zoom, out
bind = SUPER, equal, canvas:zoom, in

bind = SUPER, W, canvas:overview
bind = SUPER, P, canvas:pin
```

`SUPER + X` centers the active card. `SUPER + R` uses the same target but resets zoom to `1.0` first, which makes it useful both as a recovery key and during development.

Mouse controls:

- `SUPER + wheel`: zoom.
- `SUPER + left drag` on empty space: pan.
- In canvas mode, `SUPER + left drag` on a canvas window moves that card.
- In canvas mode, `SUPER + right drag` on a canvas window resizes that card.

## Build

Requires Hyprland development headers matching the running compositor.

```sh
make
```

Load manually:

```sh
hyprctl plugin load "$PWD/hypr-canvas.so"
```

Or configure Hyprland:

```conf
plugin = /absolute/path/to/hypr-canvas.so
```

## Status

This is an alpha plugin against Hyprland internals. The 0.55.4-oriented branch is intentionally being rebuilt around a small, readable canvas-mode core with per-workspace shape memory, protected apps, overview clustering, and canvas-aware keybind wrappers.
