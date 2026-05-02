#include "plugin/session_launcher.hpp"

#include "shared/protocol.hpp"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace hyprshot {
namespace {

std::string boolArg(bool value) {
    return value ? "1" : "0";
}

} // namespace

LaunchResult launchHelper(const LaunchRequest& request) {
    CaptureSession session;
    session.id = makeSessionId();
    session.defaults = request.defaults;
    session.defaults.mode = request.requestedMode;

    const auto sessionJson = encodeSessionJson(session);
    const pid_t pid = fork();
    if (pid < 0)
        return {.success = false, .error = std::string("fork failed: ") + std::strerror(errno)};

    if (pid == 0) {
        std::vector<std::string> args;
        args.push_back(request.defaults.helper.empty() ? "hyprshot-ui" : request.defaults.helper);
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
        args.push_back("--save-dir");
        args.push_back(request.defaults.saveDir);
        args.push_back("--filename-template");
        args.push_back(request.defaults.filenameTemplate);
        args.push_back("--thumbnail-timeout-ms");
        args.push_back(std::to_string(request.defaults.thumbnailTimeoutMs));
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

} // namespace hyprshot
