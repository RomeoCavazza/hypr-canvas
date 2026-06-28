# hyprcanvas

[![Build](https://github.com/RomeoCavazza/hypr-canvas/actions/workflows/build.yml/badge.svg)](https://github.com/RomeoCavazza/hypr-canvas/actions/workflows/build.yml)
[![Release](https://github.com/RomeoCavazza/hypr-canvas/actions/workflows/release.yml/badge.svg)](https://github.com/RomeoCavazza/hypr-canvas/actions/workflows/release.yml)
[![Hyprland](https://img.shields.io/badge/Hyprland-v0.55.4-58E1FF)](https://github.com/hyprwm/Hyprland/releases/tag/v0.55.4)

Native C++ infinite canvas mode for Hyprland.

<video src="https://github.com/RomeoCavazza/hypr-canvas/raw/main/assets/showcase.mp4" controls="controls" width="100%"></video>

hyprcanvas turns one Hyprland workspace into a spatial plane: windows become persistent cards in virtual canvas coordinates, and the monitor becomes a camera controlled by `offset + zoom`.

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

### Closer Look

<video src="https://github.com/RomeoCavazza/hypr-canvas/raw/main/assets/demo.mp4" controls="controls" width="100%"></video>

## Configuration

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
