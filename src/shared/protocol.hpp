#pragma once

#include "shared/config.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hyprshot {

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct MonitorInfo {
    std::string name;
    Rect        logicalGeometry;
    double      scale = 1.0;
    int         transform = 0;
    std::string artifactPath;
    int         artifactWidth = 0;
    int         artifactHeight = 0;
    bool        artifactTopDown = true;
};

struct WindowInfo {
    std::string address;
    std::string title;
    std::string appClass;
    Rect        visibleGeometry;
    Rect        fullGeometry;
    std::string artifactPath;
    int         artifactWidth = 0;
    int         artifactHeight = 0;
    bool        artifactTopDown = true;
    int         zIndex = 0;
    bool        selectable = true;
};

struct CaptureSession {
    std::string              id;
    CaptureDefaults          defaults;
    std::vector<MonitorInfo> monitors;
    std::vector<WindowInfo>  windows;
};

std::string makeSessionId();
std::string encodeSessionJson(const CaptureSession& session);
std::optional<CaptureSession> decodeSessionJson(const std::string& json);

} // namespace hyprshot
