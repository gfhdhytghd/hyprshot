#pragma once

#include "shared/config.hpp"
#include "shared/protocol.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

extern HANDLE g_pluginHandle;

namespace hyprshot {

CaptureSession captureCompositorArtifacts(const CaptureDefaults& defaults);

} // namespace hyprshot
