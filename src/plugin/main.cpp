#include <any>
#include <string>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/rule/Engine.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRule.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRuleEffectContainer.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include "plugin/session_launcher.hpp"

inline HANDLE g_pluginHandle = nullptr;

namespace {

constexpr const char* kOverlayLayerRuleName = "hyprcapture-ui-no-compositor-anim";

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
    const auto value = HyprlandAPI::getConfigValue(g_pluginHandle, "plugin:hyprcapture:" + suffix);
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
    return configValue<Hyprlang::INT>("plugin:hyprcapture:" + suffix, fallback);
}

hyprcapture::CaptureDefaults readDefaults() {
    hyprcapture::CaptureDefaults defaults;
    defaults.mode = hyprcapture::parseCaptureMode(configString("default_mode", hyprcapture::toString(defaults.mode)), defaults.mode);
    defaults.fullscreenScope =
        hyprcapture::parseFullscreenScope(configString("fullscreen_scope", hyprcapture::toString(defaults.fullscreenScope)), defaults.fullscreenScope);
    defaults.regionScope = hyprcapture::parseRegionScope(configString("region_scope", hyprcapture::toString(defaults.regionScope)), defaults.regionScope);
    defaults.windowBackground =
        hyprcapture::parseWindowBackground(configString("window_background", hyprcapture::toString(defaults.windowBackground)), defaults.windowBackground);
    defaults.windowBorder = hyprcapture::parseDecorationPolicy(configString("window_border", hyprcapture::toString(defaults.windowBorder)), defaults.windowBorder);
    defaults.windowShadow = hyprcapture::parseDecorationPolicy(configString("window_shadow", hyprcapture::toString(defaults.windowShadow)), defaults.windowShadow);
    defaults.save = configInt("save", defaults.save ? 1 : 0) != 0;
    defaults.clipboard = configInt("clipboard", defaults.clipboard ? 1 : 0) != 0;
    defaults.showThumbnail = configInt("show_thumbnail", defaults.showThumbnail ? 1 : 0) != 0;
    defaults.includeCursor = configInt("include_cursor", defaults.includeCursor ? 1 : 0) != 0;
    defaults.fushionMode = configInt("fushion_mode", defaults.fushionMode ? 1 : 0) != 0;
    defaults.saveDir = configString("save_dir", defaults.saveDir);
    defaults.filenameTemplate = configString("filename_template", defaults.filenameTemplate);
    defaults.helper = configString("helper", defaults.helper);
    defaults.thumbnailTimeoutMs = configInt("thumbnail_timeout_ms", defaults.thumbnailTimeoutMs);
    return defaults;
}

void installOverlayLayerRule() {
    using namespace Desktop::Rule;

    ruleEngine()->unregisterRule(kOverlayLayerRuleName);

    SP<CLayerRule> rule = makeShared<CLayerRule>(kOverlayLayerRuleName);
    rule->registerMatch(RULE_PROP_NAMESPACE, "^hyprcapture-ui$");
    rule->addEffect(LAYER_RULE_EFFECT_NO_ANIM, "1");
    ruleEngine()->registerRule(SP<IRule>{rule});
}

SDispatchResult openCapture(const std::string& args, bool quick) {
    installOverlayLayerRule();

    auto defaults = readDefaults();
    const auto requestedMode = hyprcapture::parseCaptureMode(args, defaults.mode);
    const auto result = hyprcapture::launchHelper({.defaults = defaults, .requestedMode = requestedMode, .quick = quick});
    if (!result.success) {
        HyprlandAPI::addNotification(g_pluginHandle, "[hyprcapture] " + result.error, CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
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

    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:default_mode", Hyprlang::STRING{"region"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:fullscreen_scope", Hyprlang::STRING{"all"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:region_scope", Hyprlang::STRING{"global"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:window_background", Hyprlang::STRING{"follow-system"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:window_border", Hyprlang::STRING{"keep"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:window_shadow", Hyprlang::STRING{"keep"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:save", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:clipboard", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:show_thumbnail", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:include_cursor", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:fushion_mode", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:save_dir", Hyprlang::STRING{"~/Pictures/Screenshots"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:filename_template", Hyprlang::STRING{"Screenshot-%Y-%m-%d-%H%M%S.png"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:thumbnail_timeout_ms", Hyprlang::INT{5000});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hyprcapture:helper", Hyprlang::STRING{"hyprcapture-ui"});

    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:open", dispatchOpen);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:quick", dispatchQuick);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:cancel", dispatchCancel);
    HyprlandAPI::reloadConfig();
    installOverlayLayerRule();

    return {
        .name = "HyprCapture",
        .description = "Hyprland-only screenshot overlay",
        .author = "wilf",
        .version = "0.1.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    Desktop::Rule::ruleEngine()->unregisterRule(kOverlayLayerRuleName);
    g_pluginHandle = nullptr;
}
