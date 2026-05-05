#pragma once

#include "shared/config.hpp"
#include "shared/protocol.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <string>
#include <string_view>

extern HANDLE g_pluginHandle;

namespace hyprcapture {

CaptureSession captureCompositorArtifacts(const CaptureDefaults& defaults);
std::string writeCompositorSessionJsonFile(const CaptureSession& session, std::string_view json);
void cleanupCompositorArtifacts(const CaptureSession& session);

} // namespace hyprcapture
