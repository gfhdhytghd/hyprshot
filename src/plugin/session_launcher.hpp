#pragma once

#include "shared/config.hpp"

#include <string>

namespace hyprcapture {

struct LaunchRequest {
    CaptureDefaults defaults;
    CaptureMode     requestedMode = CaptureMode::Region;
    bool            quick = false;
    bool            record = false;
    bool            recordActive = false;
};

struct LaunchResult {
    bool        success = false;
    std::string error;
};

LaunchResult launchHelper(const LaunchRequest& request);
LaunchResult launchRecordingResultHelper(const CaptureDefaults& defaults, const std::string& outputPath);

} // namespace hyprcapture
