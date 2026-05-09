#include <chrono>
#include <deque>
#include <string>
#include <typeindex>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/Types.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/rule/Engine.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRule.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRuleEffectContainer.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include "plugin/artifact_capture.hpp"
#include "plugin/recording.hpp"
#include "plugin/session_launcher.hpp"

inline HANDLE g_pluginHandle = nullptr;

namespace {

constexpr const char* kOverlayLayerRuleName = "hyprcapture-ui-no-compositor-anim";
constexpr auto        kMinDispatchInterval = std::chrono::milliseconds(750);

std::chrono::steady_clock::time_point g_lastCaptureDispatch {};
std::chrono::steady_clock::time_point g_lastQuickRejectNotification {};
std::deque<std::string>               g_configValueNames;

std::string configName(const std::string& suffix) {
    return "plugin:hyprcapture:" + suffix;
}

const Config::SConfigOptionReply configOption(const std::string& name) {
    if (!g_pluginHandle)
        return {};
    return Config::mgr()->getConfigValue(name);
}

std::string configString(const std::string& suffix, const std::string& fallback) {
    const auto value = configOption(configName(suffix));
    if (!value.dataptr || !value.type)
        return fallback;

    const auto type = std::type_index(*value.type);
    if (type == typeid(Config::STRING))
        return **reinterpret_cast<Config::STRING* const*>(value.dataptr);

    if (type == typeid(Hyprlang::STRING)) {
        const auto raw = *reinterpret_cast<Hyprlang::STRING const*>(value.dataptr);
        return raw ? std::string(raw) : fallback;
    }

    return fallback;
}

std::int64_t configInt(const std::string& suffix, std::int64_t fallback) {
    const auto value = configOption(configName(suffix));
    if (!value.dataptr || !value.type)
        return fallback;

    const auto type = std::type_index(*value.type);
    if (type == typeid(Config::INTEGER))
        return **reinterpret_cast<Config::INTEGER* const*>(value.dataptr);

    if (type == typeid(Config::BOOL))
        return **reinterpret_cast<Config::BOOL* const*>(value.dataptr) ? 1 : 0;

    return fallback;
}

bool configBool(const std::string& suffix, bool fallback) {
    const auto value = configOption(configName(suffix));
    if (!value.dataptr || !value.type)
        return fallback;

    const auto type = std::type_index(*value.type);
    if (type == typeid(Config::BOOL))
        return **reinterpret_cast<Config::BOOL* const*>(value.dataptr);

    if (type == typeid(Config::INTEGER))
        return **reinterpret_cast<Config::INTEGER* const*>(value.dataptr) != 0;

    return fallback;
}

void addStringConfig(const char* suffix, const char* description, const char* fallback) {
    const auto& name = g_configValueNames.emplace_back(configName(suffix));
    HyprlandAPI::addConfigValueV2(g_pluginHandle, makeShared<Config::Values::CStringValue>(name.c_str(), description, fallback));
}

void addIntConfig(const char* suffix, const char* description, std::int64_t fallback) {
    const auto& name = g_configValueNames.emplace_back(configName(suffix));
    HyprlandAPI::addConfigValueV2(g_pluginHandle, makeShared<Config::Values::CIntValue>(name.c_str(), description, fallback));
}

void addBoolConfig(const char* suffix, const char* description, bool fallback) {
    const auto& name = g_configValueNames.emplace_back(configName(suffix));
    HyprlandAPI::addConfigValueV2(g_pluginHandle, makeShared<Config::Values::CBoolValue>(name.c_str(), description, fallback));
}

void registerConfigValues() {
    g_configValueNames.clear();

    addStringConfig("default_mode", "Default HyprCapture mode", "region");
    addStringConfig("fullscreen_scope", "Fullscreen capture scope", "all");
    addStringConfig("window_background", "Window capture background mode", "follow-system");
    addStringConfig("window_border", "Window capture border policy", "keep");
    addStringConfig("window_shadow", "Window capture shadow policy", "keep");
    addBoolConfig("save", "Save captures to disk", true);
    addBoolConfig("clipboard", "Copy captures to the clipboard", true);
    addBoolConfig("show_thumbnail", "Show a result thumbnail after capture", true);
    addBoolConfig("include_cursor", "Include the cursor in captures", false);
    addBoolConfig("allow_quick", "Enable no-confirmation quick capture dispatchers", false);
    addBoolConfig("fushion_mode", "Fuse region and window interactions in one overlay", false);
    addStringConfig("save_dir", "Capture output directory", "~/Pictures/Screenshots");
    addStringConfig("filename_template", "Screenshot filename strftime template", "Screenshot-%Y-%m-%d-%H%M%S.png");
    addStringConfig("record_filename_template", "Recording filename strftime template", "Recording-%Y-%m-%d-%H%M%S.mp4");
    addStringConfig("record_format", "Default recording container", "mp4");
    addStringConfig("record_transparent_format", "Default transparent recording container", "webm");
    addIntConfig("record_fps", "Recording frame rate", 30);
    addStringConfig("record_fps_options", "Recording frame rate choices", "15 24 30 60");
    addIntConfig("record_window_fps_limit", "Compositor window recording FPS cap", 12);
    addIntConfig("record_window_real_bg_fps_limit", "Real-background window recording FPS cap", 8);
    addStringConfig("record_codec", "Default recording codec", "libx264");
    addStringConfig("record_transparent_codec", "Default transparent recording codec", "auto");
    addStringConfig("record_preset", "FFmpeg preset", "veryfast");
    addStringConfig("record_gsr_flags", "Extra gpu-screen-recorder flags", "");
    addStringConfig("record_window_backend", "Window recording backend", "compositor");
    addIntConfig("record_max_seconds", "Optional automatic recording stop in seconds", 0);
    addIntConfig("thumbnail_timeout_ms", "Thumbnail auto-close timeout in milliseconds", 5000);
    addStringConfig("helper", "Optional helper executable override", "");
    addStringConfig("watermark", "Watermark path or built-in name", "");
    addStringConfig("watermark_position", "Watermark position", "central");
    addStringConfig("watermark_width", "Watermark width", "20%");
    addStringConfig("watermark_offset", "Watermark offset", "0 0");
    addBoolConfig("timing", "Enable HyprCapture timing traces", false);
    addStringConfig("timing_file", "Private timing trace output file", "");
}

SDispatchResult dispatchResult(const hyprcapture::LaunchResult& result) {
    return result.success ? SDispatchResult{.success = true} : SDispatchResult{.success = false, .error = result.error};
}

hyprcapture::CaptureDefaults readDefaults() {
    hyprcapture::CaptureDefaults defaults;
    defaults.mode = hyprcapture::parseCaptureMode(configString("default_mode", hyprcapture::toString(defaults.mode)), defaults.mode);
    defaults.fullscreenScope =
        hyprcapture::parseFullscreenScope(configString("fullscreen_scope", hyprcapture::toString(defaults.fullscreenScope)), defaults.fullscreenScope);
    defaults.windowBackground =
        hyprcapture::parseWindowBackground(configString("window_background", hyprcapture::toString(defaults.windowBackground)), defaults.windowBackground);
    defaults.windowBorder = hyprcapture::parseDecorationPolicy(configString("window_border", hyprcapture::toString(defaults.windowBorder)), defaults.windowBorder);
    defaults.windowShadow = hyprcapture::parseDecorationPolicy(configString("window_shadow", hyprcapture::toString(defaults.windowShadow)), defaults.windowShadow);
    defaults.save = configBool("save", defaults.save);
    defaults.clipboard = configBool("clipboard", defaults.clipboard);
    defaults.showThumbnail = configBool("show_thumbnail", defaults.showThumbnail);
    defaults.includeCursor = configBool("include_cursor", defaults.includeCursor);
    defaults.allowQuick = configBool("allow_quick", defaults.allowQuick);
    defaults.fushionMode = configBool("fushion_mode", defaults.fushionMode);
    defaults.saveDir = configString("save_dir", defaults.saveDir);
    defaults.filenameTemplate = configString("filename_template", defaults.filenameTemplate);
    defaults.helper = configString("helper", defaults.helper);
    defaults.recordFilenameTemplate = configString("record_filename_template", defaults.recordFilenameTemplate);
    defaults.recordFormat = configString("record_format", defaults.recordFormat);
    defaults.recordTransparentFormat = configString("record_transparent_format", defaults.recordTransparentFormat);
    defaults.recordCodec = configString("record_codec", defaults.recordCodec);
    defaults.recordTransparentCodec = configString("record_transparent_codec", defaults.recordTransparentCodec);
    defaults.recordPreset = configString("record_preset", defaults.recordPreset);
    defaults.recordGsrFlags = configString("record_gsr_flags", defaults.recordGsrFlags);
    defaults.recordWindowBackend =
        hyprcapture::parseRecordWindowBackend(configString("record_window_backend", hyprcapture::toString(defaults.recordWindowBackend)), defaults.recordWindowBackend);
    defaults.recordFps = configInt("record_fps", defaults.recordFps);
    defaults.recordFpsOptions = configString("record_fps_options", defaults.recordFpsOptions);
    defaults.recordWindowFpsLimit = configInt("record_window_fps_limit", defaults.recordWindowFpsLimit);
    defaults.recordWindowRealBgFpsLimit = configInt("record_window_real_bg_fps_limit", defaults.recordWindowRealBgFpsLimit);
    defaults.recordMaxSeconds = configInt("record_max_seconds", defaults.recordMaxSeconds);
    defaults.thumbnailTimeoutMs = configInt("thumbnail_timeout_ms", defaults.thumbnailTimeoutMs);
    defaults.watermark = configString("watermark", defaults.watermark);
    defaults.watermarkPosition =
        hyprcapture::parseWatermarkPosition(configString("watermark_position", hyprcapture::toString(defaults.watermarkPosition)), defaults.watermarkPosition);
    defaults.watermarkWidth = configString("watermark_width", defaults.watermarkWidth);
    defaults.watermarkOffset = configString("watermark_offset", defaults.watermarkOffset);
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

SDispatchResult openCapture(const std::string& args, bool quick, bool record) {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastCaptureDispatch.time_since_epoch().count() != 0 && now - g_lastCaptureDispatch < kMinDispatchInterval) {
        const std::string error = "capture dispatch rate-limited";
        HyprlandAPI::addNotification(g_pluginHandle, "[hyprcapture] " + error, CHyprColor(1.0, 0.2, 0.2, 1.0), 2500);
        return {.success = false, .error = error};
    }

    auto defaults = readDefaults();
    if (quick && !defaults.allowQuick) {
        const std::string error = "hyprcapture:quick disabled; set plugin:hyprcapture:allow_quick = 1 to enable no-confirmation capture";
        if (g_lastQuickRejectNotification.time_since_epoch().count() == 0 || now - g_lastQuickRejectNotification >= kMinDispatchInterval) {
            g_lastQuickRejectNotification = now;
            HyprlandAPI::addNotification(g_pluginHandle, "[hyprcapture] " + error, CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
        }
        return {.success = false, .error = error};
    }

    installOverlayLayerRule();

    const auto requestedMode = hyprcapture::parseCaptureMode(args, defaults.mode);
    const auto result = hyprcapture::launchHelper(
        {.defaults = defaults, .requestedMode = requestedMode, .quick = quick, .record = record, .recordActive = hyprcapture::isRecordingActive()});
    if (!result.success) {
        HyprlandAPI::addNotification(g_pluginHandle, "[hyprcapture] " + result.error, CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
        return {.success = false, .error = result.error};
    }
    g_lastCaptureDispatch = now;
    return {.success = true};
}

SDispatchResult dispatchOpen(const std::string& args) {
    return openCapture(args, false, false);
}

SDispatchResult dispatchQuick(const std::string& args) {
    return openCapture(args, true, false);
}

SDispatchResult dispatchRecord(const std::string& args) {
    if (hyprcapture::isRecordingActive())
        return {.success = false, .error = "recording already active"};
    return openCapture(args, false, true);
}

SDispatchResult dispatchRecordToggle(const std::string& args) {
    if (hyprcapture::isRecordingActive())
        return dispatchResult(hyprcapture::stopRecording("stopped"));
    return dispatchRecord(args);
}

SDispatchResult dispatchRecordStop(const std::string&) {
    return dispatchResult(hyprcapture::stopRecording("stopped"));
}

SDispatchResult dispatchRecordStart(const std::string& args) {
    const auto result = hyprcapture::startRecordingFromRequestFile(args);
    if (!result.success)
        HyprlandAPI::addNotification(g_pluginHandle, "[hyprcapture] " + result.error, CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
    return dispatchResult(result);
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

    registerConfigValues();

    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:open", dispatchOpen);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:quick", dispatchQuick);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:record", dispatchRecord);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:record-toggle", dispatchRecordToggle);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:record-stop", dispatchRecordStop);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:record-start", dispatchRecordStart);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hyprcapture:cancel", dispatchCancel);
    HyprlandAPI::reloadConfig();
    installOverlayLayerRule();

    return {
        .name = "HyprCapture",
        .description = "Hyprland-only screenshot overlay",
        .author = "wilf",
        .version = "0.2.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    hyprcapture::shutdownRecording();
    hyprcapture::shutdownArtifactCapture();
    Desktop::Rule::ruleEngine()->unregisterRule(kOverlayLayerRuleName);
    g_pluginHandle = nullptr;
}
