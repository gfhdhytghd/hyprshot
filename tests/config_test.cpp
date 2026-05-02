#include "shared/config.hpp"
#include "shared/protocol.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>

int main() {
    using namespace hyprshot;

    assert(parseCaptureMode("full") == CaptureMode::Fullscreen);
    assert(parseCaptureMode("selection") == CaptureMode::Region);
    assert(parseCaptureMode("window") == CaptureMode::Window);
    assert(parseCaptureMode("bad", CaptureMode::Window) == CaptureMode::Window);

    assert(parseFullscreenScope("all-monitors") == FullscreenScope::All);
    assert(parseFullscreenScope("current-monitor") == FullscreenScope::Current);
    assert(parseFullscreenScope("per_monitor") == FullscreenScope::PerMonitor);

    assert(parseWindowBackground("follow_system") == WindowBackground::FollowSystem);
    assert(parseWindowBackground("transparent") == WindowBackground::Transparent);
    assert(parseDecorationPolicy("strip") == DecorationPolicy::Remove);

    assert(toString(CaptureMode::Fullscreen) == "fullscreen");
    assert(toString(FullscreenScope::PerMonitor) == "per-monitor");
    assert(toString(WindowBackground::FollowSystem) == "follow-system");

    const auto expanded = expandUserPath("~/Pictures/Screenshots").string();
    assert(expanded.find("Pictures/Screenshots") != std::string::npos);
    assert(makeTimestampedFilename("Screenshot-%Y.png").ends_with(".png"));

    CaptureSession session;
    session.id = "test-session";
    session.defaults.mode = CaptureMode::Window;
    session.defaults.windowBackground = WindowBackground::FollowSystem;
    session.monitors.push_back({.name = "eDP-1", .logicalGeometry = {.x = 0, .y = 0, .width = 1920, .height = 1080}, .scale = 2.0, .transform = 0});
    session.monitors.back().artifactPath = "/tmp/monitor.rgba";
    session.monitors.back().artifactWidth = 3840;
    session.monitors.back().artifactHeight = 2160;
    session.windows.push_back({.address = "0x1",
                               .title = "Title",
                               .appClass = "Class",
                               .visibleGeometry = {.x = 10, .y = 10, .width = 100, .height = 100},
                               .fullGeometry = {.x = -40, .y = 10, .width = 200, .height = 100},
                               .zIndex = 1,
                               .selectable = true});
    session.windows.back().artifactPath = "/tmp/window.rgba";
    session.windows.back().artifactWidth = 200;
    session.windows.back().artifactHeight = 100;
    const auto json = encodeSessionJson(session);
    assert(json.find("\"windowBackground\":\"follow-system\"") != std::string::npos);
    assert(json.find("\"fullGeometry\"") != std::string::npos);
    assert(json.find("\"artifactPath\":\"/tmp/window.rgba\"") != std::string::npos);

    std::cout << "hyprshot config tests passed\n";
    return 0;
}
