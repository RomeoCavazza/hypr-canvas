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
#include <cctype>
#include <limits>
#include <vector>
#include <linux/input-event-codes.h>

static int getCfgInt(const std::string &key, int fallback) {
    if (!PHANDLE) return fallback;
    auto *cv = HyprlandAPI::getConfigValue(PHANDLE, key);
    if (!cv) return fallback;
    try {
        auto val = cv->getValue();
        if (val.has_value()) {
            return std::any_cast<Hyprlang::INT>(val);
        }
    } catch (...) {}
    return fallback;
}

static std::string getCfgString(const std::string &key, const std::string &fallback) {
    if (!PHANDLE) return fallback;
    auto *cv = HyprlandAPI::getConfigValue(PHANDLE, key);
    if (!cv) return fallback;
    try {
        auto val = cv->getValue();
        if (val.has_value()) {
            auto s = std::any_cast<Hyprlang::STRING>(val);
            if (s) return std::string{s};
        }
    } catch (...) {}
    return fallback;
}

static void logf(const char* fmt, ...) {
    if (getCfgInt("plugin:canvas:debug", 0) == 0) return;
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

            const double steps = (e.deltaDiscrete != 0) ? (double)e.deltaDiscrete / 120.0 : e.delta / 15.0;
            const double zoomFactor = std::pow(CCanvas::ZOOM_STEP, std::abs(steps));
            double       newZoom    = g_pCanvas->zoom;
            if (scrollDelta < 0)
                newZoom *= zoomFactor;
            else
                newZoom /= zoomFactor;

            // Cursor-anchored zoom for natural canvas navigation
            auto mon = Desktop::focusState()->monitor();
            if (!mon)
                return;

            const Vector2D cursorCoords = g_pInputManager->getMouseCoordsInternal();

            g_pCanvas->applyZoom(newZoom, cursorCoords);
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

typedef void (*onMouseButtonFn)(CInputManager*, IPointer::SButtonEvent, SP<IPointer>);

static void hkOnMouseButton(CInputManager* self, IPointer::SButtonEvent e, SP<IPointer> pointer) {
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
                original(self, e, pointer);
                return;
            }

            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                g_pCanvas->m_overviewActive = false;
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
        g_pCanvas->emitIPCEvent(true);
        return;
    }

    if (g_pCanvas && g_pCanvas->m_movingWindow && e.button == BTN_LEFT && e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
        logf("[hypr-canvas] window move stop id=%lx\n", g_pCanvas->m_dragWindow);
        g_pCanvas->m_movingWindow = false;
        g_pCanvas->m_dragWindow = 0;
        g_pCanvas->emitIPCEvent(true);
        return;
    }

    if (g_pCanvas && g_pCanvas->m_resizingWindow && e.button == BTN_RIGHT && e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
        logf("[hypr-canvas] window resize stop id=%lx\n", g_pCanvas->m_dragWindow);
        g_pCanvas->m_resizingWindow = false;
        g_pCanvas->m_dragWindow = 0;
        g_pCanvas->emitIPCEvent(true);
        return;
    }

    auto original = (onMouseButtonFn)g_pCanvas->m_mouseButtonHook->m_original;
    original(self, e, pointer);
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

SDispatchResult dispatchHome(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    g_pCanvas->ensureActive();
    g_pCanvas->home(ECommitMode::Animate);

    scheduleFrame();
    return {};
}



// --- canvas:nav -----------------------------------------------------------
// Context-aware arrows:
//   canvas inactive → delegate to Hyprland movefocus (tiled navigation)
//   canvas active   → navigate window focus (directional focus + centering)
// -------------------------------------------------------------------------
SDispatchResult dispatchNav(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    // Map arg → movefocus direction
    std::string mfDir;
    if      (args == "left")  mfDir = "l";
    else if (args == "right") mfDir = "r";
    else if (args == "up")    mfDir = "u";
    else if (args == "down")  mfDir = "d";
    else return {};

    if (!g_pCanvas->active) {
        // Tiled mode: forward to native movefocus
        auto it = g_pKeybindManager->m_dispatchers.find("movefocus");
        if (it != g_pKeybindManager->m_dispatchers.end())
            it->second(mfDir);
        return {};
    }

    // Canvas mode: navigate window focus in the specified direction
    g_pCanvas->nav(args);
    scheduleFrame();
    return {};
}

// --- canvas:swap ----------------------------------------------------------
// Context-aware window swap:
//   canvas inactive → delegate to Hyprland swapwindow
//   canvas active   → swap canvas position and size with directional target
// -------------------------------------------------------------------------
SDispatchResult dispatchSwap(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    std::string swDir;
    if      (args == "left")  swDir = "l";
    else if (args == "right") swDir = "r";
    else if (args == "up")    swDir = "u";
    else if (args == "down")  swDir = "d";
    else return {};

    if (!g_pCanvas->active) {
        auto it = g_pKeybindManager->m_dispatchers.find("swapwindow");
        if (it != g_pKeybindManager->m_dispatchers.end())
            it->second(swDir);
        return {};
    }

    g_pCanvas->swap(args);
    scheduleFrame();
    return {};
}

// --- canvas:pan ------------------------------------------------------------
// Explicit camera pan — only operates when canvas is already active.
// Does NOT auto-enter canvas (use canvas:toggle for that).
// -------------------------------------------------------------------------
SDispatchResult dispatchPan(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    // Guard: don't pull windows into canvas mode on an accidental nudge
    if (!g_pCanvas->active)
        return {};

    Vector2D delta = {0, 0};
    if (args == "left")       delta.x = CCanvas::PAN_STEP;
    else if (args == "right") delta.x = -CCanvas::PAN_STEP;
    else if (args == "up")    delta.y = CCanvas::PAN_STEP;
    else if (args == "down")  delta.y = -CCanvas::PAN_STEP;
    else
        return {};

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

    const Vector2D center = mon->m_position + mon->m_size / 2.0;

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

bool CCanvas::isProtectedApp(const SP<Desktop::View::CWindow>& window) const {
    if (!window) return false;

    std::string classname = window->m_class;
    std::string title = window->m_title;

    // Helper lambda for case-insensitive substring search
    auto contains = [](const std::string& str, const std::string& sub) {
        auto it = std::search(
            str.begin(), str.end(),
            sub.begin(), sub.end(),
            [](unsigned char ch1, unsigned char ch2) {
                return std::tolower(ch1) == std::tolower(ch2);
            }
        );
        return it != str.end();
    };

    // Auto-exclude overlays, launchers, lockscreens, sharing pickers, panels, screenshot tools, and portal popups
    if (contains(classname, "rofi") ||
        contains(classname, "slurp") ||
        contains(classname, "grim") ||
        contains(classname, "waybar") ||
        contains(classname, "pinentry") ||
        contains(classname, "polkit") ||
        contains(classname, "share-picker") ||
        contains(classname, "portal") ||
        contains(classname, "lock") ||
        contains(classname, "screenshot") ||
        contains(classname, "hyprspace") ||
        contains(title, "rofi") ||
        contains(title, "screenshot")) {
        return true;
    }

    std::string protectedApps = getCfgString("plugin:canvas:protected_apps", "");
    if (protectedApps.empty()) return false;

    std::stringstream ss(protectedApps);
    std::string token;
    while (std::getline(ss, token, ',')) {
        size_t first = token.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        size_t last = token.find_last_not_of(' ');
        token = token.substr(first, (last - first + 1));

        if (!token.empty() && (contains(classname, token) || contains(title, token))) {
            return true;
        }
    }
    return false;
}

void CCanvas::emitIPCEvent(bool force) {
    if (!g_pEventManager) return;
    static std::chrono::steady_clock::time_point lastEmit;
    static std::string lastPayload;

    std::string payload = std::format("{},{:.2f},{:.1f},{:.1f}", active ? 1 : 0, zoom, offset.x, offset.y);
    if (!force && payload == lastPayload) return;

    auto now = std::chrono::steady_clock::now();
    if (!force && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit).count() < 33) {
        return;
    }

    lastEmit = now;
    lastPayload = payload;
    g_pEventManager->postEvent({ "canvas", payload });
}

// --- Constructor / Destructor ---

CCanvas::CCanvas() {
    m_mouseWheelHook  = hookByName("onMouseWheel", (void*)&hkOnMouseWheel);
    m_mouseButtonHook = hookByName("onMouseButton", (void*)&hkOnMouseButton);
    m_mouseMovedHook  = hookByName("onMouseMoved", (void*)&hkOnMouseMoved);

    m_destroyWindowListener = Event::bus()->m_events.window.destroy.listen([this](PHLWINDOW w) {
        forgetWindow(w, false);
    });
    m_closeWindowListener = Event::bus()->m_events.window.close.listen([this](PHLWINDOW w) {
        forgetWindow(w, true);
    });
    m_openWindowListener = Event::bus()->m_events.window.open.listen([this](PHLWINDOW w) {
        onWindowOpen(w);
    });
    m_moveWindowWorkspaceListener = Event::bus()->m_events.window.moveToWorkspace.listen([this](PHLWINDOW w, PHLWORKSPACE ws) {
        if (!w || !ws)
            return;

        const uint64_t id = (uint64_t)w.get();
        if (active && m_savedStates.contains(id) && ws->m_id != m_canvasWorkspace)
            forgetWindow(w, true);
        else if (active && ws->m_id == m_canvasWorkspace)
            onWindowOpen(w);
    });
    m_openLayerListener = Event::bus()->m_events.layer.opened.listen([this](PHLLS) {
        logf("[hypr-canvas] layer opened: reasserting canvas geometry\n");
        requestReassert();
    });
    m_closeLayerListener = Event::bus()->m_events.layer.closed.listen([this](PHLLS) {
        logf("[hypr-canvas] layer closed: reasserting canvas geometry\n");
        requestReassert();
    });
    m_tickListener = Event::bus()->m_events.tick.listen([this]() {
        stabilizeAfterEvent();
    });
    m_workspaceActiveListener = Event::bus()->m_events.workspace.active.listen([this](PHLWORKSPACE ws) {
        if (!ws) return;
        if (active && ws->m_id != m_canvasWorkspace) {
            logf("[hypr-canvas] workspace switched (active on %ld, switched to %ld), suspending canvas session\n",
                 (long)m_canvasWorkspace, (long)ws->m_id);
            suspendForWorkspaceChange();
        }

        auto stateIt = m_workspaceStates.find(ws->m_id);
        if (!active && stateIt != m_workspaceStates.end() && stateIt->second.resumeOnWorkspaceFocus) {
            logf("[hypr-canvas] resuming suspended canvas session on workspace %ld\n", (long)ws->m_id);
            enter();
        }
    });

    m_swipeBeginListener = Event::bus()->m_events.gesture.swipe.begin.listen([this](IPointer::SSwipeBeginEvent e, Event::SCallbackInfo& info) {
        if (!active) return;
        m_panning = true;
        info.cancelled = true;
    });
    m_swipeUpdateListener = Event::bus()->m_events.gesture.swipe.update.listen([this](IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) {
        if (!active) return;
        if (e.fingers == 3 || e.fingers == 4) {
            offset.x -= e.delta.x / zoom;
            offset.y -= e.delta.y / zoom;
            repositionWindows(ECommitMode::Warp);
            scheduleFrame();
            info.cancelled = true;
        }
    });
    m_swipeEndListener = Event::bus()->m_events.gesture.swipe.end.listen([this](IPointer::SSwipeEndEvent e, Event::SCallbackInfo& info) {
        if (!active) return;
        m_panning = false;
        info.cancelled = true;
        emitIPCEvent(true);
    });

    m_pinchBeginListener = Event::bus()->m_events.gesture.pinch.begin.listen([this](IPointer::SPinchBeginEvent e, Event::SCallbackInfo& info) {
        if (!active) return;
        m_pinchStartZoom = zoom;
        info.cancelled = true;
    });
    m_pinchUpdateListener = Event::bus()->m_events.gesture.pinch.update.listen([this](IPointer::SPinchUpdateEvent e, Event::SCallbackInfo& info) {
        if (!active) return;
        if (e.fingers == 2 || e.fingers == 3) {
            double newZoom = m_pinchStartZoom * e.scale;
            Vector2D cursorCoords = g_pInputManager->getMouseCoordsInternal();
            applyZoom(newZoom, cursorCoords);
            repositionWindows(ECommitMode::Warp);
            scheduleFrame();
            info.cancelled = true;
        }
    });
    m_pinchEndListener = Event::bus()->m_events.gesture.pinch.end.listen([this](IPointer::SPinchEndEvent e, Event::SCallbackInfo& info) {
        if (!active) return;
        info.cancelled = true;
        emitIPCEvent(true);
    });

    logf("[hypr-canvas] initialized (VXWM mode — no render hooks)\n");
}

CCanvas::~CCanvas() {
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
    m_canvasWorkspace = mon->activeWorkspaceID();
    m_panning = false;
    m_movingWindow = false;
    m_resizingWindow = false;
    m_dragWindow = 0;

    // Fetch or initialize workspace state (shape memory)
    auto& wsState = m_workspaceStates[m_canvasWorkspace];
    const bool resumingSuspended = wsState.resumeOnWorkspaceFocus;
    zoom = wsState.zoom;
    offset = wsState.offset;
    m_overviewActive = wsState.overviewActive;
    m_overviewSavedPos = wsState.overviewSavedPos;

    std::vector<SEntry> entries;
    std::vector<size_t> tiledEntries;
    auto focused = Desktop::focusState()->window();

    for (auto& w : g_pCompositor->m_windows) {
        if (!w || w->isHidden() || !w->m_isMapped || !windowOnCanvasWorkspace(w))
            continue;

        SEntry entry;
        entry.window = w;
        entry.id = (uint64_t)w.get();
        entry.currentPos = w->m_position;
        entry.currentSize = w->m_size;
        entry.center = entry.currentPos + entry.currentSize / 2.0;
        entry.wasFloating = w->m_isFloating;

        if (!entry.wasFloating)
            tiledEntries.push_back(entries.size());

        entries.push_back(entry);
    }

    // Remove any windows from wsState.savedStates that are no longer on the workspace
    for (auto it = wsState.savedStates.begin(); it != wsState.savedStates.end(); ) {
        uint64_t id = it->first;
        bool found = false;
        for (const auto& entry : entries) {
            if (entry.id == id) {
                found = true;
                break;
            }
        }
        if (!found) {
            it = wsState.savedStates.erase(it);
        } else {
            ++it;
        }
    }

    // Now populate m_savedStates from wsState.savedStates
    m_savedStates = wsState.savedStates;

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

        if (m_savedStates.contains(entry.id)) {
            auto& saved = m_savedStates[entry.id];
            if (!resumingSuspended) {
                saved.restorePos = entry.currentPos;
                saved.restoreSize = entry.currentSize;
                saved.wasFloating = entry.wasFloating;
            }
            saved.pinned = (w->m_pinned || isProtectedApp(w));
        } else {
            SWindowState state;
            state.restorePos  = entry.currentPos;
            state.restoreSize = entry.currentSize;
            state.wasFloating = entry.wasFloating;
            state.pinned      = (w->m_pinned || isProtectedApp(w));

            if (state.pinned) {
                state.canvasPos  = entry.currentPos;
                state.canvasSize = entry.currentSize;
            } else if (entry.wasFloating) {
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
        }

        const auto& state = m_savedStates[entry.id];
        if (state.pinned)
            continue;

        g_pHyprRenderer->damageWindow(w);

        if (!w->m_isFloating) {
            w->m_isFloating = true;
        }

        Vector2D screenPos = (state.canvasPos - offset) * zoom;
        Vector2D screenSize = state.canvasSize * zoom;
        commitWindow(w, screenPos, screenSize, ECommitMode::Warp);
        g_pHyprRenderer->damageWindow(w);
    }

    logf("[hypr-canvas] entered canvas mode on workspace %ld, saved/restored %zu windows\n",
         (long)m_canvasWorkspace, m_savedStates.size());
    wsState.resumeOnWorkspaceFocus = false;
    persistWorkspaceState();
    emitIPCEvent(true);
}

void CCanvas::exit() {
    // Save current layout to workspace configurations before restoring layout settings
    SWorkspaceCanvasState wsState;
    wsState.savedStates = m_savedStates;
    wsState.overviewSavedPos = m_overviewSavedPos;
    wsState.zoom = zoom;
    wsState.offset = offset;
    wsState.overviewActive = m_overviewActive;
    wsState.resumeOnWorkspaceFocus = false;
    m_workspaceStates[m_canvasWorkspace] = wsState;

    // Restore all saved window positions and float state
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || w->isHidden() || !w->m_isMapped || !windowOnCanvasWorkspace(w))
            continue;

        uint64_t id = (uint64_t)w.get();
        auto it = m_savedStates.find(id);
        if (it == m_savedStates.end())
            continue;

        const auto& saved = it->second;
        if (saved.pinned || w->m_pinned || isProtectedApp(w))
            continue;

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
    m_pendingReassertFrames = 0;
    m_overviewActive = false;
    m_overviewSavedPos.clear();

    // Force relayout to snap windows back to tiling
    auto mon = Desktop::focusState()->monitor();
    if (mon)
        g_layoutManager->recalculateMonitor(mon);

    logf("[hypr-canvas] exited canvas mode, restored windows\n");
    emitIPCEvent(true);
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

    return mon->m_position + mon->m_size / 2.0;
}




void CCanvas::centerActive(ECommitMode mode) {
    centerOnWindow(activeCanvasWindow(), mode);
}

void CCanvas::home(ECommitMode mode) {
    zoom = 1.0;
    centerActive(mode);
}

void CCanvas::nav(const std::string& direction) {
    if (!active)
        return;

    int cooldownMs = getCfgInt("plugin:canvas:nav_cooldown_ms", 150);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastNavTime).count();
    if (elapsed < cooldownMs) {
        logf("[hypr-canvas] nav throttled by cooldown (%ld ms < %d ms)\n", (long)elapsed, cooldownMs);
        return;
    }
    m_lastNavTime = now;

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

void CCanvas::swap(const std::string& direction) {
    if (!active)
        return;

    auto source = activeCanvasWindow();
    if (!source)
        return;

    auto target = findDirectionalTarget(source, direction);
    if (!target || target == source)
        return;

    auto sourceIt = m_savedStates.find((uint64_t)source.get());
    auto targetIt = m_savedStates.find((uint64_t)target.get());
    if (sourceIt == m_savedStates.end() || targetIt == m_savedStates.end())
        return;

    if (sourceIt->second.pinned || targetIt->second.pinned)
        return;

    // Like drag from overview: an explicit edit accepts the packed shape.
    if (m_overviewActive) {
        m_overviewActive = false;
        m_overviewSavedPos.clear();
    }

    std::swap(sourceIt->second.canvasPos, targetIt->second.canvasPos);
    std::swap(sourceIt->second.canvasSize, targetIt->second.canvasSize);

    centerOnWindow(source, ECommitMode::Animate);
    focusWindow(source);
    logf("[hypr-canvas] swap %s source=%lx target=%lx\n",
         direction.c_str(), (unsigned long)source.get(), (unsigned long)target.get());
    emitIPCEvent(true);
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
    target->m_pinned = it->second.pinned;
    if (!it->second.pinned) {
        it->second.canvasPos = screenToCanvas(target->m_realPosition->value());
        it->second.canvasSize = target->m_realSize->value() / zoom;
    }

    logf("[hypr-canvas] pin id=%lx pinned=%d\n", id, it->second.pinned ? 1 : 0);
    emitIPCEvent(true);
}

SP<Desktop::View::CWindow> CCanvas::activeCanvasWindow() const {
    auto focused = Desktop::focusState()->window();
    if (focused && windowOnCanvasWorkspace(focused)) {
        auto it = m_savedStates.find((uint64_t)focused.get());
        if (it != m_savedStates.end() && !it->second.pinned && !focused->m_pinned && !isProtectedApp(focused))
            return focused;
    }

    return firstCanvasWindow();
}

SP<Desktop::View::CWindow> CCanvas::firstCanvasWindow() const {
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || w->isHidden() || !w->m_isMapped || !windowOnCanvasWorkspace(w))
            continue;
        auto it = m_savedStates.find((uint64_t)w.get());
        if (it != m_savedStates.end() && !it->second.pinned && !w->m_pinned && !isProtectedApp(w))
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

    const bool sizeChanged =
        std::abs(window->m_size.x - size.x) > 0.5 ||
        std::abs(window->m_size.y - size.y) > 0.5;

    window->m_position = pos;
    window->m_size = size;

    if (mode == ECommitMode::Warp) {
        window->m_realPosition->setValueAndWarp(pos);
        window->m_realSize->setValueAndWarp(size);
    } else {
        *window->m_realPosition = pos;
        *window->m_realSize = size;
    }

    if (sizeChanged)
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
        if (saved.pinned || w->m_pinned || isProtectedApp(w))
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
    emitIPCEvent();
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
    logf("[hypr-canvas] workspace change detected (%ld -> %ld), suspending canvas session\n",
         (long)m_canvasWorkspace, (long)currentWorkspace);
    suspendForWorkspaceChange();

    if (currentWorkspace != WORKSPACE_INVALID) {
        auto stateIt = m_workspaceStates.find(currentWorkspace);
        if (stateIt != m_workspaceStates.end() && stateIt->second.resumeOnWorkspaceFocus)
            enter();
    }
}

void CCanvas::suspendForWorkspaceChange() {
    if (!active || m_canvasWorkspace == WORKSPACE_INVALID)
        return;

    persistWorkspaceState();
    auto& wsState = m_workspaceStates[m_canvasWorkspace];
    wsState.resumeOnWorkspaceFocus = true;

    m_savedStates.clear();
    active = false;
    zoom = 1.0;
    offset = {0, 0};
    m_canvasWorkspace = WORKSPACE_INVALID;
    m_panning = false;
    m_movingWindow = false;
    m_resizingWindow = false;
    m_dragWindow = 0;
    m_pendingStabilize = false;
    m_pendingRefocus = false;
    m_pendingReassertFrames = 0;
    m_overviewActive = false;
    m_overviewSavedPos.clear();

    emitIPCEvent(true);
}

void CCanvas::persistWorkspaceState() {
    if (!active || m_canvasWorkspace == WORKSPACE_INVALID)
        return;

    auto& wsState = m_workspaceStates[m_canvasWorkspace];
    wsState.savedStates = m_savedStates;
    wsState.overviewSavedPos = m_overviewSavedPos;
    wsState.zoom = zoom;
    wsState.offset = offset;
    wsState.overviewActive = m_overviewActive;
}

void CCanvas::forgetWindow(const SP<Desktop::View::CWindow>& window, bool stabilize) {
    if (!window)
        return;

    const uint64_t id = (uint64_t)window.get();
    const bool wasFocused = Desktop::focusState()->window() == window;
    const bool wasManaged = m_savedStates.contains(id);

    m_savedStates.erase(id);
    m_overviewSavedPos.erase(id);
    for (auto& [wsId, wsState] : m_workspaceStates) {
        wsState.savedStates.erase(id);
        wsState.overviewSavedPos.erase(id);
    }

    if (m_dragWindow == id) {
        m_panning = false;
        m_movingWindow = false;
        m_resizingWindow = false;
        m_dragWindow = 0;
    }

    if (active && !wasManaged && (isProtectedApp(window) || windowOnCanvasWorkspace(window)))
        requestReassert();

    if (!active || !wasManaged)
        return;

    if (m_savedStates.empty()) {
        m_overviewActive = false;
        m_overviewSavedPos.clear();
        persistWorkspaceState();
        emitIPCEvent(true);
        return;
    }

    persistWorkspaceState();
    if (stabilize)
        requestStabilize(wasFocused);
}

void CCanvas::requestStabilize(bool refocus) {
    if (!active)
        return;

    m_pendingStabilize = true;
    m_pendingRefocus = m_pendingRefocus || refocus;
}

void CCanvas::requestReassert(int frames) {
    if (!active)
        return;

    m_pendingReassertFrames = std::max(m_pendingReassertFrames, frames);
}

void CCanvas::stabilizeAfterEvent() {
    if (!active || (!m_pendingStabilize && m_pendingReassertFrames <= 0))
        return;

    if (m_savedStates.empty()) {
        m_pendingStabilize = false;
        m_pendingRefocus = false;
        m_pendingReassertFrames = 0;
        m_overviewActive = false;
        m_overviewSavedPos.clear();
        persistWorkspaceState();
        emitIPCEvent(true);
        return;
    }

    if (m_pendingStabilize) {
        m_pendingStabilize = false;

        if (m_pendingRefocus) {
            auto next = firstCanvasWindow();
            if (next) {
                centerOnWindow(next, ECommitMode::Animate);
                focusWindow(next);
            } else {
                repositionWindows(ECommitMode::Animate);
            }
        } else {
            repositionWindows(ECommitMode::Animate);
        }

        m_pendingRefocus = false;
        persistWorkspaceState();
    }

    if (m_pendingReassertFrames > 0) {
        --m_pendingReassertFrames;
        repositionWindows(ECommitMode::Warp);
    }

    scheduleFrame();
    emitIPCEvent(true);
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

void CCanvas::onWindowOpen(const SP<Desktop::View::CWindow>& w) {
    if (!active || !w) return;

    if (isProtectedApp(w)) {
        requestReassert();
        return;
    }

    if (w->isHidden() || !windowOnCanvasWorkspace(w))
        return;

    uint64_t id = (uint64_t)w.get();
    if (m_savedStates.contains(id))
        return;

    logf("[hypr-canvas] new window opened while canvas active: id=%lx class=%s title=%s\n",
         id, w->m_class.c_str(), w->m_title.c_str());

    w->m_isFloating = true;

    SWindowState state;
    state.restorePos = w->m_realPosition->value();
    state.restoreSize = w->m_realSize->value();
    state.wasFloating = true;
    state.pinned = false;

    Vector2D initialSize = w->m_realSize->value();
    if (initialSize.x < CCanvas::MIN_WINDOW_W || initialSize.y < CCanvas::MIN_WINDOW_H) {
        initialSize = {800, 600};
    }

    Vector2D centerScreen = monitorCenter();
    Vector2D centerCanvas = screenToCanvas(centerScreen);

    state.canvasSize = initialSize;
    state.canvasPos = centerCanvas - initialSize / 2.0;

    m_savedStates[id] = state;
    persistWorkspaceState();
    requestStabilize(false);

    g_pHyprRenderer->damageWindow(w);
    scheduleFrame();
}

SDispatchResult dispatchOverview(std::string args) {
    if (!g_pCanvas)
        return {};

    if (g_pCanvas->workspaceChanged())
        g_pCanvas->resetForWorkspaceChange();

    g_pCanvas->ensureActive();

    if (g_pCanvas->m_overviewActive) {
        // --- Toggle OFF: restore original canvas positions ---
        g_pCanvas->m_overviewActive = false;

        for (auto& [id, state] : g_pCanvas->m_savedStates) {
            auto it = g_pCanvas->m_overviewSavedPos.find(id);
            if (it != g_pCanvas->m_overviewSavedPos.end())
                state.canvasPos = it->second;
        }
        g_pCanvas->m_overviewSavedPos.clear();
        g_pCanvas->repositionWindows(ECommitMode::Animate);
        g_pCanvas->persistWorkspaceState();
        logf("[hypr-canvas] overview OFF: restored original positions\n");

    } else {
        // --- Toggle ON: pack all non-pinned windows into a tight cluster ---

        // Collect non-pinned windows, save current positions
        struct WEntry {
            uint64_t   id;
            Vector2D   size;
            Vector2D   origPos;
        };
        std::vector<WEntry> entries;

        g_pCanvas->m_overviewSavedPos.clear();
        for (auto& [id, state] : g_pCanvas->m_savedStates) {
            if (state.pinned) continue;
            g_pCanvas->m_overviewSavedPos[id] = state.canvasPos;
            entries.push_back({ id, state.canvasSize, state.canvasPos });
        }

        if (entries.empty()) {
            logf("[hypr-canvas] overview: no windows to pack\n");
            return {};
        }

        // Sort windows by area descending (largest first) to put the largest in the center
        std::sort(entries.begin(), entries.end(), [](const WEntry& a, const WEntry& b) {
            return (a.size.x * a.size.y) > (b.size.x * b.size.y);
        });

        // Tight gap between windows
        constexpr double GAP = 18.0;

        struct Placed {
            uint64_t id;
            Vector2D pos;
            Vector2D size;
        };
        std::vector<Placed> placed;

        // Helper to check overlap
        auto overlaps = [&](const Vector2D& pos, const Vector2D& size) {
            for (const auto& p : placed) {
                // Check intersection with small epsilon to avoid float inaccuracies
                bool intersects = !(pos.x + size.x + GAP - 0.1 <= p.pos.x ||
                                    p.pos.x + p.size.x + GAP - 0.1 <= pos.x ||
                                    pos.y + size.y + GAP - 0.1 <= p.pos.y ||
                                    p.pos.y + p.size.y + GAP - 0.1 <= pos.y);
                if (intersects) return true;
            }
            return false;
        };

        // Center of the first (largest) window as the target center
        Vector2D targetCenter = {0, 0};

        double minX = 999999.0, maxX = -999999.0;
        double minY = 999999.0, maxY = -999999.0;

        for (size_t i = 0; i < entries.size(); ++i) {
            auto& e = entries[i];
            if (i == 0) {
                // Place the largest window at (0,0)
                placed.push_back({ e.id, {0, 0}, e.size });
                targetCenter = e.size / 2.0;
                minX = 0;
                maxX = e.size.x;
                minY = 0;
                maxY = e.size.y;
                continue;
            }

            // Find best candidate position
            Vector2D bestPos = {0, 0};
            double bestDist = std::numeric_limits<double>::max();
            bool found = false;

            // Helper to evaluate a candidate position
            auto evalCandidate = [&](const Vector2D& cand) {
                if (overlaps(cand, e.size)) return;
                Vector2D candCenter = cand + e.size / 2.0;
                double dist = candCenter.distance(targetCenter);
                if (dist < bestDist) {
                    bestDist = dist;
                    bestPos = cand;
                    found = true;
                }
            };

            for (const auto& p : placed) {
                // 1. Right of p
                double rx = p.pos.x + p.size.x + GAP;
                evalCandidate({ rx, p.pos.y }); // Align top
                evalCandidate({ rx, p.pos.y + (p.size.y - e.size.y) / 2.0 }); // Align center
                evalCandidate({ rx, p.pos.y + p.size.y - e.size.y }); // Align bottom

                // 2. Left of p
                double lx = p.pos.x - (e.size.x + GAP);
                evalCandidate({ lx, p.pos.y }); // Align top
                evalCandidate({ lx, p.pos.y + (p.size.y - e.size.y) / 2.0 }); // Align center
                evalCandidate({ lx, p.pos.y + p.size.y - e.size.y }); // Align bottom

                // 3. Bottom of p
                double by = p.pos.y + p.size.y + GAP;
                evalCandidate({ p.pos.x, by }); // Align left
                evalCandidate({ p.pos.x + (p.size.x - e.size.x) / 2.0, by }); // Align center
                evalCandidate({ p.pos.x + p.size.x - e.size.x, by }); // Align right

                // 4. Top of p
                double ty = p.pos.y - (e.size.y + GAP);
                evalCandidate({ p.pos.x, ty }); // Align left
                evalCandidate({ p.pos.x + (p.size.x - e.size.x) / 2.0, ty }); // Align center
                evalCandidate({ p.pos.x + p.size.x - e.size.x, ty }); // Align right
            }

            if (!found) {
                // Fallback: place it somewhere safe (e.g. far right)
                bestPos = { (placed.back().pos.x + placed.back().size.x + GAP), 0.0 };
            }

            minX = std::min(minX, bestPos.x);
            maxX = std::max(maxX, bestPos.x + e.size.x);
            minY = std::min(minY, bestPos.y);
            maxY = std::max(maxY, bestPos.y + e.size.y);

            placed.push_back({ e.id, bestPos, e.size });
        }

        double totalW = maxX - minX;
        double totalH = maxY - minY;

        // Center the cluster around the current viewport center in canvas space
        const Vector2D viewportCenter = g_pCanvas->screenToCanvas(g_pCanvas->monitorCenter());
        const Vector2D clusterOrigin  = {
            viewportCenter.x - (minX + maxX) / 2.0,
            viewportCenter.y - (minY + maxY) / 2.0
        };

        // Assign new positions
        for (auto& p : placed) {
            auto it = g_pCanvas->m_savedStates.find(p.id);
            if (it != g_pCanvas->m_savedStates.end())
                it->second.canvasPos = clusterOrigin + p.pos;
        }

        g_pCanvas->m_overviewActive = true;
        g_pCanvas->repositionWindows(ECommitMode::Animate);
        g_pCanvas->persistWorkspaceState();
        logf("[hypr-canvas] overview ON: packed %zu windows (totalW=%.0f totalH=%.0f)\n",
             entries.size(), totalW, totalH);
    }

    scheduleFrame();
    return {};
}
