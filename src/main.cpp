#include "canvas.hpp"

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// Dispatcher declarations (defined in canvas.cpp)
extern SDispatchResult dispatchToggle(std::string args);
extern SDispatchResult dispatchEnter(std::string args);
extern SDispatchResult dispatchExit(std::string args);
extern SDispatchResult dispatchReset(std::string args);
extern SDispatchResult dispatchHome(std::string args);
extern SDispatchResult dispatchCenter(std::string args);
extern SDispatchResult dispatchNav(std::string args);
extern SDispatchResult dispatchPan(std::string args);
extern SDispatchResult dispatchZoom(std::string args);
extern SDispatchResult dispatchPin(std::string args);
extern SDispatchResult dispatchFloat(std::string args);
extern SDispatchResult dispatchOverview(std::string args);
extern SDispatchResult dispatchBookmark(std::string args);

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:toggle",   dispatchToggle);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:enter",    dispatchEnter);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:exit",     dispatchExit);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:reset",    dispatchReset);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:home",     dispatchHome);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:center",   dispatchCenter);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:nav",      dispatchNav);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:pan",      dispatchPan);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:zoom",     dispatchZoom);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:pin",      dispatchPin);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:float",    dispatchFloat);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:overview", dispatchOverview);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:bookmark", dispatchBookmark);

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:canvas:protected_apps", Hyprlang::STRING{(char*)""});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:canvas:debug", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:canvas:nav_cooldown_ms", Hyprlang::INT{150});

    g_pCanvas = std::make_unique<CCanvas>();

    return {"hypr-canvas", "VXWM-style infinite canvas — physically moves windows", "Aaron+tco", "0.4"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pCanvas.reset();
}
