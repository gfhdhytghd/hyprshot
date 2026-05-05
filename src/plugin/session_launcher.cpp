#include "plugin/session_launcher.hpp"

#include "plugin/artifact_capture.hpp"
#include "shared/protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
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

bool isTrustedOwner(uid_t uid) {
    return uid == 0 || uid == geteuid();
}

bool hasWritableGroupOrOther(mode_t mode) {
    return (mode & 0022) != 0;
}

bool parentChainTrusted(const std::filesystem::path& path) {
    for (auto current = path.parent_path(); !current.empty(); current = current.parent_path()) {
        struct stat st {};
        const auto  native = current.string();
        if (stat(native.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || !isTrustedOwner(st.st_uid) || hasWritableGroupOrOther(st.st_mode))
            return false;
        if (current == current.root_path())
            break;
    }
    return true;
}

std::optional<std::string> trustedExecutablePath(const std::string& candidate) {
    if (candidate.empty())
        return std::nullopt;

    std::error_code ec;
    auto            path = std::filesystem::path(candidate);
    if (!path.is_absolute())
        return std::nullopt;

    path = std::filesystem::weakly_canonical(path, ec);
    if (ec || !path.is_absolute() || !parentChainTrusted(path))
        return std::nullopt;

    const auto native = path.string();
    struct stat st {};
    if (stat(native.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !isTrustedOwner(st.st_uid) || hasWritableGroupOrOther(st.st_mode))
        return std::nullopt;
    if (access(native.c_str(), X_OK) != 0)
        return std::nullopt;
    return native;
}

std::vector<std::string> helperCandidates(const std::string& configured) {
    std::vector<std::string> candidates;
    if (!configured.empty())
        candidates.push_back(configured);

    if (const char* helperEnv = std::getenv("HYPRCAPTURE_HELPER"); helperEnv && *helperEnv)
        candidates.push_back(helperEnv);

    if (auto installed = defaultInstalledHelperPath(); !installed.empty())
        candidates.push_back(std::move(installed));

    candidates.push_back("/usr/local/bin/hyprcapture-ui");
    candidates.push_back("/usr/bin/hyprcapture-ui");

    std::vector<std::string> unique;
    for (auto& candidate : candidates) {
        if (std::find(unique.begin(), unique.end(), candidate) == unique.end())
            unique.push_back(candidate);
    }
    return unique;
}

std::optional<std::string> firstRunnableHelper(const std::string& configured) {
    for (const auto& candidate : helperCandidates(configured)) {
        if (const auto trusted = trustedExecutablePath(candidate))
            return trusted;
    }
    return std::nullopt;
}

bool allowEnvironmentName(std::string_view name) {
    if (name == "HOME" || name == "USER" || name == "LOGNAME" || name == "LANG" || name == "XDG_RUNTIME_DIR" || name == "XDG_CURRENT_DESKTOP" ||
        name == "XDG_SESSION_TYPE" || name == "WAYLAND_DISPLAY" || name == "DISPLAY" || name == "DBUS_SESSION_BUS_ADDRESS" ||
        name == "QT_QPA_PLATFORM" || name == "QT_SCALE_FACTOR" || name == "QT_AUTO_SCREEN_SCALE_FACTOR" || name == "QT_ENABLE_HIGHDPI_SCALING" ||
        name == "HYPRCAPTURE_TIMING" || name == "HYPRCAPTURE_TIMING_FILE")
        return true;
    return name.starts_with("LC_");
}

std::vector<std::string> childEnvironment() {
    std::vector<std::string> env;
    env.push_back("PATH=/usr/local/bin:/usr/bin:/bin");
    for (char** item = environ; item && *item; ++item) {
        const std::string entry(*item);
        const auto        separator = entry.find('=');
        if (separator == std::string::npos)
            continue;
        if (allowEnvironmentName(std::string_view(entry).substr(0, separator)))
            env.push_back(entry);
    }
    return env;
}

} // namespace

LaunchResult launchHelper(const LaunchRequest& request) {
    const auto helper = firstRunnableHelper(request.defaults.helper);
    if (!helper)
        return {.success = false, .error = "no trusted hyprcapture-ui helper found"};

    CaptureSession session = captureCompositorArtifacts(request.defaults);
    session.defaults.mode = request.requestedMode;

    const auto sessionJson = encodeSessionJson(session);
    const pid_t pid = fork();
    if (pid < 0)
        return {.success = false, .error = std::string("fork failed: ") + std::strerror(errno)};

    if (pid == 0) {
        std::vector<std::string> args;
        args.push_back(*helper);
        args.push_back("--mode");
        args.push_back(toString(request.requestedMode));
        args.push_back("--fullscreen-scope");
        args.push_back(toString(request.defaults.fullscreenScope));
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

        auto childEnv = childEnvironment();
        std::vector<char*> envp;
        envp.reserve(childEnv.size() + 1);
        for (auto& env : childEnv)
            envp.push_back(env.data());
        envp.push_back(nullptr);

        execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }

    return {.success = true};
}

} // namespace hyprcapture
