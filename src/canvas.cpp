#include "canvas.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/helpers/Monitor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <limits>
#include <vector>
#include <linux/input-event-codes.h>

static void logf(const char* fmt, ...) {
    FILE* f = fopen("/tmp/hypr-canvas.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

static void scheduleFrame() {
    auto mon = Desktop::focusState()->monitor();
    if (mon) {
        g_pHyprRenderer->damageMonitor(mon);
        g_pCompositor->scheduleFrameForMonitor(mon);
    }
}

// --- Forward typedefs ---
using PHLWINDOW = SP<Desktop::View::CWindow>;

// --- Scroll/zoom hook ---

typedef void (*onMouseWheelFn)(CInputManager*, IPointer::SAxisEvent, SP<IPointer>);
typedef Vector2D (*positionFn)(CPointerManager*);

static void hkOnMouseWheel(CInputManager* self, IPointer::SAxisEvent e, SP<IPointer> pointer) {
    const uint32_t mods = g_pInputManager->getModsFromAllKBs();

    if (g_pCanvas && g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    if ((mods & HL_MODIFIER_META) && g_pCanvas && e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        const double scrollDelta = (e.deltaDiscrete != 0) ? (double)e.deltaDiscrete : e.delta;
        if (scrollDelta != 0) {
            if (!g_pCanvas->active)
                g_pCanvas->enter();

            const double zoomFactor = std::pow(CCanvas::ZOOM_STEP, std::abs(scrollDelta));
            double       newZoom    = g_pCanvas->zoom;
            if (scrollDelta < 0)
                newZoom *= zoomFactor;
            else
                newZoom /= zoomFactor;

            // Center-anchored zoom feels more like a desktop camera than a drag.
            auto mon = Desktop::focusState()->monitor();
            if (!mon)
                return;

            const auto monSize = mon->m_transformedSize;
            const Vector2D center = {monSize.x / 2.0, monSize.y / 2.0};

            g_pCanvas->applyZoom(newZoom, center);
            g_pCanvas->repositionWindows(ECommitMode::Warp);

            logf("[hypr-canvas] zoom=%.3f offset=(%.1f, %.1f)\n",
                 g_pCanvas->zoom, g_pCanvas->offset.x, g_pCanvas->offset.y);
            scheduleFrame();
            return;
        }
    }

    auto original = (onMouseWheelFn)g_pCanvas->m_mouseWheelHook->m_original;
    original(self, e, pointer);
}

// --- Mouse button hook (pan start/stop) ---

typedef void (*onMouseButtonFn)(CInputManager*, IPointer::SButtonEvent);

static void hkOnMouseButton(CInputManager* self, IPointer::SButtonEvent e) {
    if (g_pCanvas && g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    if (g_pCanvas && (e.button == BTN_LEFT || e.button == BTN_RIGHT)) {
        const uint32_t mods = g_pInputManager->getModsFromAllKBs();
        if ((mods & HL_MODIFIER_META) && e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            if (!g_pCanvas->active && e.button == BTN_LEFT) {
                const auto coords = g_pInputManager->getMouseCoordsInternal();
                using namespace Desktop::View;
                auto windowUnder = g_pCompositor->vectorToWindowUnified(coords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);

                if (!windowUnder) {
                    g_pCanvas->enter();
                    g_pCanvas->m_panning = true;
                    logf("[hypr-canvas] pan start\n");
                    return;
                }
            }

            if (!g_pCanvas->active) {
                auto original = (onMouseButtonFn)g_pCanvas->m_mouseButtonHook->m_original;
                original(self, e);
                return;
            }

            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                const auto coords = g_pInputManager->getMouseCoordsInternal();
                using namespace Desktop::View;
                auto windowUnder = g_pCompositor->vectorToWindowUnified(coords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);

                if (e.button == BTN_LEFT && !windowUnder) {
                    g_pCanvas->m_panning = true;
                    logf("[hypr-canvas] pan start\n");
                    return;
                }

                if (windowUnder) {
                    const uint64_t id = (uint64_t)windowUnder.get();
                    if (g_pCanvas->m_savedStates.contains(id)) {
                        g_pCanvas->m_dragWindow = id;

                        if (e.button == BTN_LEFT) {
                            g_pCanvas->m_movingWindow = true;
                            g_pCanvas->m_resizingWindow = false;
                            logf("[hypr-canvas] window move start id=%lx\n", id);
                            return;
                        }

                        if (e.button == BTN_RIGHT) {
                            g_pCanvas->m_resizingWindow = true;
                            g_pCanvas->m_movingWindow = false;
                            logf("[hypr-canvas] window resize start id=%lx\n", id);
                            return;
                        }
                    }
                }
            }
        }
    }

    if (g_pCanvas && g_pCanvas->m_panning && e.button == BTN_LEFT && e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
        g_pCanvas->m_panning = false;
        logf("[hypr-canvas] pan stop\n");
        return;
    }

    if (g_pCanvas && g_pCanvas->m_movingWindow && e.button == BTN_LEFT && e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
        logf("[hypr-canvas] window move stop id=%lx\n", g_pCanvas->m_dragWindow);
        g_pCanvas->m_movingWindow = false;
        g_pCanvas->m_dragWindow = 0;
        return;
    }

    if (g_pCanvas && g_pCanvas->m_resizingWindow && e.button == BTN_RIGHT && e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
        logf("[hypr-canvas] window resize stop id=%lx\n", g_pCanvas->m_dragWindow);
        g_pCanvas->m_resizingWindow = false;
        g_pCanvas->m_dragWindow = 0;
        return;
    }

    auto original = (onMouseButtonFn)g_pCanvas->m_mouseButtonHook->m_original;
    original(self, e);
}

// --- Mouse move hook (pan drag) ---

typedef void (*onMouseMovedFn)(CInputManager*, IPointer::SMotionEvent);

static void hkOnMouseMoved(CInputManager* self, IPointer::SMotionEvent e) {
    const uint32_t mods = g_pInputManager->getModsFromAllKBs();

    if (g_pCanvas && g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    // If we ever miss a button-release event, do not stay stuck in a drag mode.
    if (g_pCanvas && !(mods & HL_MODIFIER_META) &&
        (g_pCanvas->m_panning || g_pCanvas->m_movingWindow || g_pCanvas->m_resizingWindow)) {
        logf("[hypr-canvas] drag watchdog reset (mods released)\n");
        g_pCanvas->m_panning = false;
        g_pCanvas->m_movingWindow = false;
        g_pCanvas->m_resizingWindow = false;
        g_pCanvas->m_dragWindow = 0;
    }

    if (g_pCanvas && g_pCanvas->m_panning) {
        // Pan: move viewport by mouse delta
        g_pCanvas->offset.x -= e.delta.x / g_pCanvas->zoom;
        g_pCanvas->offset.y -= e.delta.y / g_pCanvas->zoom;
        g_pCanvas->repositionWindows(ECommitMode::Warp);
        scheduleFrame();
        return;
    }

    if (g_pCanvas && g_pCanvas->m_movingWindow && g_pCanvas->m_dragWindow != 0) {
        auto it = g_pCanvas->m_savedStates.find(g_pCanvas->m_dragWindow);
        if (it != g_pCanvas->m_savedStates.end()) {
            it->second.canvasPos.x += e.delta.x / g_pCanvas->zoom;
            it->second.canvasPos.y += e.delta.y / g_pCanvas->zoom;
            g_pCanvas->repositionWindows(ECommitMode::Warp);
            scheduleFrame();
            return;
        }
    }

    if (g_pCanvas && g_pCanvas->m_resizingWindow && g_pCanvas->m_dragWindow != 0) {
        auto it = g_pCanvas->m_savedStates.find(g_pCanvas->m_dragWindow);
        if (it != g_pCanvas->m_savedStates.end()) {
            it->second.canvasSize.x = std::max(CCanvas::MIN_WINDOW_W, it->second.canvasSize.x + e.delta.x / g_pCanvas->zoom);
            it->second.canvasSize.y = std::max(CCanvas::MIN_WINDOW_H, it->second.canvasSize.y + e.delta.y / g_pCanvas->zoom);
            g_pCanvas->repositionWindows(ECommitMode::Warp);
            scheduleFrame();
            return;
        }
    }

    auto original = (onMouseMovedFn)g_pCanvas->m_mouseMovedHook->m_original;
    original(self, e);
}

// --- Dispatchers ---

SDispatchResult dispatchEnter(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    if (!g_pCanvas->active) {
        g_pCanvas->enter();
        logf("[hypr-canvas] canvas ON\n");
        scheduleFrame();
    }
    return {};
}

SDispatchResult dispatchExit(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->active) {
        g_pCanvas->exit();
        logf("[hypr-canvas] canvas OFF\n");
        scheduleFrame();
    }
    return {};
}

SDispatchResult dispatchReset(std::string args) {
    if (g_pCanvas && g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    if (g_pCanvas && g_pCanvas->active) {
        g_pCanvas->exit();
        logf("[hypr-canvas] exit canvas mode\n");
        scheduleFrame();
    }
    return {};
}

SDispatchResult dispatchHome(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    const bool wasActive = g_pCanvas->active;
    auto preferred = Desktop::focusState()->window();

    g_pCanvas->ensureActive();

    if (!wasActive && preferred && g_pCanvas->windowOnCanvasWorkspace(preferred) &&
        g_pCanvas->m_savedStates.contains((uint64_t)preferred.get())) {
        g_pCanvas->centerOnWindow(preferred, ECommitMode::Animate);
        g_pCanvas->focusWindow(preferred);
        logf("[hypr-canvas] home enter-center id=%lx\n", (unsigned long)preferred.get());
    } else {
        g_pCanvas->home(ECommitMode::Animate);
        logf("[hypr-canvas] home\n");
    }

    scheduleFrame();
    return {};
}

SDispatchResult dispatchCenter(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    auto preferred = Desktop::focusState()->window();

    g_pCanvas->ensureActive();

    if (preferred && g_pCanvas->windowOnCanvasWorkspace(preferred) &&
        g_pCanvas->m_savedStates.contains((uint64_t)preferred.get())) {
        g_pCanvas->centerOnWindow(preferred, ECommitMode::Animate);
        g_pCanvas->focusWindow(preferred);
        logf("[hypr-canvas] center id=%lx\n", (unsigned long)preferred.get());
    } else {
        g_pCanvas->centerActive(ECommitMode::Animate);
        logf("[hypr-canvas] center fallback\n");
    }

    scheduleFrame();
    return {};
}

SDispatchResult dispatchNav(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    auto preferred = Desktop::focusState()->window();

    g_pCanvas->ensureActive();

    if (preferred && g_pCanvas->windowOnCanvasWorkspace(preferred) &&
        g_pCanvas->m_savedStates.contains((uint64_t)preferred.get()))
        g_pCanvas->focusWindow(preferred);

    g_pCanvas->nav(args);
    logf("[hypr-canvas] nav %s\n", args.c_str());
    scheduleFrame();
    return {};
}

SDispatchResult dispatchPan(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    Vector2D delta = {0, 0};
    if (args == "left")       delta.x = CCanvas::PAN_STEP;
    else if (args == "right") delta.x = -CCanvas::PAN_STEP;
    else if (args == "up")    delta.y = CCanvas::PAN_STEP;
    else if (args == "down")  delta.y = -CCanvas::PAN_STEP;
    else
        return {};

    if (!g_pCanvas->active)
        g_pCanvas->enter();

    g_pCanvas->offset.x += delta.x / g_pCanvas->zoom;
    g_pCanvas->offset.y += delta.y / g_pCanvas->zoom;
    g_pCanvas->repositionWindows(ECommitMode::Warp);
    logf("[hypr-canvas] pan %s → offset=(%.1f, %.1f)\n",
         args.c_str(), g_pCanvas->offset.x, g_pCanvas->offset.y);
    scheduleFrame();
    return {};
}

SDispatchResult dispatchZoom(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    auto mon = Desktop::focusState()->monitor();
    if (!mon) return {};

    const auto monSize = mon->m_transformedSize;
    const Vector2D center = {monSize.x / 2.0, monSize.y / 2.0};

    double newZoom = g_pCanvas->zoom;
    if (args == "in")
        newZoom *= CCanvas::ZOOM_STEP;
    else if (args == "out")
        newZoom /= CCanvas::ZOOM_STEP;
    else if (args == "reset")
        newZoom = 1.0;
    else
        return {};

    if (!g_pCanvas->active)
        g_pCanvas->enter();

    g_pCanvas->applyZoom(newZoom, center);
    g_pCanvas->repositionWindows(ECommitMode::Warp);
    logf("[hypr-canvas] zoom %s → %.3f\n", args.c_str(), g_pCanvas->zoom);
    scheduleFrame();
    return {};
}

SDispatchResult dispatchPin(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    g_pCanvas->ensureActive();
    g_pCanvas->togglePin();
    scheduleFrame();
    return {};
}

SDispatchResult dispatchFloat(std::string args) {
    auto dispatcher = g_pKeybindManager->m_dispatchers.find("togglefloating");
    if (dispatcher == g_pKeybindManager->m_dispatchers.end())
        return {};

    SP<Desktop::View::CWindow> target;
    if (g_pCanvas) {
        if (g_pCanvas->workspaceChanged())
            g_pCanvas->resetForWorkspaceChange();

        if (g_pCanvas->active) {
            target = g_pCanvas->activeCanvasWindow();
            g_pCanvas->exit();

            if (target)
                g_pCanvas->focusWindow(target);
        }
    }

    logf("[hypr-canvas] float handoff\n");
    return dispatcher->second(args);
}

SDispatchResult dispatchToggle(std::string args) {
    if (!g_pCanvas) return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    if (g_pCanvas->active) {
        g_pCanvas->exit();
        logf("[hypr-canvas] canvas OFF\n");
    } else {
        g_pCanvas->enter();
        logf("[hypr-canvas] canvas ON\n");
    }
    scheduleFrame();
    return {};
}

// --- Hook helper ---

static CFunctionHook* hookByName(const std::string& name, void* dest) {
    auto fns = HyprlandAPI::findFunctionsByName(PHANDLE, name);
    logf("[hypr-canvas] %s: %zu matches\n", name.c_str(), fns.size());
    if (fns.empty()) return nullptr;
    auto hook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, dest);
    if (hook && hook->hook())
        logf("[hypr-canvas] hooked %s\n", name.c_str());
    return hook;
}

// --- Constructor / Destructor ---

CCanvas::CCanvas() {
    m_mouseWheelHook  = hookByName("onMouseWheel", (void*)&hkOnMouseWheel);
    m_mouseButtonHook = hookByName("onMouseButton", (void*)&hkOnMouseButton);
    m_mouseMovedHook  = hookByName("onMouseMoved", (void*)&hkOnMouseMoved);
    logf("[hypr-canvas] initialized (VXWM mode — no render hooks)\n");
}

CCanvas::~CCanvas() {
    // Restore windows if still in canvas mode
    if (active)
        exit();

    if (m_mouseWheelHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, m_mouseWheelHook);
    if (m_mouseButtonHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, m_mouseButtonHook);
    if (m_mouseMovedHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, m_mouseMovedHook);
}

// --- Canvas mode enter/exit ---

void CCanvas::enter() {
    auto mon = Desktop::focusState()->monitor();
    if (!mon)
        return;

    struct SEntry {
        SP<Desktop::View::CWindow> window;
        uint64_t id = 0;
        Vector2D currentPos;
        Vector2D currentSize;
        Vector2D center;
        bool wasFloating = false;
    };

    active = true;
    zoom = 1.0;
    offset = {0, 0};
    m_canvasWorkspace = mon->activeWorkspaceID();
    m_savedStates.clear();
    m_panning = false;
    m_movingWindow = false;
    m_resizingWindow = false;
    m_dragWindow = 0;

    std::vector<SEntry> entries;
    std::vector<size_t> tiledEntries;
    auto focused = Desktop::focusState()->window();

    for (auto& w : g_pCompositor->m_windows) {
        if (!w || w->isHidden() || !w->m_isMapped || !windowOnCanvasWorkspace(w))
            continue;

        SEntry entry;
        entry.window = w;
        entry.id = (uint64_t)w.get();
        entry.currentPos = w->m_realPosition->value();
        entry.currentSize = w->m_realSize->value();
        entry.center = entry.currentPos + entry.currentSize / 2.0;
        entry.wasFloating = w->m_isFloating;

        if (!entry.wasFloating)
            tiledEntries.push_back(entries.size());

        entries.push_back(entry);
    }

    std::sort(tiledEntries.begin(), tiledEntries.end(), [&](size_t a, size_t b) {
        const auto& ca = entries[a].center;
        const auto& cb = entries[b].center;
        if (std::abs(ca.x - cb.x) > 1.0)
            return ca.x < cb.x;
        return ca.y < cb.y;
    });

    size_t focusedTiledIndex = tiledEntries.empty() ? 0 : tiledEntries.size() / 2;
    if (focused) {
        for (size_t i = 0; i < tiledEntries.size(); ++i) {
            if (entries[tiledEntries[i]].window == focused) {
                focusedTiledIndex = i;
                break;
            }
        }
    }

    const Vector2D baseCenter = monitorCenter();
    const double stride = CANVAS_REF_W + CARD_GAP;

    for (auto& entry : entries) {
        auto& w = entry.window;

        SWindowState state;
        state.restorePos  = entry.currentPos;
        state.restoreSize = entry.currentSize;
        state.wasFloating = entry.wasFloating;

        if (entry.wasFloating) {
            state.canvasPos  = entry.currentPos;
            state.canvasSize = entry.currentSize;
        } else {
            auto it = std::find(tiledEntries.begin(), tiledEntries.end(), (size_t)(&entry - entries.data()));
            const double rank = it == tiledEntries.end() ? 0.0 : (double)std::distance(tiledEntries.begin(), it) - (double)focusedTiledIndex;
            const Vector2D cardCenter = {
                baseCenter.x + rank * stride,
                baseCenter.y,
            };

            state.canvasSize = {CANVAS_REF_W, CANVAS_REF_H};
            state.canvasPos = {
                cardCenter.x - state.canvasSize.x / 2.0,
                cardCenter.y - state.canvasSize.y / 2.0
            };
        }
        m_savedStates[entry.id] = state;

        g_pHyprRenderer->damageWindow(w);

        if (!w->m_isFloating) {
            w->m_isFloating = true;
        }

        commitWindow(w, state.canvasPos, state.canvasSize, ECommitMode::Warp);
        g_pHyprRenderer->damageWindow(w);
    }

    logf("[hypr-canvas] entered canvas mode on workspace %ld, saved %zu windows\n",
         (long)m_canvasWorkspace, m_savedStates.size());
}

void CCanvas::exit() {
    // Restore all saved window positions and float state
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || w->isHidden() || !w->m_isMapped || !windowOnCanvasWorkspace(w))
            continue;

        uint64_t id = (uint64_t)w.get();
        auto it = m_savedStates.find(id);
        if (it == m_savedStates.end())
            continue;

        const auto& saved = it->second;
        g_pHyprRenderer->damageWindow(w);
        commitWindow(w, saved.restorePos, saved.restoreSize, ECommitMode::Warp);

        if (!saved.wasFloating) {
            w->m_isFloating = false;
        }
        g_pHyprRenderer->damageWindow(w);
    }

    m_savedStates.clear();
    active = false;
    zoom = 1.0;
    offset = {0, 0};
    m_canvasWorkspace = WORKSPACE_INVALID;
    m_panning = false;
    m_movingWindow = false;
    m_resizingWindow = false;
    m_dragWindow = 0;

    // Force relayout to snap windows back to tiling
    auto mon = Desktop::focusState()->monitor();
    if (mon)
        g_layoutManager->recalculateMonitor(mon);

    logf("[hypr-canvas] exited canvas mode, restored windows\n");
}

void CCanvas::ensureActive() {
    if (!active)
        enter();
}

Vector2D CCanvas::screenToCanvas(const Vector2D& screen) const {
    return offset + screen / zoom;
}

Vector2D CCanvas::canvasToScreen(const Vector2D& canvas) const {
    return (canvas - offset) * zoom;
}

Vector2D CCanvas::monitorCenter() const {
    auto mon = Desktop::focusState()->monitor();
    if (!mon)
        return {0, 0};

    return {
        mon->m_position.x + mon->m_transformedSize.x / 2.0,
        mon->m_position.y + mon->m_transformedSize.y / 2.0,
    };
}

void CCanvas::home(ECommitMode mode) {
    zoom = 1.0;
    offset = {0, 0};
    repositionWindows(mode);
}

void CCanvas::centerActive(ECommitMode mode) {
    centerOnWindow(activeCanvasWindow(), mode);
}

void CCanvas::nav(const std::string& direction) {
    auto source = activeCanvasWindow();
    if (!source)
        return;

    SP<Desktop::View::CWindow> target;
    if (direction == "next" || direction == "n")
        target = findCycleTarget(source, false);
    else if (direction == "prev" || direction == "previous" || direction == "p")
        target = findCycleTarget(source, true);
    else
        target = findDirectionalTarget(source, direction);

    if (!target)
        target = source;

    centerOnWindow(target, ECommitMode::Animate);
    focusWindow(target);
}

void CCanvas::togglePin() {
    auto target = activeCanvasWindow();
    if (!target)
        return;

    const uint64_t id = (uint64_t)target.get();
    auto it = m_savedStates.find(id);
    if (it == m_savedStates.end())
        return;

    it->second.pinned = !it->second.pinned;
    if (!it->second.pinned) {
        it->second.canvasPos = screenToCanvas(target->m_realPosition->value());
        it->second.canvasSize = target->m_realSize->value() / zoom;
    }

    logf("[hypr-canvas] pin id=%lx pinned=%d\n", id, it->second.pinned ? 1 : 0);
}

SP<Desktop::View::CWindow> CCanvas::activeCanvasWindow() const {
    auto focused = Desktop::focusState()->window();
    if (focused && windowOnCanvasWorkspace(focused) && m_savedStates.contains((uint64_t)focused.get()))
        return focused;

    return firstCanvasWindow();
}

SP<Desktop::View::CWindow> CCanvas::firstCanvasWindow() const {
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || w->isHidden() || !w->m_isMapped || !windowOnCanvasWorkspace(w))
            continue;
        if (m_savedStates.contains((uint64_t)w.get()))
            return w;
    }

    return nullptr;
}

SP<Desktop::View::CWindow> CCanvas::findCycleTarget(const SP<Desktop::View::CWindow>& source, bool previous) const {
    std::vector<SP<Desktop::View::CWindow>> windows;
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || w->isHidden() || !w->m_isMapped || !windowOnCanvasWorkspace(w))
            continue;

        auto it = m_savedStates.find((uint64_t)w.get());
        if (it != m_savedStates.end() && !it->second.pinned)
            windows.push_back(w);
    }

    if (windows.empty())
        return nullptr;

    auto it = std::find(windows.begin(), windows.end(), source);
    if (it == windows.end())
        return windows.front();

    const auto idx = (size_t)std::distance(windows.begin(), it);
    if (previous)
        return windows[(idx + windows.size() - 1) % windows.size()];

    return windows[(idx + 1) % windows.size()];
}

SP<Desktop::View::CWindow> CCanvas::findDirectionalTarget(const SP<Desktop::View::CWindow>& source, const std::string& direction) const {
    if (!source)
        return nullptr;

    const auto sourceIt = m_savedStates.find((uint64_t)source.get());
    if (sourceIt == m_savedStates.end())
        return nullptr;

    const Vector2D sourceCenter = sourceIt->second.canvasPos + sourceIt->second.canvasSize / 2.0;
    double bestScore = std::numeric_limits<double>::infinity();
    SP<Desktop::View::CWindow> best;

    for (auto& w : g_pCompositor->m_windows) {
        if (!w || w == source || w->isHidden() || !w->m_isMapped || !windowOnCanvasWorkspace(w))
            continue;

        const auto it = m_savedStates.find((uint64_t)w.get());
        if (it == m_savedStates.end() || it->second.pinned)
            continue;

        const Vector2D center = it->second.canvasPos + it->second.canvasSize / 2.0;
        const double dx = center.x - sourceCenter.x;
        const double dy = center.y - sourceCenter.y;

        double axial = 0.0;
        double lateral = 0.0;
        if (direction == "left" || direction == "l") {
            if (dx >= 0)
                continue;
            axial = -dx;
            lateral = std::abs(dy);
        } else if (direction == "right" || direction == "r") {
            if (dx <= 0)
                continue;
            axial = dx;
            lateral = std::abs(dy);
        } else if (direction == "up" || direction == "u") {
            if (dy >= 0)
                continue;
            axial = -dy;
            lateral = std::abs(dx);
        } else if (direction == "down" || direction == "d") {
            if (dy <= 0)
                continue;
            axial = dy;
            lateral = std::abs(dx);
        } else {
            return nullptr;
        }

        const double score = axial + (lateral * lateral) / (axial + 1.0);
        if (score < bestScore) {
            bestScore = score;
            best = w;
        }
    }

    return best;
}

void CCanvas::centerOnWindow(const SP<Desktop::View::CWindow>& window, ECommitMode mode) {
    if (!window)
        return;

    auto it = m_savedStates.find((uint64_t)window.get());
    if (it == m_savedStates.end())
        return;

    const Vector2D targetCenter = it->second.canvasPos + it->second.canvasSize / 2.0;
    offset = targetCenter - monitorCenter() / zoom;
    repositionWindows(mode);
}

void CCanvas::focusWindow(const SP<Desktop::View::CWindow>& window) const {
    if (!window)
        return;

    auto dispatcher = g_pKeybindManager->m_dispatchers.find("focuswindow");
    if (dispatcher == g_pKeybindManager->m_dispatchers.end())
        return;

    char arg[64];
    std::snprintf(arg, sizeof(arg), "address:0x%lx", (unsigned long)window.get());
    dispatcher->second(arg);
}

void CCanvas::commitWindow(const SP<Desktop::View::CWindow>& window, const Vector2D& pos, const Vector2D& size, ECommitMode mode) const {
    if (!window)
        return;

    g_pHyprRenderer->damageWindow(window);

    window->m_position = pos;
    window->m_size = size;

    if (mode == ECommitMode::Warp) {
        window->m_realPosition->setValueAndWarp(pos);
        window->m_realSize->setValueAndWarp(size);
    } else {
        *window->m_realPosition = pos;
        *window->m_realSize = size;
    }

    window->sendWindowSize();
    g_pHyprRenderer->damageWindow(window);
}

// --- Reposition all windows based on zoom+offset ---

void CCanvas::repositionWindows(ECommitMode mode) {
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || w->isHidden() || !w->m_isMapped || !windowOnCanvasWorkspace(w))
            continue;

        uint64_t id = (uint64_t)w.get();
        auto it = m_savedStates.find(id);
        if (it == m_savedStates.end())
            continue;

        const auto& saved = it->second;
        if (saved.pinned)
            continue;

        // Canvas-to-screen: screenPos = (canvasPos - offset) * zoom
        Vector2D newPos = {
            (saved.canvasPos.x - offset.x) * zoom,
            (saved.canvasPos.y - offset.y) * zoom
        };
        Vector2D newSize = {
            saved.canvasSize.x * zoom,
            saved.canvasSize.y * zoom
        };

        commitWindow(w, newPos, newSize, mode);
    }
}

bool CCanvas::workspaceChanged() const {
    if (!active)
        return false;

    auto mon = Desktop::focusState()->monitor();
    if (!mon)
        return false;

    return mon->activeWorkspaceID() != m_canvasWorkspace;
}

void CCanvas::resetForWorkspaceChange() {
    if (!active)
        return;

    auto mon = Desktop::focusState()->monitor();
    const auto currentWorkspace = mon ? mon->activeWorkspaceID() : WORKSPACE_INVALID;
    logf("[hypr-canvas] workspace change detected (%ld -> %ld), resetting canvas session\n",
         (long)m_canvasWorkspace, (long)currentWorkspace);
    exit();
}

bool CCanvas::windowOnCanvasWorkspace(const SP<Desktop::View::CWindow>& window) const {
    if (!window || !window->m_workspace)
        return false;

    return window->workspaceID() == m_canvasWorkspace;
}

// --- Zoom with cursor anchoring ---

void CCanvas::applyZoom(double newZoom, const Vector2D& anchorScreen) {
    // anchorScreen = point under cursor in screen coords
    // Find what canvas point is under cursor: canvasPoint = offset + anchorScreen / zoom
    const Vector2D anchorCanvas = {
        offset.x + anchorScreen.x / zoom,
        offset.y + anchorScreen.y / zoom
    };

    zoom = std::clamp(newZoom, ZOOM_MIN, ZOOM_MAX);

    // Adjust offset so anchorCanvas stays under cursor:
    // anchorScreen = (anchorCanvas - offset) * zoom
    // offset = anchorCanvas - anchorScreen / zoom
    offset = {
        anchorCanvas.x - anchorScreen.x / zoom,
        anchorCanvas.y - anchorScreen.y / zoom
    };
}

void CCanvas::pan(const Vector2D& delta) {
    offset.x += delta.x / zoom;
    offset.y += delta.y / zoom;
}
