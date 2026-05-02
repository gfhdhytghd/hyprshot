#include "shared/config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace hyprshot {
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

RegionScope parseRegionScope(std::string_view value, RegionScope fallback) {
    const auto v = normalized(value);
    if (v == "global" || v == "all")
        return RegionScope::Global;
    if (v == "current" || v == "current-monitor")
        return RegionScope::CurrentMonitor;
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

std::string toString(RegionScope value) {
    switch (value) {
        case RegionScope::Global: return "global";
        case RegionScope::CurrentMonitor: return "current-monitor";
    }
    return "global";
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
    return filename.empty() ? "Screenshot.png" : filename;
}

} // namespace hyprshot
