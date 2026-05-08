#include "shared/config.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace hyprcapture {
namespace {

std::string normalized(std::string_view value) {
    std::string out(value);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        if (c == '_' || c == ' ')
            return '-';
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

CaptureMode parseCaptureMode(std::string_view value, CaptureMode fallback) {
    const auto v = normalized(value);
    if (v == "full" || v == "fullscreen")
        return CaptureMode::Fullscreen;
    if (v == "region" || v == "selection")
        return CaptureMode::Region;
    if (v == "window")
        return CaptureMode::Window;
    return fallback;
}

FullscreenScope parseFullscreenScope(std::string_view value, FullscreenScope fallback) {
    const auto v = normalized(value);
    if (v == "all" || v == "all-monitors")
        return FullscreenScope::All;
    if (v == "current" || v == "current-monitor")
        return FullscreenScope::Current;
    if (v == "per-monitor" || v == "each")
        return FullscreenScope::PerMonitor;
    return fallback;
}

WindowBackground parseWindowBackground(std::string_view value, WindowBackground fallback) {
    const auto v = normalized(value);
    if (v == "white")
        return WindowBackground::White;
    if (v == "black")
        return WindowBackground::Black;
    if (v == "follow-system" || v == "system")
        return WindowBackground::FollowSystem;
    if (v == "real" || v == "real-background")
        return WindowBackground::Real;
    if (v == "transparent" || v == "alpha")
        return WindowBackground::Transparent;
    return fallback;
}

DecorationPolicy parseDecorationPolicy(std::string_view value, DecorationPolicy fallback) {
    const auto v = normalized(value);
    if (v == "keep" || v == "preserve")
        return DecorationPolicy::Keep;
    if (v == "remove" || v == "strip")
        return DecorationPolicy::Remove;
    return fallback;
}

RecordWindowBackend parseRecordWindowBackend(std::string_view value, RecordWindowBackend fallback) {
    const auto v = normalized(value);
    if (v == "compositor" || v == "hyprcapture" || v == "exact")
        return RecordWindowBackend::Compositor;
    if (v == "gsr-visible" || v == "visible-gsr" || v == "gsr" || v == "region")
        return RecordWindowBackend::GsrVisible;
    return fallback;
}

WatermarkPosition parseWatermarkPosition(std::string_view value, WatermarkPosition fallback) {
    const auto v = normalized(value);
    if (v == "up-left" || v == "top-left" || v == "upper-left")
        return WatermarkPosition::UpLeft;
    if (v == "up-middle" || v == "up-center" || v == "top-middle" || v == "top-center" || v == "upper-middle" || v == "upper-center")
        return WatermarkPosition::UpMiddle;
    if (v == "up-right" || v == "top-right" || v == "upper-right")
        return WatermarkPosition::UpRight;
    if (v == "left-middle" || v == "middle-left" || v == "left-center" || v == "center-left")
        return WatermarkPosition::LeftMiddle;
    if (v == "central" || v == "center" || v == "middle" || v == "middle-middle" || v == "center-center")
        return WatermarkPosition::Central;
    if (v == "right-middle" || v == "right-meddle" || v == "middle-right" || v == "right-center" || v == "center-right")
        return WatermarkPosition::RightMiddle;
    if (v == "down-left" || v == "bottom-left" || v == "lower-left")
        return WatermarkPosition::DownLeft;
    if (v == "down-middle" || v == "down-center" || v == "bottom-middle" || v == "bottom-center" || v == "lower-middle" || v == "lower-center")
        return WatermarkPosition::DownMiddle;
    if (v == "down-right" || v == "bottom-right" || v == "lower-right")
        return WatermarkPosition::DownRight;
    return fallback;
}

std::string toString(CaptureMode value) {
    switch (value) {
        case CaptureMode::Fullscreen: return "fullscreen";
        case CaptureMode::Region: return "region";
        case CaptureMode::Window: return "window";
    }
    return "region";
}

std::string toString(FullscreenScope value) {
    switch (value) {
        case FullscreenScope::All: return "all";
        case FullscreenScope::Current: return "current";
        case FullscreenScope::PerMonitor: return "per-monitor";
    }
    return "all";
}

std::string toString(WindowBackground value) {
    switch (value) {
        case WindowBackground::White: return "white";
        case WindowBackground::Black: return "black";
        case WindowBackground::FollowSystem: return "follow-system";
        case WindowBackground::Real: return "real";
        case WindowBackground::Transparent: return "transparent";
    }
    return "follow-system";
}

std::string toString(DecorationPolicy value) {
    switch (value) {
        case DecorationPolicy::Keep: return "keep";
        case DecorationPolicy::Remove: return "remove";
    }
    return "keep";
}

std::string toString(RecordWindowBackend value) {
    switch (value) {
        case RecordWindowBackend::Compositor: return "compositor";
        case RecordWindowBackend::GsrVisible: return "gsr-visible";
    }
    return "compositor";
}

std::string toString(WatermarkPosition value) {
    switch (value) {
        case WatermarkPosition::UpLeft: return "up-left";
        case WatermarkPosition::UpMiddle: return "up-middle";
        case WatermarkPosition::UpRight: return "up-right";
        case WatermarkPosition::LeftMiddle: return "left-middle";
        case WatermarkPosition::Central: return "central";
        case WatermarkPosition::RightMiddle: return "right-middle";
        case WatermarkPosition::DownLeft: return "down-left";
        case WatermarkPosition::DownMiddle: return "down-middle";
        case WatermarkPosition::DownRight: return "down-right";
    }
    return "central";
}

std::filesystem::path expandUserPath(std::string_view path) {
    if (path.empty())
        return {};

    std::string p(path);
    if (p == "~" || p.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home && *home)
            return std::filesystem::path(home) / p.substr(p == "~" ? 1 : 2);
    }
    return std::filesystem::path(p);
}

std::string makeTimestampedFilename(std::string_view filenameTemplate) {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm {};
    localtime_r(&t, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, std::string(filenameTemplate).c_str());
    auto filename = out.str();
    filename = std::filesystem::path(filename).filename().string();
    if (filename.empty() || filename == "." || filename == "..")
        return "Screenshot.png";
    return filename;
}

} // namespace hyprcapture
