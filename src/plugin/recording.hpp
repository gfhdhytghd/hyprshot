#pragma once

#include <string>

namespace hyprcapture {

struct LaunchResult;

LaunchResult startRecordingFromRequestFile(const std::string& path);
LaunchResult stopRecording(const std::string& reason = "stopped");
bool isRecordingActive();
void shutdownRecording();

} // namespace hyprcapture
