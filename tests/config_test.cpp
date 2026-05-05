#include "shared/config.hpp"
#include "shared/protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (condition)
        return;

    std::cerr << "config test failed: " << message << '\n';
    std::exit(1);
}

} // namespace

int main() {
    using namespace hyprcapture;

    require(parseCaptureMode("full") == CaptureMode::Fullscreen, "full mode parse");
    require(parseCaptureMode("selection") == CaptureMode::Region, "selection mode parse");
    require(parseCaptureMode("window") == CaptureMode::Window, "window mode parse");
    require(parseCaptureMode("bad", CaptureMode::Window) == CaptureMode::Window, "mode fallback");

    require(parseFullscreenScope("all-monitors") == FullscreenScope::All, "all monitor scope parse");
    require(parseFullscreenScope("current-monitor") == FullscreenScope::Current, "current monitor scope parse");
    require(parseFullscreenScope("per_monitor") == FullscreenScope::PerMonitor, "per monitor scope parse");

    require(parseWindowBackground("follow_system") == WindowBackground::FollowSystem, "follow system background parse");
    require(parseWindowBackground("transparent") == WindowBackground::Transparent, "transparent background parse");
    require(parseDecorationPolicy("strip") == DecorationPolicy::Remove, "decoration policy parse");
    require(parseWatermarkPosition("central") == WatermarkPosition::Central, "central watermark position parse");
    require(parseWatermarkPosition("right-meddle") == WatermarkPosition::RightMiddle, "legacy watermark position alias");
    require(parseWatermarkPosition("top_center") == WatermarkPosition::UpMiddle, "top center watermark position parse");
    require(parseWatermarkPosition("bad", WatermarkPosition::DownRight) == WatermarkPosition::DownRight, "watermark position fallback");

    require(toString(CaptureMode::Fullscreen) == "fullscreen", "fullscreen stringify");
    require(toString(FullscreenScope::PerMonitor) == "per-monitor", "per monitor stringify");
    require(toString(WindowBackground::FollowSystem) == "follow-system", "follow system stringify");
    require(toString(WatermarkPosition::DownMiddle) == "down-middle", "down middle stringify");

    const auto expanded = expandUserPath("~/Pictures/Screenshots").string();
    require(expanded.find("Pictures/Screenshots") != std::string::npos, "home path expansion");
    require(makeTimestampedFilename("Screenshot-%Y.png").ends_with(".png"), "timestamp filename suffix");
    require(makeTimestampedFilename("../escape.png") == "escape.png", "filename basename clamp");
    require(makeTimestampedFilename("..") == "Screenshot.png", "invalid filename fallback");

    CaptureSession session;
    session.id = "test-session";
    session.defaults.mode = CaptureMode::Window;
    session.defaults.fushionMode = true;
    session.defaults.windowBackground = WindowBackground::FollowSystem;
    session.defaults.watermark = "activate-linux";
    session.defaults.watermarkPosition = WatermarkPosition::RightMiddle;
    session.defaults.watermarkWidth = "18%";
    session.defaults.watermarkOffset = "-2% 24px";
    session.cursorPosition = Point{.x = 120, .y = 240};
    session.monitors.push_back({.name = "eDP-1", .logicalGeometry = {.x = 0, .y = 0, .width = 1920, .height = 1080}, .scale = 2.0, .transform = 0});
    session.monitors.back().artifactPath = "/tmp/monitor.rgba";
    session.monitors.back().artifactWidth = 3840;
    session.monitors.back().artifactHeight = 2160;
    session.windows.push_back({.address = "0x1",
                               .title = "Title",
                               .appClass = "Class",
                               .visibleGeometry = {.x = 10, .y = 10, .width = 100, .height = 100},
                               .fullGeometry = {.x = -40, .y = 10, .width = 200, .height = 100},
                               .rounding = 12,
                               .roundingPower = 2.5,
                               .borderSize = 2,
                               .zIndex = 1,
                               .selectable = true});
    session.windows.back().artifactPath = "/tmp/window.rgba";
    session.windows.back().artifactWidth = 200;
    session.windows.back().artifactHeight = 100;
    session.windows.back().realBackgroundPath = "/tmp/window-real.rgba";
    session.windows.back().realBackgroundWidth = 200;
    session.windows.back().realBackgroundHeight = 100;
    session.windows.back().title = std::string("Title") + '\x01';
    const auto json = encodeSessionJson(session);
    require(json.find("\"fushionMode\":true") != std::string::npos, "fushion mode json");
    require(json.find("\"windowBackground\":\"follow-system\"") != std::string::npos, "window background json");
    require(json.find("\"watermark\":\"activate-linux\"") != std::string::npos, "watermark json");
    require(json.find("\"watermarkPosition\":\"right-middle\"") != std::string::npos, "watermark position json");
    require(json.find("\"watermarkWidth\":\"18%\"") != std::string::npos, "watermark width json");
    require(json.find("\"watermarkOffset\":\"-2% 24px\"") != std::string::npos, "watermark offset json");
    require(json.find("\"cursorPosition\":{\"x\":120,\"y\":240}") != std::string::npos, "cursor position json");
    require(json.find("\"fullGeometry\"") != std::string::npos, "full geometry json");
    require(json.find("\"rounding\":12") != std::string::npos, "rounding json");
    require(json.find("\"roundingPower\":2.5") != std::string::npos, "rounding power json");
    require(json.find("\"borderSize\":2") != std::string::npos, "border size json");
    require(json.find("\"artifactPath\":\"/tmp/window.rgba\"") != std::string::npos, "artifact path json");
    require(json.find("\"artifactTopDown\":true") != std::string::npos, "artifact orientation json");
    require(json.find("\"realBackgroundPath\":\"/tmp/window-real.rgba\"") != std::string::npos, "real background path json");
    require(json.find("\"realBackgroundWidth\":200") != std::string::npos, "real background width json");
    require(json.find("Title\\u0001") != std::string::npos, "control byte json escaping");

    std::cout << "hyprcapture config tests passed\n";
    return 0;
}
