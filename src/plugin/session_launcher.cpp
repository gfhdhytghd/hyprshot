#include "plugin/session_launcher.hpp"

#include "plugin/artifact_capture.hpp"
#include "shared/protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace hyprcapture {
namespace {

std::string boolArg(bool value) {
    return value ? "1" : "0";
}

std::string defaultInstalledHelperPath() {
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.local/bin/hyprcapture-ui";
    return {};
}

std::vector<std::string> helperCandidates(const std::string& configured) {
    std::vector<std::string> candidates;
    if (!configured.empty())
        candidates.push_back(configured);

    if (const char* helperEnv = std::getenv("HYPRCAPTURE_HELPER"); helperEnv && *helperEnv)
        candidates.push_back(helperEnv);

    if (auto installed = defaultInstalledHelperPath(); !installed.empty())
        candidates.push_back(std::move(installed));

    candidates.push_back("hyprcapture-ui");

    std::vector<std::string> unique;
    for (auto& candidate : candidates) {
        if (std::find(unique.begin(), unique.end(), candidate) == unique.end())
            unique.push_back(candidate);
    }
    return unique;
}

std::string firstRunnableHelper(const std::string& configured) {
    for (const auto& candidate : helperCandidates(configured)) {
        if (candidate.find('/') == std::string::npos)
            return candidate;
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return configured.empty() ? "hyprcapture-ui" : configured;
}

} // namespace

LaunchResult launchHelper(const LaunchRequest& request) {
    CaptureSession session = captureCompositorArtifacts(request.defaults);
    session.defaults.mode = request.requestedMode;

    const auto sessionJson = encodeSessionJson(session);
    const pid_t pid = fork();
    if (pid < 0)
        return {.success = false, .error = std::string("fork failed: ") + std::strerror(errno)};

    if (pid == 0) {
        std::vector<std::string> args;
        args.push_back(firstRunnableHelper(request.defaults.helper));
        args.push_back("--mode");
        args.push_back(toString(request.requestedMode));
        args.push_back("--fullscreen-scope");
        args.push_back(toString(request.defaults.fullscreenScope));
        args.push_back("--region-scope");
        args.push_back(toString(request.defaults.regionScope));
        args.push_back("--window-background");
        args.push_back(toString(request.defaults.windowBackground));
        args.push_back("--window-border");
        args.push_back(toString(request.defaults.windowBorder));
        args.push_back("--window-shadow");
        args.push_back(toString(request.defaults.windowShadow));
        args.push_back("--save");
        args.push_back(boolArg(request.defaults.save));
        args.push_back("--clipboard");
        args.push_back(boolArg(request.defaults.clipboard));
        args.push_back("--thumbnail");
        args.push_back(boolArg(request.defaults.showThumbnail));
        args.push_back("--include-cursor");
        args.push_back(boolArg(request.defaults.includeCursor));
        args.push_back("--fushion-mode");
        args.push_back(boolArg(request.defaults.fushionMode));
        args.push_back("--save-dir");
        args.push_back(request.defaults.saveDir);
        args.push_back("--filename-template");
        args.push_back(request.defaults.filenameTemplate);
        args.push_back("--thumbnail-timeout-ms");
        args.push_back(std::to_string(request.defaults.thumbnailTimeoutMs));
        args.push_back("--watermark");
        args.push_back(request.defaults.watermark);
        args.push_back("--watermark-position");
        args.push_back(toString(request.defaults.watermarkPosition));
        args.push_back("--watermark-width");
        args.push_back(request.defaults.watermarkWidth);
        args.push_back("--watermark-offset");
        args.push_back(request.defaults.watermarkOffset);
        if (request.quick)
            args.push_back("--quick");
        args.push_back("--session-json");
        args.push_back(sessionJson);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);

        execvpe(argv[0], argv.data(), environ);
        _exit(127);
    }

    return {.success = true};
}

} // namespace hyprcapture
