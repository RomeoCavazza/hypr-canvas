#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

#include <chrono>
#include <map>
#include <string>
#include <sstream>

enum class ECommitMode {
    Warp,
    Animate,
};

// Saved window state for canvas mode
struct SWindowState {
    Vector2D restorePos;
    Vector2D restoreSize;
    Vector2D canvasPos;
    Vector2D canvasSize;
    bool     wasFloating;
    bool     pinned = false;
};

class CCanvas {
  public:
    CCanvas();
    ~CCanvas();

    // Canvas state
    double   zoom   = 1.0;
    Vector2D offset = {0, 0};   // canvas-space offset of viewport origin
    bool     active = false;     // canvas mode on/off
    WORKSPACEID m_canvasWorkspace = WORKSPACE_INVALID;

    // Saved window states (keyed by window address)
    std::map<uint64_t, SWindowState> m_savedStates;

    // Enter/exit canvas mode
    void enter();
    void exit();
    void ensureActive();

    // Coordinate transforms
    Vector2D screenToCanvas(const Vector2D& screen) const;
    Vector2D canvasToScreen(const Vector2D& canvas) const;
    Vector2D monitorCenter() const;

    // Apply zoom (cursor-anchored)
    void applyZoom(double newZoom, const Vector2D& anchorScreen);

    // Pan by delta in screen pixels
    void pan(const Vector2D& delta);


    void centerActive(ECommitMode mode);
    void nav(const std::string& direction);
    void togglePin();

    // Reposition all windows based on current zoom+offset
    void repositionWindows(ECommitMode mode);
    bool workspaceChanged() const;
    void resetForWorkspaceChange();
    bool windowOnCanvasWorkspace(const SP<Desktop::View::CWindow>& window) const;
    SP<Desktop::View::CWindow> activeCanvasWindow() const;
    SP<Desktop::View::CWindow> firstCanvasWindow() const;
    SP<Desktop::View::CWindow> findDirectionalTarget(const SP<Desktop::View::CWindow>& source, const std::string& direction) const;
    SP<Desktop::View::CWindow> findCycleTarget(const SP<Desktop::View::CWindow>& source, bool previous) const;
    void centerOnWindow(const SP<Desktop::View::CWindow>& window, ECommitMode mode);
    void focusWindow(const SP<Desktop::View::CWindow>& window) const;
    void commitWindow(const SP<Desktop::View::CWindow>& window, const Vector2D& pos, const Vector2D& size, ECommitMode mode) const;

    // Constants
    static constexpr double ZOOM_MIN  = 0.1;
    static constexpr double ZOOM_MAX  = 2.0;
    static constexpr double ZOOM_STEP = 1.03;
    static constexpr double PAN_STEP  = 120.0;
    static constexpr double CANVAS_REF_W = 939.0;
    static constexpr double CANVAS_REF_H = 1136.0;
    static constexpr double CARD_GAP = 80.0;
    static constexpr double MIN_WINDOW_W = 160.0;
    static constexpr double MIN_WINDOW_H = 120.0;

    // Panning state
    bool m_panning = false;
    bool m_movingWindow = false;
    bool m_resizingWindow = false;
    uint64_t m_dragWindow = 0;

    // Overview & gesture state
    bool m_overviewActive = false;
    std::map<uint64_t, Vector2D> m_overviewSavedPos; // canvasPos before cluster rearrange
    double m_pinchStartZoom = 1.0;

    // Hooks — only mouse input hooks needed (no render hooks!)
    CFunctionHook* m_mouseWheelHook  = nullptr;
    CFunctionHook* m_mouseButtonHook = nullptr;
    CFunctionHook* m_mouseMovedHook  = nullptr;

    // IPC & Protected apps
    void emitIPCEvent(bool force = false);
    bool isProtectedApp(const SP<Desktop::View::CWindow>& window) const;
    void onWindowOpen(const SP<Desktop::View::CWindow>& window);

    // Cooldown & Signal listeners
    std::chrono::steady_clock::time_point m_lastNavTime;
    CHyprSignalListener m_destroyWindowListener;
    CHyprSignalListener m_closeWindowListener;
    CHyprSignalListener m_openWindowListener;
    CHyprSignalListener m_swipeBeginListener;
    CHyprSignalListener m_swipeUpdateListener;
    CHyprSignalListener m_swipeEndListener;
    CHyprSignalListener m_pinchBeginListener;
    CHyprSignalListener m_pinchUpdateListener;
    CHyprSignalListener m_pinchEndListener;
};

inline std::unique_ptr<CCanvas> g_pCanvas;
inline HANDLE                   PHANDLE = nullptr;
