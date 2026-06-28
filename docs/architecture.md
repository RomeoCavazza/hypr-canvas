# Architecture

## Product Scope

hyprcanvas transforms a Hyprland workspace into a navigable spatial plane.

The plugin does not try to become a complete layout engine. It owns only one mode: canvas mode. When active, windows are represented as cards on a virtual canvas. The viewport is derived from:

```text
screen = (canvas - offset) * zoom
canvas = offset + screen / zoom
```

## Core Principles

1. One source of truth: virtual window geometry lives in `SWindowState::canvasPos` and `SWindowState::canvasSize`.
2. Mouse operations are immediate: live drag, live resize, pan, and wheel zoom use `setValueAndWarp`.
3. Keyboard operations are animated: navigation, centering, and home assign Hyprland animated variables with `operator=`.
4. Focus is deliberate: navigation picks a target, recenters the viewport, then calls Hyprland's native `focuswindow` dispatcher.
5. The plugin stays small: no Python daemon, no raw `/dev/input`, no polling loop, no subprocess IPC loop.

## Target Internal Blocks

The first 0.55.4 refactor keeps the code in a small number of files, but it follows these boundaries:

- `CanvasSession`: enter, exit, workspace lifecycle, restore state.
- `CanvasStore`: saved window state, virtual geometry, floating state, pinned state.
- `Viewport`: `offset`, `zoom`, `screenToCanvas`, `canvasToScreen`, monitor center.
- `GeometryCommitter`: applies geometry through `Warp` or `Animate`.
- `Navigator`: `home`, `center`, directional `nav`, cycle `next/prev`.
- `Dispatchers`: Hyprland API surface.

## Window Lifecycle

On enter:

1. Capture the active workspace.
2. Save every mapped window on that workspace.
3. Preserve original position, size, and floating state.
4. Convert tiled windows into independent canvas cards using a fixed reference size while preserving visual center.
5. Force windows floating.
6. Commit the initial card geometry with `Warp`.

On exit:

1. Restore saved geometry with `Warp`.
2. Restore original floating state.
3. Clear canvas state.
4. Ask the active layout to recalculate the monitor.

## Commit Policy

`ECommitMode::Warp`:

- Used for mouse and continuous interactions.
- Calls `setValueAndWarp` on position and size.
- Keeps the pointer-to-window relationship exact.

`ECommitMode::Animate`:

- Used for keyboard navigation and recentering.
- Assigns `*m_realPosition = pos` and `*m_realSize = size`.
- Lets Hyprland's configured window animation curves move the canvas.

## Navigation

Directional navigation uses the VXWM scoring model.

For each candidate in the requested direction:

```text
score = axial + lateral^2 / (axial + 1)
```

This prefers windows that are truly in the requested direction, while still tolerating imperfect spatial grids.

Centering a target updates only the viewport:

```text
offset = targetCenter - monitorCenter / zoom
```

Then all non-pinned canvas windows are recommitted from their virtual geometry.

## Focus

The plugin does not call removed/private compositor focus APIs. It reuses the public keybind dispatcher table:

```text
focuswindow address:0x...
```

This keeps focus behavior aligned with Hyprland's native dispatcher path.

## Current Limitations

- Canvas state is per active session, not persisted per workspace yet.
- Protected applications are not implemented yet.
- Pinned windows are basic and need UX validation.
- The Hyprland config should eventually bind `canvas:nav`, `canvas:center`, and `canvas:home` directly instead of routing through helper scripts.
