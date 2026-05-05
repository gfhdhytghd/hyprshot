#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace hyprcapture {

enum class CaptureMode { Fullscreen, Region, Window };
enum class FullscreenScope { All, Current, PerMonitor };
enum class RegionScope { Global, CurrentMonitor };
enum class WindowBackground { White, Black, FollowSystem, Real, Transparent };
enum class DecorationPolicy { Keep, Remove };

struct CaptureDefaults {
    CaptureMode      mode = CaptureMode::Region;
    FullscreenScope  fullscreenScope = FullscreenScope::All;
    RegionScope      regionScope = RegionScope::Global;
    WindowBackground windowBackground = WindowBackground::FollowSystem;
    DecorationPolicy windowBorder = DecorationPolicy::Keep;
    DecorationPolicy windowShadow = DecorationPolicy::Keep;
    bool             save = true;
    bool             clipboard = true;
    bool             showThumbnail = true;
    bool             includeCursor = false;
    bool             fushionMode = false;
    std::string      saveDir = "~/Pictures/Screenshots";
    std::string      filenameTemplate = "Screenshot-%Y-%m-%d-%H%M%S.png";
    std::string      helper = "hyprcapture-ui";
    std::int64_t     thumbnailTimeoutMs = 5000;
};

CaptureMode parseCaptureMode(std::string_view value, CaptureMode fallback = CaptureMode::Region);
FullscreenScope parseFullscreenScope(std::string_view value, FullscreenScope fallback = FullscreenScope::All);
RegionScope parseRegionScope(std::string_view value, RegionScope fallback = RegionScope::Global);
WindowBackground parseWindowBackground(std::string_view value, WindowBackground fallback = WindowBackground::FollowSystem);
DecorationPolicy parseDecorationPolicy(std::string_view value, DecorationPolicy fallback = DecorationPolicy::Keep);

std::string toString(CaptureMode value);
std::string toString(FullscreenScope value);
std::string toString(RegionScope value);
std::string toString(WindowBackground value);
std::string toString(DecorationPolicy value);

std::filesystem::path expandUserPath(std::string_view path);
std::string makeTimestampedFilename(std::string_view filenameTemplate);

} // namespace hyprcapture
