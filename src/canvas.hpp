#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

#include <chrono>
#include <map>
#include <string>
#include <sstream>

enum class ECommitMode {
    Warp,
    Animate,
};

struct SWindowState {
    Vector2D restorePos;
    Vector2D restoreSize;
    Vector2D canvasPos;
    Vector2D canvasSize;
    bool     wasFloating;
    bool     pinned = false;
};

struct SWorkspaceCanvasState {
    std::map<uint64_t, SWindowState> savedStates;
    std::map<uint64_t, Vector2D> overviewSavedPos;
    double zoom = 1.0;
    Vector2D offset = {0, 0};
    bool overviewActive = false;
    bool resumeOnWorkspaceFocus = false;
};

class CCanvas {
  public:
    CCanvas();
    ~CCanvas();

    double   zoom   = 1.0;
    Vector2D offset = {0, 0};
    bool     active = false;
    WORKSPACEID m_canvasWorkspace = WORKSPACE_INVALID;

    std::map<uint64_t, SWindowState> m_savedStates;

    std::map<WORKSPACEID, SWorkspaceCanvasState> m_workspaceStates;

    void enter();
    void exit();
    void ensureActive();

    Vector2D screenToCanvas(const Vector2D& screen) const;
    Vector2D canvasToScreen(const Vector2D& canvas) const;
    Vector2D canvasSizeToScreen(const Vector2D& canvasSize) const;
    Vector2D monitorCenter() const;

    void applyZoom(double newZoom, const Vector2D& anchorScreen);

    void pan(const Vector2D& delta);


    void centerActive(ECommitMode mode);
    void home(ECommitMode mode);
    void nav(const std::string& direction);
    void swap(const std::string& direction);
    void togglePin();

    void repositionWindows(ECommitMode mode);
    bool workspaceChanged() const;
    void resetForWorkspaceChange();
    void suspendForWorkspaceChange();
    void persistWorkspaceState();
    void forgetWindow(const SP<Desktop::View::CWindow>& window, bool stabilize);
    void requestStabilize(bool refocus);
    void requestReassert(int frames = 6);
    void stabilizeAfterEvent();
    bool windowOnCanvasWorkspace(const SP<Desktop::View::CWindow>& window) const;
    SP<Desktop::View::CWindow> activeCanvasWindow() const;
    SP<Desktop::View::CWindow> firstCanvasWindow() const;
    SP<Desktop::View::CWindow> findDirectionalTarget(const SP<Desktop::View::CWindow>& source, const std::string& direction) const;
    SP<Desktop::View::CWindow> findCycleTarget(const SP<Desktop::View::CWindow>& source, bool previous) const;
    void centerOnWindow(const SP<Desktop::View::CWindow>& window, ECommitMode mode);
    void focusWindow(const SP<Desktop::View::CWindow>& window) const;
    void setWindowFloating(const SP<Desktop::View::CWindow>& window, bool floating) const;
    void commitWindow(const SP<Desktop::View::CWindow>& window, const Vector2D& pos, const Vector2D& size, ECommitMode mode, bool notifyClient = true) const;
    void attachWindowTransformer(const SP<Desktop::View::CWindow>& window) const;
    void detachWindowTransformer(const SP<Desktop::View::CWindow>& window) const;
    void detachAllWindowTransformers() const;
    Vector2D logicalWindowPos(const SWindowState& state) const;
    Vector2D logicalWindowSize(const SWindowState& state) const;
    Vector2D visualWindowPos(const SP<Desktop::View::CWindow>& window, const SWindowState& state) const;
    Vector2D visualWindowSize(const SP<Desktop::View::CWindow>& window, const SWindowState& state) const;
    CBox visualWindowBox(const SP<Desktop::View::CWindow>& window, const SWindowState& state) const;
    Vector2D visualPointToLogicalPoint(const SP<Desktop::View::CWindow>& window, const SWindowState& state, const Vector2D& point) const;
    SP<Desktop::View::CWindow> windowAtVisualPoint(const Vector2D& point) const;

    static constexpr double ZOOM_MIN  = 0.1;
    static constexpr double ZOOM_MAX  = 2.0;
    static constexpr double ZOOM_STEP = 1.06;
    static constexpr double PAN_STEP  = 120.0;
    static constexpr double CANVAS_REF_W = 939.0;
    static constexpr double CANVAS_REF_H = 1136.0;
    static constexpr double CARD_GAP = 80.0;
    static constexpr double MIN_WINDOW_W = 160.0;
    static constexpr double MIN_WINDOW_H = 120.0;

    bool m_panning = false;
    bool m_movingWindow = false;
    bool m_resizingWindow = false;
    uint64_t m_dragWindow = 0;

    bool m_overviewActive = false;
    std::map<uint64_t, Vector2D> m_overviewSavedPos;
    double m_pinchStartZoom = 1.0;
    bool m_pendingStabilize = false;
    bool m_pendingRefocus = false;
    int  m_pendingReassertFrames = 0;

    CFunctionHook* m_mouseWheelHook  = nullptr;
    CFunctionHook* m_mouseButtonHook = nullptr;
    CFunctionHook* m_mouseMovedHook  = nullptr;
    CFunctionHook* m_windowHitHook   = nullptr;
    CFunctionHook* m_surfaceHitHook  = nullptr;

    void emitIPCEvent(bool force = false);
    bool isProtectedApp(const SP<Desktop::View::CWindow>& window) const;
    void onWindowOpen(const SP<Desktop::View::CWindow>& window);

    std::chrono::steady_clock::time_point m_lastNavTime;
    CHyprSignalListener m_destroyWindowListener;
    CHyprSignalListener m_closeWindowListener;
    CHyprSignalListener m_openWindowListener;
    CHyprSignalListener m_moveWindowWorkspaceListener;
    CHyprSignalListener m_openLayerListener;
    CHyprSignalListener m_closeLayerListener;
    CHyprSignalListener m_tickListener;
    CHyprSignalListener m_workspaceActiveListener;
    CHyprSignalListener m_swipeBeginListener;
    CHyprSignalListener m_swipeUpdateListener;
    CHyprSignalListener m_swipeEndListener;
    CHyprSignalListener m_pinchBeginListener;
    CHyprSignalListener m_pinchUpdateListener;
    CHyprSignalListener m_pinchEndListener;
};

inline std::unique_ptr<CCanvas> g_pCanvas;
inline HANDLE                   PHANDLE = nullptr;

inline SP<Config::Values::String> g_pCfgProtectedApps;
inline SP<Config::Values::Int>    g_pCfgDebug;
inline SP<Config::Values::Int>    g_pCfgNavCooldown;
