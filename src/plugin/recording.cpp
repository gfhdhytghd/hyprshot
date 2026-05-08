#include "plugin/recording.hpp"

#include "plugin/artifact_capture.hpp"
#include "plugin/session_launcher.hpp"
#include "shared/config.hpp"
#include "shared/protocol.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <memory>
#include <optional>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;
extern HANDLE g_pluginHandle;

namespace hyprcapture {
namespace {

constexpr std::size_t MAX_RECORD_REQUEST_BYTES = 64 * 1024;
constexpr int         MAX_FRAME_QUEUE = 2;
constexpr int         MAX_CONSECUTIVE_FRAME_FAILURES = 30;
constexpr int         RGBA_BYTES_PER_PIXEL = 4;

struct PipeFds {
    int read = -1;
    int write = -1;
};

void closeFd(int& fd) {
    if (fd >= 0)
        close(fd);
    fd = -1;
}

bool setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

std::optional<PipeFds> makePipe() {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
        return std::nullopt;

    PipeFds pipe{.read = fds[0], .write = fds[1]};
    if (!setCloseOnExec(pipe.read) || !setCloseOnExec(pipe.write)) {
        closeFd(pipe.read);
        closeFd(pipe.write);
        return std::nullopt;
    }
    return pipe;
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
    auto            path = std::filesystem::weakly_canonical(std::filesystem::path(candidate), ec);
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

std::optional<std::string> trustedFfmpegPath() {
    for (const auto& candidate : {"/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg", "/bin/ffmpeg"}) {
        if (const auto trusted = trustedExecutablePath(candidate))
            return trusted;
    }
    return std::nullopt;
}

std::optional<std::string> trustedGpuScreenRecorderPath() {
    for (const auto& candidate : {"/usr/local/bin/gpu-screen-recorder", "/usr/bin/gpu-screen-recorder", "/bin/gpu-screen-recorder"}) {
        if (const auto trusted = trustedExecutablePath(candidate))
            return trusted;
    }
    return std::nullopt;
}

std::optional<std::string> findVaapiRenderDevice() {
    for (int minor = 128; minor <= 143; ++minor) {
        const std::string candidate = "/dev/dri/renderD" + std::to_string(minor);
        struct stat       st {};
        if (stat(candidate.c_str(), &st) == 0 && S_ISCHR(st.st_mode) && access(candidate.c_str(), R_OK | W_OK) == 0)
            return candidate;
    }
    return std::nullopt;
}

bool hasSuffix(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool isVaapiCodec(std::string_view codec) {
    return hasSuffix(codec, "_vaapi");
}

bool isNvencCodec(std::string_view codec) {
    return hasSuffix(codec, "_nvenc");
}

bool isHardwareCodec(std::string_view codec) {
    return isVaapiCodec(codec) || isNvencCodec(codec);
}

bool allowEnvironmentName(std::string_view name) {
    if (name == "HOME" || name == "USER" || name == "LOGNAME" || name == "LANG" || name == "XDG_RUNTIME_DIR" || name == "XDG_CURRENT_DESKTOP" ||
        name == "XDG_SESSION_TYPE" || name == "WAYLAND_DISPLAY" || name == "DISPLAY" || name == "DBUS_SESSION_BUS_ADDRESS" ||
        name == "HYPRLAND_INSTANCE_SIGNATURE")
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

std::filesystem::path privateRuntimeRoot() {
    const auto rootName = "hyprcapture-" + std::to_string(static_cast<unsigned long long>(geteuid()));
    for (const auto& base : {std::filesystem::path{"/dev/shm"}, std::filesystem::temp_directory_path()}) {
        const auto root = base / rootName;
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(root, ec);
        if (ec)
            continue;

        struct stat st {};
        const auto  native = canonical.string();
        if (stat(native.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0777) == 0700)
            return canonical;
    }
    return {};
}

bool pathIsInPrivateRuntimeRoot(const std::filesystem::path& path) {
    std::error_code ec;
    const auto      root = privateRuntimeRoot();
    if (root.empty())
        return false;

    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        return false;

    const auto rootNative = root.native();
    const auto pathNative = canonical.native();
    return pathNative == rootNative || pathNative.starts_with(rootNative + "/");
}

std::optional<std::string> readPrivateRequestFile(const std::string& rawPath) {
    const std::filesystem::path path(rawPath);
    if (rawPath.empty() || !path.is_absolute() || !pathIsInPrivateRuntimeRoot(path))
        return std::nullopt;

    const auto native = path.string();
    struct stat st {};
    if (stat(native.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || hasWritableGroupOrOther(st.st_mode) || st.st_size < 0 ||
        static_cast<std::uintmax_t>(st.st_size) > MAX_RECORD_REQUEST_BYTES)
        return std::nullopt;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;

    std::ostringstream out;
    out << file.rdbuf();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto value = out.str();
    if (value.empty() || value.size() > MAX_RECORD_REQUEST_BYTES)
        return std::nullopt;
    return value;
}

bool safeCodecToken(std::string_view token) {
    return !token.empty() && token.size() <= 64 && std::all_of(token.begin(), token.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    });
}

std::vector<std::string> splitShellLikeFlags(std::string_view raw, std::string& error) {
    std::vector<std::string> out;
    std::string              current;
    char                     quote = '\0';
    bool                     escape = false;

    for (const char ch : raw) {
        if (escape) {
            current.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (quote != '\0') {
            if (ch == quote)
                quote = '\0';
            else
                current.push_back(ch);
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (escape || quote != '\0') {
        error = "invalid record_gsr_flags quoting";
        return {};
    }
    if (!current.empty())
        out.push_back(std::move(current));
    return out;
}

std::optional<std::vector<std::string>> sanitizedGsrExtraFlags(std::string_view raw, std::string& error) {
    auto tokens = splitShellLikeFlags(raw, error);
    if (!error.empty())
        return std::nullopt;

    std::vector<std::string> out;
    out.reserve(tokens.size());
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        if (token == "-w" || token == "-o" || token.starts_with("-w=") || token.starts_with("-o=")) {
            error = "record_gsr_flags may not contain -w or -o";
            return std::nullopt;
        }
        out.push_back(token);
    }
    return out;
}

std::string gsrCaptureSource(const RecordingRequest& request) {
    if (request.mode == CaptureMode::Region ||
        request.mode == CaptureMode::Window ||
        (request.mode == CaptureMode::Fullscreen && request.targetGeometry.width > 0.0 && request.targetGeometry.height > 0.0)) {
        const int width = std::max(1, static_cast<int>(std::round(request.targetGeometry.width)));
        const int height = std::max(1, static_cast<int>(std::round(request.targetGeometry.height)));
        const int x = static_cast<int>(std::round(request.targetGeometry.x));
        const int y = static_cast<int>(std::round(request.targetGeometry.y));
        return std::to_string(width) + "x" + std::to_string(height) + "+" + std::to_string(x) + "+" + std::to_string(y);
    }
    return "screen";
}

std::string sanitizedCodec(std::string codec) {
    if (codec == "auto")
        return findVaapiRenderDevice() ? "h264_vaapi" : "libx264";
    if (!safeCodecToken(codec))
        return "libx264";
    return codec;
}

std::string sanitizedPreset(std::string preset) {
    if (!safeCodecToken(preset))
        return "veryfast";
    return preset;
}

Time::steady_dur frameIntervalForFps(int fps) {
    const auto safeFps = std::max(1, fps);
    auto       interval = std::chrono::duration_cast<Time::steady_dur>(std::chrono::duration<double>(1.0 / safeFps));
    if (interval <= Time::steady_dur::zero())
        interval = std::chrono::milliseconds(1);
    return interval;
}

int effectiveRecordingFps(const RecordingFrameRequest& request, int requestedFps) {
    int fps = std::clamp(requestedFps, 1, 240);
    if (request.mode != CaptureMode::Window)
        return fps;

    const int windowLimit = std::clamp<int>(static_cast<int>(request.defaults.recordWindowFpsLimit), 0, 240);
    if (windowLimit > 0)
        fps = std::min(fps, windowLimit);

    if (request.defaults.windowBackground == WindowBackground::Real) {
        const int realBgLimit = std::clamp<int>(static_cast<int>(request.defaults.recordWindowRealBgFpsLimit), 0, 240);
        if (realBgLimit > 0)
            fps = std::min(fps, realBgLimit);
    }

    return std::max(1, fps);
}

std::filesystem::path uniqueOutputPath(const CaptureDefaults& defaults) {
    auto dir = expandUserPath(defaults.saveDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::filesystem::path filename(makeTimestampedFilename(defaults.recordFilenameTemplate));
    if (filename.empty() || filename == "." || filename == "..")
        filename = "Recording.mp4";
    if (filename.extension().empty())
        filename += ".mp4";

    const auto stem = filename.stem().string().empty() ? std::string("Recording") : filename.stem().string();
    const auto ext = filename.extension().string().empty() ? std::string(".mp4") : filename.extension().string();
    for (int i = 0; i < 1000; ++i) {
        const auto candidate = dir / (i == 0 ? stem + ext : stem + "-" + std::to_string(i) + ext);
        if (!std::filesystem::exists(candidate, ec))
            return candidate;
    }

    return dir / (stem + "-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ext);
}

void setOwnerOnlyPermissions(const std::filesystem::path& path) {
    std::error_code ec;
    if (!path.empty())
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
}

bool resizeFrameNearest(RecordingFrame& frame, int width, int height) {
    if (frame.width == width && frame.height == height)
        return true;
    if (frame.rgba.empty() || frame.width <= 0 || frame.height <= 0 || width <= 0 || height <= 0)
        return false;

    std::vector<unsigned char> resized(static_cast<std::size_t>(width) * height * RGBA_BYTES_PER_PIXEL);
    for (int y = 0; y < height; ++y) {
        const int srcY = std::clamp(static_cast<int>((static_cast<long long>(y) * frame.height) / height), 0, frame.height - 1);
        for (int x = 0; x < width; ++x) {
            const int srcX = std::clamp(static_cast<int>((static_cast<long long>(x) * frame.width) / width), 0, frame.width - 1);
            const auto src = (static_cast<std::size_t>(srcY) * frame.width + srcX) * RGBA_BYTES_PER_PIXEL;
            const auto dst = (static_cast<std::size_t>(y) * width + x) * RGBA_BYTES_PER_PIXEL;
            std::copy(frame.rgba.data() + src, frame.rgba.data() + src + RGBA_BYTES_PER_PIXEL, resized.data() + dst);
        }
    }

    frame.width = width;
    frame.height = height;
    frame.rgba = std::move(resized);
    return true;
}

bool makeEvenFrame(RecordingFrame& frame) {
    const int evenWidth = frame.width & ~1;
    const int evenHeight = frame.height & ~1;
    if (evenWidth < 2 || evenHeight < 2)
        return false;
    if (evenWidth == frame.width && evenHeight == frame.height)
        return true;

    std::vector<unsigned char> trimmed(static_cast<std::size_t>(evenWidth) * evenHeight * RGBA_BYTES_PER_PIXEL);
    const std::size_t dstRowBytes = static_cast<std::size_t>(evenWidth) * RGBA_BYTES_PER_PIXEL;
    const std::size_t srcRowBytes = static_cast<std::size_t>(frame.width) * RGBA_BYTES_PER_PIXEL;
    for (int y = 0; y < evenHeight; ++y)
        std::copy(frame.rgba.data() + static_cast<std::size_t>(y) * srcRowBytes,
                  frame.rgba.data() + static_cast<std::size_t>(y) * srcRowBytes + dstRowBytes,
                  trimmed.data() + static_cast<std::size_t>(y) * dstRowBytes);

    frame.width = evenWidth;
    frame.height = evenHeight;
    frame.rgba = std::move(trimmed);
    return true;
}

class RawVideoEncoder {
  public:
    RawVideoEncoder(std::filesystem::path outputPath, int width, int height, int fps, std::string codec, std::string preset)
        : m_outputPath(std::move(outputPath)), m_width(width), m_height(height), m_fps(fps), m_codec(std::move(codec)), m_preset(std::move(preset)) {}

    ~RawVideoEncoder() {
        stopAndJoin(false);
    }

    LaunchResult start() {
        const auto ffmpeg = trustedFfmpegPath();
        if (!ffmpeg)
            return {.success = false, .error = "no trusted ffmpeg executable found"};
        const auto vaapiDevice = isVaapiCodec(m_codec) ? findVaapiRenderDevice() : std::optional<std::string>{};
        if (isVaapiCodec(m_codec) && !vaapiDevice)
            return {.success = false, .error = "no writable VAAPI render device found"};

        auto pipe = makePipe();
        if (!pipe)
            return {.success = false, .error = std::string("pipe failed: ") + std::strerror(errno)};

        std::vector<std::string> args{
            *ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
        };

        if (vaapiDevice) {
            args.push_back("-vaapi_device");
            args.push_back(*vaapiDevice);
        }

        const std::vector<std::string> inputArgs{
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgba",
            "-video_size",
            std::to_string(m_width) + "x" + std::to_string(m_height),
            "-framerate",
            std::to_string(m_fps),
            "-i",
            "pipe:0",
            "-an",
        };
        args.insert(args.end(), inputArgs.begin(), inputArgs.end());

        if (isVaapiCodec(m_codec)) {
            args.push_back("-vf");
            args.push_back("format=nv12,hwupload");
            args.push_back("-c:v");
            args.push_back(m_codec);
            args.push_back("-qp");
            args.push_back("23");
            args.push_back("-quality");
            args.push_back("7");
        } else {
            args.push_back("-c:v");
            args.push_back(m_codec);
        }

        if (m_codec == "libx264" || m_codec == "libx264rgb") {
            args.push_back("-preset");
            args.push_back(m_preset);
            args.push_back("-crf");
            args.push_back("23");
        }
        if (isNvencCodec(m_codec)) {
            args.push_back("-preset");
            args.push_back("p1");
            args.push_back("-cq");
            args.push_back("23");
        }
        if (m_codec != "libx264rgb" && !isHardwareCodec(m_codec)) {
            args.push_back("-pix_fmt");
            args.push_back("yuv420p");
        }
        args.push_back("-movflags");
        args.push_back("+faststart");
        args.push_back(m_outputPath.string());

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

        posix_spawn_file_actions_t fileActions {};
        if (const int error = posix_spawn_file_actions_init(&fileActions); error != 0) {
            closeFd(pipe->read);
            closeFd(pipe->write);
            return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(error)};
        }

        posix_spawn_file_actions_adddup2(&fileActions, pipe->read, STDIN_FILENO);
        posix_spawn_file_actions_addclose(&fileActions, pipe->read);
        posix_spawn_file_actions_addclose(&fileActions, pipe->write);
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 34)
        posix_spawn_file_actions_addclosefrom_np(&fileActions, 3);
#endif

        const int spawnError = posix_spawn(&m_pid, argv[0], &fileActions, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&fileActions);
        closeFd(pipe->read);
        if (spawnError != 0) {
            closeFd(pipe->write);
            return {.success = false, .error = std::string("ffmpeg exec failed: ") + std::strerror(spawnError)};
        }

        m_writeFd = pipe->write;
        pipe->write = -1;
        m_worker = std::thread([this] { workerMain(); });
        return {.success = true};
    }

    bool hasQueueSpace() const {
        std::lock_guard lock(m_mutex);
        return !m_stopping && m_frames.size() < MAX_FRAME_QUEUE;
    }

    bool enqueue(RecordingFrame&& frame) {
        std::lock_guard lock(m_mutex);
        if (m_stopping || m_frames.size() >= MAX_FRAME_QUEUE)
            return false;

        m_frames.push_back(std::move(frame.rgba));
        m_cv.notify_one();
        return true;
    }

    void stopAndJoin(bool drain) {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            if (!drain)
                m_frames.clear();
        }
        m_cv.notify_one();

        if (m_worker.joinable())
            m_worker.join();
    }

  private:
    bool writeAll(const std::vector<unsigned char>& frame) {
        const auto* data = reinterpret_cast<const char*>(frame.data());
        std::size_t written = 0;
        while (written < frame.size()) {
            const ssize_t chunk = write(m_writeFd, data + written, frame.size() - written);
            if (chunk < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (chunk == 0)
                return false;
            written += static_cast<std::size_t>(chunk);
        }
        return true;
    }

    void waitForFfmpeg() {
        if (m_pid <= 0)
            return;

        int status = 0;
        while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {
        }
        m_pid = -1;
        setOwnerOnlyPermissions(m_outputPath);
    }

    void workerMain() {
        while (true) {
            std::vector<unsigned char> frame;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopping || !m_frames.empty(); });
                if (m_frames.empty()) {
                    if (m_stopping)
                        break;
                    continue;
                }
                frame = std::move(m_frames.front());
                m_frames.pop_front();
            }

            if (!writeAll(frame))
                break;
        }

        closeFd(m_writeFd);
        waitForFfmpeg();
    }

    std::filesystem::path m_outputPath;
    int                   m_width = 0;
    int                   m_height = 0;
    int                   m_fps = 30;
    std::string           m_codec;
    std::string           m_preset;
    int                   m_writeFd = -1;
    pid_t                 m_pid = -1;
    mutable std::mutex    m_mutex;
    std::condition_variable m_cv;
    std::deque<std::vector<unsigned char>> m_frames;
    bool                                m_stopping = false;
    std::thread                         m_worker;
};

struct ActiveRecording {
    RecordingFrameRequest                    request;
    std::unique_ptr<RawVideoEncoder>         encoder;
    SP<CEventLoopTimer>                      timer;
    Time::steady_dur                         interval{std::chrono::milliseconds(33)};
    Time::steady_tp                          startedAt;
    std::filesystem::path                    outputPath;
    int                                      width = 0;
    int                                      height = 0;
    int                                      consecutiveFrameFailures = 0;
};

std::unique_ptr<ActiveRecording> g_recording;

struct ActiveGsrRecording {
    pid_t                 pid = -1;
    SP<CEventLoopTimer>   timer;
    std::filesystem::path outputPath;
};

std::unique_ptr<ActiveGsrRecording> g_gsrRecording;

void notifyRecording(const std::string& message, const CHyprColor& color = CHyprColor(0.2, 0.8, 0.3, 1.0), float timeoutMs = 3000) {
    if (g_pluginHandle)
        HyprlandAPI::addNotification(g_pluginHandle, "[hyprcapture] " + message, color, timeoutMs);
}

bool reapGsrRecordingIfExited() {
    if (!g_gsrRecording)
        return false;

    int status = 0;
    const pid_t result = waitpid(g_gsrRecording->pid, &status, WNOHANG);
    if (result == 0)
        return true;
    if (result == g_gsrRecording->pid || (result < 0 && errno == ECHILD)) {
        auto recording = std::move(g_gsrRecording);
        if (recording->timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(recording->timer);
        setOwnerOnlyPermissions(recording->outputPath);
        notifyRecording("recording finished: " + recording->outputPath.string());
        return false;
    }

    return true;
}

LaunchResult stopRecordingInternal(const std::string& reason, bool drain) {
    if (g_gsrRecording) {
        auto recording = std::move(g_gsrRecording);
        if (recording->timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(recording->timer);
        recording->timer.reset();
        if (recording->pid > 0) {
            kill(recording->pid, SIGINT);
            int status = 0;
            while (waitpid(recording->pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
        setOwnerOnlyPermissions(recording->outputPath);
        notifyRecording("recording " + reason + ": " + recording->outputPath.string());
        return {.success = true};
    }

    if (!g_recording)
        return {.success = false, .error = "no active recording"};

    auto recording = std::move(g_recording);
    if (recording->timer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(recording->timer);
    recording->timer.reset();
    if (recording->encoder)
        recording->encoder->stopAndJoin(drain);

    notifyRecording("recording " + reason + ": " + recording->outputPath.string());
    return {.success = true};
}

void scheduleGsrStopTimer(int maxSeconds) {
    if (!g_gsrRecording || !g_pEventLoopManager || maxSeconds <= 0)
        return;

    g_gsrRecording->timer = makeShared<CEventLoopTimer>(
        std::chrono::seconds(maxSeconds),
        [](SP<CEventLoopTimer> self, void*) {
            if (!g_gsrRecording || g_gsrRecording->timer.get() != self.get())
                return;
            stopRecordingInternal("stopped at max duration", true);
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_gsrRecording->timer);
}

void captureRecordingTick(SP<CEventLoopTimer> self) {
    if (!g_recording || g_recording->timer.get() != self.get())
        return;

    const auto now = Time::steadyNow();
    if (g_recording->request.defaults.recordMaxSeconds > 0 &&
        std::chrono::duration_cast<std::chrono::seconds>(now - g_recording->startedAt).count() >= g_recording->request.defaults.recordMaxSeconds) {
        stopRecordingInternal("stopped at max duration", true);
        return;
    }

    if (g_recording->encoder && g_recording->encoder->hasQueueSpace()) {
        auto frame = captureRecordingFrame(g_recording->request);
        if (frame && makeEvenFrame(*frame) && resizeFrameNearest(*frame, g_recording->width, g_recording->height) &&
            g_recording->encoder->enqueue(std::move(*frame))) {
            g_recording->consecutiveFrameFailures = 0;
        } else {
            ++g_recording->consecutiveFrameFailures;
        }
    }

    if (g_recording && g_recording->consecutiveFrameFailures >= MAX_CONSECUTIVE_FRAME_FAILURES) {
        stopRecordingInternal("stopped after frame failures", true);
        return;
    }

    if (g_recording && g_recording->timer)
        self->updateTimeout(g_recording->interval);
}

void scheduleRecordingTimer() {
    if (!g_recording || !g_pEventLoopManager)
        return;

    g_recording->timer = makeShared<CEventLoopTimer>(
        g_recording->interval,
        [](SP<CEventLoopTimer> self, void*) {
            captureRecordingTick(self);
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_recording->timer);
}

LaunchResult spawnGpuScreenRecorder(const RecordingRequest& request, const std::filesystem::path& outputPath, pid_t& pid) {
    const auto executable = trustedGpuScreenRecorderPath();
    if (!executable)
        return {.success = false, .error = "no trusted gpu-screen-recorder executable found"};

    std::string flagsError;
    auto        extraFlags = sanitizedGsrExtraFlags(request.defaults.recordGsrFlags, flagsError);
    if (!extraFlags)
        return {.success = false, .error = flagsError.empty() ? "invalid record_gsr_flags" : flagsError};

    const int fps = std::clamp<int>(static_cast<int>(request.defaults.recordFps), 1, 240);
    std::vector<std::string> args{
        *executable,
        "-w",
        gsrCaptureSource(request),
        "-f",
        std::to_string(fps),
        "-cursor",
        request.defaults.includeCursor ? "yes" : "no",
    };
    args.insert(args.end(), extraFlags->begin(), extraFlags->end());
    args.push_back("-o");
    args.push_back(outputPath.string());

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

    posix_spawn_file_actions_t fileActions {};
    if (const int error = posix_spawn_file_actions_init(&fileActions); error != 0)
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(error)};
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 34)
    posix_spawn_file_actions_addclosefrom_np(&fileActions, 3);
#endif

    const int spawnError = posix_spawn(&pid, argv[0], &fileActions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fileActions);
    if (spawnError != 0)
        return {.success = false, .error = std::string("gpu-screen-recorder exec failed: ") + std::strerror(spawnError)};

    return {.success = true};
}

LaunchResult startGsrRecording(const RecordingRequest& request) {
    if ((request.mode == CaptureMode::Region || request.mode == CaptureMode::Window) && (request.targetGeometry.width <= 0.0 || request.targetGeometry.height <= 0.0))
        return {.success = false, .error = "invalid recording geometry"};

    const auto outputPath = uniqueOutputPath(request.defaults);
    pid_t      pid = -1;
    if (const auto result = spawnGpuScreenRecorder(request, outputPath, pid); !result.success)
        return result;

    g_gsrRecording = std::make_unique<ActiveGsrRecording>();
    g_gsrRecording->pid = pid;
    g_gsrRecording->outputPath = outputPath;
    scheduleGsrStopTimer(std::clamp<int>(static_cast<int>(request.defaults.recordMaxSeconds), 0, 24 * 60 * 60));

    notifyRecording("recording started via gpu-screen-recorder: " + outputPath.string());
    return {.success = true};
}

} // namespace

LaunchResult startRecordingFromRequestFile(const std::string& path) {
    if (g_recording || reapGsrRecordingIfExited())
        return {.success = false, .error = "recording already active"};
    if (!g_pEventLoopManager)
        return {.success = false, .error = "Hyprland event loop unavailable"};

    const auto requestJson = readPrivateRequestFile(path);
    if (!requestJson)
        return {.success = false, .error = "invalid recording request file"};

    const auto request = decodeRecordingRequestJson(*requestJson);
    if (!request)
        return {.success = false, .error = "invalid recording request metadata"};

    if (request->mode == CaptureMode::Fullscreen || request->mode == CaptureMode::Region ||
        (request->mode == CaptureMode::Window && request->defaults.recordWindowBackend == RecordWindowBackend::GsrVisible))
        return startGsrRecording(*request);

    RecordingFrameRequest frameRequest{.defaults = request->defaults,
                                       .mode = request->mode,
                                       .targetGeometry = request->targetGeometry,
                                       .windowAddress = request->windowAddress};

    auto firstFrame = captureRecordingFrame(frameRequest);
    if (!firstFrame || !makeEvenFrame(*firstFrame))
        return {.success = false, .error = "failed to capture first recording frame"};

    const int requestedFps = std::clamp<int>(static_cast<int>(request->defaults.recordFps), 1, 240);
    const int fps = effectiveRecordingFps(frameRequest, requestedFps);
    const auto outputPath = uniqueOutputPath(request->defaults);
    auto encoder = std::make_unique<RawVideoEncoder>(outputPath,
                                                     firstFrame->width,
                                                     firstFrame->height,
                                                     fps,
                                                     sanitizedCodec(request->defaults.recordCodec),
                                                     sanitizedPreset(request->defaults.recordPreset));
    if (const auto result = encoder->start(); !result.success)
        return result;

    const int width = firstFrame->width;
    const int height = firstFrame->height;
    encoder->enqueue(std::move(*firstFrame));

    g_recording = std::make_unique<ActiveRecording>();
    g_recording->request = std::move(frameRequest);
    g_recording->encoder = std::move(encoder);
    g_recording->interval = frameIntervalForFps(fps);
    g_recording->startedAt = Time::steadyNow();
    g_recording->outputPath = outputPath;
    g_recording->width = width;
    g_recording->height = height;
    scheduleRecordingTimer();

    if (fps < requestedFps)
        notifyRecording("window recording limited to " + std::to_string(fps) + " fps to avoid compositor stalls", CHyprColor(1.0, 0.72, 0.2, 1.0), 5000);
    notifyRecording("recording started: " + outputPath.string());
    return {.success = true};
}

LaunchResult stopRecording(const std::string& reason) {
    return stopRecordingInternal(reason.empty() ? "stopped" : reason, true);
}

bool isRecordingActive() {
    return static_cast<bool>(g_recording) || reapGsrRecordingIfExited();
}

void shutdownRecording() {
    if (g_recording || g_gsrRecording)
        stopRecordingInternal("stopped during plugin unload", false);
}

} // namespace hyprcapture
