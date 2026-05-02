#include <any>
#include <string>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include "plugin/session_launcher.hpp"

inline HANDLE g_pluginHandle = nullptr;

namespace {

template <typename T>
T configValue(const std::string& name, T fallback) {
    const auto value = HyprlandAPI::getConfigValue(g_pluginHandle, name);
    if (!value)
        return fallback;

    try {
        return std::any_cast<T>(value->getValue());
    } catch (const std::bad_any_cast&) {
        return fallback;
    }
}

std::string configString(const std::string& suffix, const std::string& fallback) {
    const auto value = HyprlandAPI::getConfigValue(g_pluginHandle, "plugin:hyprshot:" + suffix);
    if (!value)
        return fallback;

    try {
        const auto raw = std::any_cast<Hyprlang::STRING>(value->getValue());
        return raw ? std::string(raw) : fallback;
    } catch (const std::bad_any_cast&) {
        return fallback;
    }
}

std::int64_t configInt(const std::string& suffix, std::int64_t fallback) {
    return configValue<Hyprlang::INT>("plugin:hyprshot:" + suffix, fallback);
}

hyprshot::CaptureDefaults readDefaults() {
    hyprshot::CaptureDefaults defaults;
    defaults.mode = hyprshot::parseCaptureMode(configString("default_mode", hyprshot::toString(defaults.mode)), defaults.mode);
    defaults.fullscreenScope =
        hyprshot::parseFullscreenScope(configString("fullscreen_scope", hyprshot::toString(defaults.fullscreenScope)), defaults.fullscreenScope);
    defaults.regionScope = hyprshot::parseRegionScope(configString("region_scope", hyprshot::toString(defaults.regionScope)), defaults.regionScope);
    defaults.windowBackground =
        hyprshot::parseWindowBackground(configString("window_background", hyprshot::toString(defaults.windowBackground)), defaults.windowBackground);
    defaults.windowBorder = hyprshot::parseDecorationPolicy(configString("window_border", hyprshot::toString(defaults.windowBorder)), defaults.windowBorder);
    defaults.windowShadow = hyprshot::parseDecorationPolicy(configString("window_shadow", hyprshot::toString(defaults.windowShadow)), defaults.windowShadow);
    defaults.save = configInt("save", defaults.save ? 1 : 0) != 0;
    defaults.clipboard = configInt("clipboard", defaults.clipboard ? 1 : 0) != 0;
    defaults.showThumbnail = configInt("show_thumbnail", defaults.showThumbnail ? 1 : 0) != 0;
    defaults.includeCursor = configInt("include_cursor", defaults.includeCursor ? 1 : 0) != 0;
    defaults.saveDir = configString("save_dir", defaults.saveDir);
    defaults.filenameTemplate = configString("filename_template", defaults.filenameTemplate);
    defaults.helper = configString("helper", defaults.helper);
    defaults.thumbnailTimeoutMs = configInt("thumbnail_timeout_ms", defaults.thumbnailTimeoutMs);
    return defaults;
}

SDispatchResult openCapture(const std::string& args, bool quick) {
    auto defaults = readDefaults();
    const auto requestedMode = hyprshot::parseCaptureMode(args, defaults.mode);
    const auto result = hyprshot::launchHelper({.defaults = defaults, .requestedMode = requestedMode, .quick = quick});
    if (!result.success) {
        HyprlandAPI::addNotification(g_pluginHandle, "[hyprshot] " + result.error, CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
        return {.success = false, .error = result.error};
    }
    return {.success = true};
}

SDispatchResult dispatchOpen(const std::string& args) {
    return openCapture(args, false);
}

SDispatchResult dispatchQuick(const std::string& args) {
    return openCapture(args, true);
}

SDispatchResult dispatchCancel(const std::string&) {
    return {.success = true};
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pluginHandle = handle;

    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:default_mode", Hyprlang::STRING{"region"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:fullscreen_scope", Hyprlang::STRING{"all"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:region_scope", Hyprlang::STRING{"global"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:window_background", Hyprlang::STRING{"follow-system"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:window_border", Hyprlang::STRING{"keep"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:window_shadow", Hyprlang::STRING{"keep"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:save", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:clipboard", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:show_thumbnail", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:include_cursor", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:save_dir", Hyprlang::STRING{"~/Pictures/Screenshots"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:filename_template", Hyprlang::STRING{"Screenshot-%Y-%m-%d-%H%M%S.png"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:thumbnail_timeout_ms", Hyprlang::INT{5000});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprshot:helper", Hyprlang::STRING{"hyprshot-ui"});

    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprshot:open", dispatchOpen);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprshot:quick", dispatchQuick);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprshot:cancel", dispatchCancel);
    HyprlandAPI::reloadConfig();

    return {
        .name = "hyprshot",
        .description = "Hyprland-only screenshot overlay",
        .author = "wilf",
        .version = "0.1.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pluginHandle = nullptr;
}
