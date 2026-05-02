#pragma once

#include "shared/config.hpp"

#include <string>

namespace hyprshot {

struct LaunchRequest {
    CaptureDefaults defaults;
    CaptureMode     requestedMode = CaptureMode::Region;
    bool            quick = false;
};

struct LaunchResult {
    bool        success = false;
    std::string error;
};

LaunchResult launchHelper(const LaunchRequest& request);

} // namespace hyprshot
