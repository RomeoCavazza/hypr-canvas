#include "canvas.hpp"

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

extern SDispatchResult dispatchToggle(std::string args);
extern SDispatchResult dispatchEnter(std::string args);
extern SDispatchResult dispatchExit(std::string args);
extern SDispatchResult dispatchReset(std::string args);
extern SDispatchResult dispatchCenter(std::string args);
extern SDispatchResult dispatchHome(std::string args);
extern SDispatchResult dispatchNav(std::string args);
extern SDispatchResult dispatchSwap(std::string args);
extern SDispatchResult dispatchPan(std::string args);
extern SDispatchResult dispatchZoom(std::string args);
extern SDispatchResult dispatchPin(std::string args);
extern SDispatchResult dispatchFloat(std::string args);
extern SDispatchResult dispatchOverview(std::string args);

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:toggle",   dispatchToggle);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:enter",    dispatchEnter);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:exit",     dispatchExit);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:reset",    dispatchReset);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:center",   dispatchCenter);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:home",     dispatchHome);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:nav",      dispatchNav);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:swap",     dispatchSwap);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:pan",      dispatchPan);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:zoom",     dispatchZoom);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:pin",      dispatchPin);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:float",    dispatchFloat);
    HyprlandAPI::addDispatcherV2(PHANDLE, "canvas:overview", dispatchOverview);

    g_pCfgProtectedApps = Config::Values::makeConfigValue<Config::Values::String>("plugin:canvas:protected_apps", "Protected window class substrings", "", {});
    g_pCfgDebug         = Config::Values::makeConfigValue<Config::Values::Int>("plugin:canvas:debug", "Enable debug logging", 0, {});
    g_pCfgNavCooldown   = Config::Values::makeConfigValue<Config::Values::Int>("plugin:canvas:nav_cooldown_ms", "Navigation cooldown in ms", 150, {});

    HyprlandAPI::addConfigValueV2(PHANDLE, g_pCfgProtectedApps);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pCfgDebug);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pCfgNavCooldown);

    g_pCanvas = std::make_unique<CCanvas>();

    return {"hypr-canvas", "VXWM-style infinite canvas — physically moves windows", "Aaron+tco", "1.0.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pCanvas.reset();
}
