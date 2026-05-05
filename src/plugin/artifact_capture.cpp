#include "plugin/artifact_capture.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef private

#include <GLES2/gl2.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <system_error>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>
#include <vector>

extern HANDLE g_pluginHandle;

namespace hyprcapture {
namespace {

struct RgbaReadback {
    std::vector<unsigned char> pixels;
    int                        cropX = 0;
    int                        cropTopY = 0;
    int                        width = 0;
    int                        height = 0;
};

struct PixelBounds {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RealBackgroundCaptureState {
    PHLWINDOW    window;
    int          cropX = 0;
    int          cropY = 0;
    int          width = 0;
    int          height = 0;
    bool         queued = false;
    bool         captured = false;
    bool         awaitingBlurBackground = false;
    bool         blurCaptured = false;
    RgbaReadback readback;
};

struct PendingRealBackgroundCapture {
    PHLWINDOW             window;
    PHLMONITOR            monitor;
    CBox                  artifactBox;
    std::filesystem::path path;
    std::size_t           windowIndex = 0;
};

constexpr int WINDOW_ARTIFACT_CROP_PADDING = 128;
constexpr std::size_t MAX_SESSION_MONITORS = 64;
constexpr std::size_t MAX_SESSION_WINDOWS = 512;
constexpr std::size_t MAX_WINDOW_METADATA_BYTES = 4096;
constexpr std::size_t MAX_SESSION_JSON_BYTES = 8 * 1024 * 1024;
constexpr int         MAX_RGBA_READBACK_DIMENSION = 32768;
constexpr std::size_t MAX_RGBA_READBACK_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t RGBA_BYTES_PER_PIXEL = 4;

struct RgbaReadbackRegion {
    int         outputCropX = 0;
    int         outputCropTopY = 0;
    int         outputWidth = 0;
    int         outputHeight = 0;
    int         srcX = 0;
    int         srcTopY = 0;
    int         srcWidth = 0;
    int         srcHeight = 0;
    int         dstX = 0;
    int         dstY = 0;
    std::size_t outputBytes = 0;
    std::size_t sourceBytes = 0;
};

Rect toRect(const CBox& box) {
    return {.x = box.x, .y = box.y, .width = box.w, .height = box.h};
}

Rect monitorRect(const PHLMONITOR& monitor) {
    return {.x = monitor->m_position.x, .y = monitor->m_position.y, .width = monitor->m_size.x, .height = monitor->m_size.y};
}

bool intersects(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
}

bool ensurePrivateDirectory(const std::filesystem::path& path) {
    const auto native = path.string();
    if (native.empty())
        return false;

    if (mkdir(native.c_str(), 0700) != 0 && errno != EEXIST)
        return false;

    struct stat st {};
    if (lstat(native.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        return false;

    if ((st.st_mode & 0777) != 0700 && chmod(native.c_str(), 0700) != 0)
        return false;

    return true;
}

std::filesystem::path artifactRoot(const std::string& sessionId) {
    const auto rootName = "hyprcapture-" + std::to_string(static_cast<unsigned long long>(geteuid()));
    for (const auto& base : {std::filesystem::path{"/dev/shm"}, std::filesystem::path{"/tmp"}, std::filesystem::temp_directory_path()}) {
        const auto userRoot = base / rootName;
        if (!ensurePrivateDirectory(userRoot))
            continue;

        const auto root = userRoot / sessionId;
        if (ensurePrivateDirectory(root))
            return root;
    }

    return {};
}

std::vector<std::filesystem::path> artifactRootCandidates(const std::string& sessionId) {
    const auto rootName = "hyprcapture-" + std::to_string(static_cast<unsigned long long>(geteuid()));
    std::vector<std::filesystem::path> roots;
    for (const auto& base : {std::filesystem::path{"/dev/shm"}, std::filesystem::path{"/tmp"}, std::filesystem::temp_directory_path()}) {
        const auto root = base / rootName / sessionId;
        if (std::find(roots.begin(), roots.end(), root) == roots.end())
            roots.push_back(root);
    }
    return roots;
}

std::string boundedString(const std::string& value, std::size_t maxBytes) {
    if (value.size() <= maxBytes)
        return value;
    return value.substr(0, maxBytes);
}

bool containsPath(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

int clampedIntFromDouble(double value) {
    if (!std::isfinite(value))
        return value < 0.0 ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max();
    if (value <= static_cast<double>(std::numeric_limits<int>::min()))
        return std::numeric_limits<int>::min();
    if (value >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<int>(value);
}

int positiveIntFromDouble(double value) {
    if (!std::isfinite(value) || value <= 0.0)
        return 1;
    if (value >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return std::max(1, static_cast<int>(value));
}

int positiveRoundedIntFromDouble(double value) {
    if (!std::isfinite(value) || value <= 0.0)
        return 1;
    if (value >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return std::max(1, static_cast<int>(std::lround(value)));
}

bool checkedRgbaByteSize(int width, int height, std::size_t& bytes) {
    bytes = 0;
    if (width <= 0 || height <= 0 || width > MAX_RGBA_READBACK_DIMENSION || height > MAX_RGBA_READBACK_DIMENSION)
        return false;

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h)
        return false;

    const auto pixels = w * h;
    if (pixels > std::numeric_limits<std::size_t>::max() / RGBA_BYTES_PER_PIXEL)
        return false;

    bytes = pixels * RGBA_BYTES_PER_PIXEL;
    return bytes <= MAX_RGBA_READBACK_BYTES;
}

int clampToFramebuffer(std::int64_t value, int framebufferExtent) {
    if (value <= 0)
        return 0;
    if (value >= framebufferExtent)
        return framebufferExtent;
    return static_cast<int>(value);
}

bool prepareRgbaReadbackRegion(int framebufferWidth,
                               int framebufferHeight,
                               int cropX,
                               int cropTopY,
                               int cropWidth,
                               int cropHeight,
                               RgbaReadbackRegion& region) {
    region = {};
    if (framebufferWidth <= 0 || framebufferHeight <= 0 || cropWidth <= 0 || cropHeight <= 0)
        return false;

    const auto cropLeft = static_cast<std::int64_t>(cropX);
    const auto cropTop = static_cast<std::int64_t>(cropTopY);
    const auto cropRight = cropLeft + static_cast<std::int64_t>(cropWidth);
    const auto cropBottom = cropTop + static_cast<std::int64_t>(cropHeight);

    region.srcX = clampToFramebuffer(cropLeft, framebufferWidth);
    region.srcTopY = clampToFramebuffer(cropTop, framebufferHeight);
    const int srcRight = clampToFramebuffer(cropRight, framebufferWidth);
    const int srcBottom = clampToFramebuffer(cropBottom, framebufferHeight);
    region.srcWidth = srcRight - region.srcX;
    region.srcHeight = srcBottom - region.srcTopY;

    std::size_t requestedBytes = 0;
    if (checkedRgbaByteSize(cropWidth, cropHeight, requestedBytes)) {
        region.outputCropX = cropX;
        region.outputCropTopY = cropTopY;
        region.outputWidth = cropWidth;
        region.outputHeight = cropHeight;
        region.dstX = region.srcX - cropX;
        region.dstY = region.srcTopY - cropTopY;
        region.outputBytes = requestedBytes;
    } else {
        if (region.srcWidth <= 0 || region.srcHeight <= 0)
            return false;

        if (!checkedRgbaByteSize(region.srcWidth, region.srcHeight, region.outputBytes))
            return false;

        region.outputCropX = region.srcX;
        region.outputCropTopY = region.srcTopY;
        region.outputWidth = region.srcWidth;
        region.outputHeight = region.srcHeight;
        region.dstX = 0;
        region.dstY = 0;
    }

    if (region.srcWidth > 0 && region.srcHeight > 0 && !checkedRgbaByteSize(region.srcWidth, region.srcHeight, region.sourceBytes))
        return false;

    return true;
}

void rememberParent(std::vector<std::filesystem::path>& parents, const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (!parent.empty() && !containsPath(parents, parent))
        parents.push_back(parent);
}

bool writeRgbaFile(const std::filesystem::path& path, const std::vector<unsigned char>& pixels) {
    const auto native = path.string();
    const int  fd = open(native.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;

    const auto* data = reinterpret_cast<const char*>(pixels.data());
    std::size_t written = 0;
    while (written < pixels.size()) {
        const ssize_t chunk = write(fd, data + written, pixels.size() - written);
        if (chunk < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(native.c_str());
            return false;
        }
        if (chunk == 0) {
            close(fd);
            unlink(native.c_str());
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }

    if (close(fd) != 0) {
        unlink(native.c_str());
        return false;
    }
    return true;
}

unsigned char alphaAt(const RgbaReadback& readback, int x, int y) {
    const auto i = (static_cast<std::size_t>(y) * readback.width + x) * 4U + 3U;
    return readback.pixels[i];
}

bool findAlphaBounds(const RgbaReadback& readback, PixelBounds& bounds) {
    if (readback.width <= 0 || readback.height <= 0 || readback.pixels.empty())
        return false;

    int minX = readback.width;
    int minY = readback.height;
    int maxX = -1;
    int maxY = -1;

    for (int y = 0; y < readback.height; ++y) {
        for (int x = 0; x < readback.width; ++x) {
            if (alphaAt(readback, x, y) == 0)
                continue;

            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }

    if (maxX < minX || maxY < minY)
        return false;

    bounds = {.x = minX, .y = minY, .width = maxX - minX + 1, .height = maxY - minY + 1};
    return true;
}

RgbaReadback cropReadback(const RgbaReadback& readback, const PixelBounds& bounds) {
    if (bounds.width <= 0 || bounds.height <= 0 || readback.pixels.empty())
        return {};

    RgbaReadback cropped;
    cropped.cropX = readback.cropX + bounds.x;
    cropped.cropTopY = readback.cropTopY + bounds.y;
    cropped.width = bounds.width;
    cropped.height = bounds.height;
    std::size_t pixelBytes = 0;
    if (!checkedRgbaByteSize(bounds.width, bounds.height, pixelBytes))
        return {};
    cropped.pixels.assign(pixelBytes, 0);

    const std::size_t rowBytes = static_cast<std::size_t>(bounds.width) * RGBA_BYTES_PER_PIXEL;
    for (int y = 0; y < bounds.height; ++y) {
        const auto* src = readback.pixels.data() + (static_cast<std::size_t>(bounds.y + y) * readback.width + bounds.x) * RGBA_BYTES_PER_PIXEL;
        auto*       dst = cropped.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        std::copy(src, src + rowBytes, dst);
    }

    return cropped;
}

RgbaReadback readRgbaFramebufferRegion(CFramebuffer& framebuffer, int cropX, int cropTopY, int cropWidth, int cropHeight, bool directGlY = false) {
    const int framebufferWidth = positiveRoundedIntFromDouble(framebuffer.m_size.x);
    const int framebufferHeight = positiveRoundedIntFromDouble(framebuffer.m_size.y);
    RgbaReadbackRegion region;
    if (!prepareRgbaReadbackRegion(framebufferWidth, framebufferHeight, cropX, cropTopY, cropWidth, cropHeight, region))
        return {};

    RgbaReadback readback;
    readback.cropX = region.outputCropX;
    readback.cropTopY = region.outputCropTopY;
    readback.width = region.outputWidth;
    readback.height = region.outputHeight;
    readback.pixels.assign(region.outputBytes, 0);

    if (region.srcWidth > 0 && region.srcHeight > 0) {
        std::vector<unsigned char> rows(region.sourceBytes);
        GLint previousReadFramebuffer = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer.getFBID());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        const int readY = directGlY ? region.srcTopY : framebufferHeight - region.srcTopY - region.srcHeight;
        glReadPixels(region.srcX, readY, region.srcWidth, region.srcHeight, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);

        for (int y = 0; y < region.srcHeight; ++y) {
            const auto* src = rows.data() + static_cast<std::size_t>(y) * region.srcWidth * RGBA_BYTES_PER_PIXEL;
            auto*       dst = readback.pixels.data() +
                (static_cast<std::size_t>(region.dstY + y) * region.outputWidth + region.dstX) * RGBA_BYTES_PER_PIXEL;
            std::copy(src, src + static_cast<std::size_t>(region.srcWidth) * RGBA_BYTES_PER_PIXEL, dst);
        }
    }

    return readback;
}

RgbaReadback readRenderPassFramebufferRegion(CFramebuffer& framebuffer, int cropX, int cropTopY, int cropWidth, int cropHeight) {
    return readRgbaFramebufferRegion(framebuffer, cropX, cropTopY, cropWidth, cropHeight, true);
}

class RealBackgroundCapturePass final : public IPassElement {
  public:
    explicit RealBackgroundCapturePass(RealBackgroundCaptureState* state) : m_state(state) {}

    void draw(const CRegion&) override {
        if (!m_state || m_state->captured || !g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.currentFB)
            return;

        m_state->readback = readRenderPassFramebufferRegion(*g_pHyprOpenGL->m_renderData.currentFB, m_state->cropX, m_state->cropY, m_state->width, m_state->height);
        m_state->captured = !m_state->readback.pixels.empty();
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    const char* passName() override {
        return "HyprshotRealBackgroundCapturePass";
    }

    bool undiscardable() override {
        return true;
    }

    bool disableSimplification() override {
        return true;
    }

  private:
    RealBackgroundCaptureState* m_state = nullptr;
};

class FullSurfaceVisibleRegionOverride {
  public:
    explicit FullSurfaceVisibleRegionOverride(const PHLWINDOW& window) {
        if (!window || !window->wlSurface() || !window->wlSurface()->resource())
            return;

        window->wlSurface()->resource()->breadthfirst(
            [this](SP<CWLSurfaceResource> resource, const Vector2D&, void*) {
                auto surface = Desktop::View::CWLSurface::fromResource(resource);
                if (!surface)
                    return;

                m_records.push_back({.surface = surface, .visibleRegion = surface->m_visibleRegion});

                const int width = std::max(1, static_cast<int>(std::lround(resource->m_current.bufferSize.x > 0 ? resource->m_current.bufferSize.x :
                                                                                                                 resource->m_current.size.x)));
                const int height = std::max(1, static_cast<int>(std::lround(resource->m_current.bufferSize.y > 0 ? resource->m_current.bufferSize.y :
                                                                                                                   resource->m_current.size.y)));
                surface->m_visibleRegion = CRegion{0, 0, width, height};
            },
            nullptr);
    }

    ~FullSurfaceVisibleRegionOverride() {
        for (auto& record : m_records) {
            if (record.surface)
                record.surface->m_visibleRegion = record.visibleRegion;
        }
    }

    FullSurfaceVisibleRegionOverride(const FullSurfaceVisibleRegionOverride&) = delete;
    FullSurfaceVisibleRegionOverride& operator=(const FullSurfaceVisibleRegionOverride&) = delete;

  private:
    struct Record {
        SP<Desktop::View::CWLSurface> surface;
        CRegion                      visibleRegion;
    };

    std::vector<Record> m_records;
};

struct RealBackgroundRenderHookContext {
    PHLMONITOR                              monitor;
    std::vector<RealBackgroundCaptureState>* states = nullptr;
    RealBackgroundCaptureState*             activeBlurState = nullptr;
};

using RenderWindowFn = void (*)(void*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, eRenderPassMode, bool, bool);
using RenderTextureInternalFn = void (*)(void*, SP<CTexture>, const CBox&, const CHyprOpenGLImpl::STextureRenderData&);
using RenderTextureWithBlurInternalFn = void (*)(void*, SP<CTexture>, const CBox&, const CHyprOpenGLImpl::STextureRenderData&);

RealBackgroundRenderHookContext* g_realBackgroundRenderHookContext = nullptr;
RenderWindowFn                   g_renderWindowOriginal = nullptr;
RenderTextureInternalFn          g_renderTextureInternalOriginal = nullptr;
RenderTextureWithBlurInternalFn  g_renderTextureWithBlurInternalOriginal = nullptr;

PHLWINDOW currentRenderWindow() {
    if (!g_pHyprOpenGL)
        return {};
    return g_pHyprOpenGL->m_renderData.currentWindow.lock();
}

RealBackgroundCaptureState* findRealBackgroundStateForWindow(PHLWINDOW window) {
    if (!g_realBackgroundRenderHookContext || !g_realBackgroundRenderHookContext->states || !window)
        return nullptr;

    for (auto& state : *g_realBackgroundRenderHookContext->states) {
        if (state.window && state.window.get() == window.get())
            return &state;
    }

    return nullptr;
}

bool isActiveBlurBackgroundPass(RealBackgroundCaptureState* state, const CHyprOpenGLImpl::STextureRenderData& data) {
    if (!state || !state->awaitingBlurBackground || state->blurCaptured || data.blur || data.discardActive || !data.allowCustomUV || !g_pHyprOpenGL ||
        !g_pHyprOpenGL->m_renderData.currentFB)
        return false;

    const auto window = currentRenderWindow();
    return window && state->window && window.get() == state->window.get();
}

void hkRenderWindow(void* rendererThisptr,
                    PHLWINDOW window,
                    PHLMONITOR monitor,
                    const Time::steady_tp& time,
                    bool decorate,
                    eRenderPassMode mode,
                    bool ignorePosition,
                    bool standalone) {
    if (g_realBackgroundRenderHookContext && g_realBackgroundRenderHookContext->states && monitor && g_realBackgroundRenderHookContext->monitor &&
        monitor.get() == g_realBackgroundRenderHookContext->monitor.get() && g_pHyprRenderer) {
        for (auto& state : *g_realBackgroundRenderHookContext->states) {
            if (state.queued || state.captured || !state.window || !window || state.window.get() != window.get())
                continue;

            state.queued = true;
            g_pHyprRenderer->m_renderPass.add(makeUnique<RealBackgroundCapturePass>(&state));
            break;
        }
    }

    if (g_renderWindowOriginal)
        g_renderWindowOriginal(rendererThisptr, window, monitor, time, decorate, mode, ignorePosition, standalone);
}

void hkRenderTextureInternal(void* openGLThisptr, SP<CTexture> texture, const CBox& box, const CHyprOpenGLImpl::STextureRenderData& data) {
    auto* state = g_realBackgroundRenderHookContext ? g_realBackgroundRenderHookContext->activeBlurState : nullptr;
    const bool captureAfterDraw = isActiveBlurBackgroundPass(state, data);

    if (g_renderTextureInternalOriginal)
        g_renderTextureInternalOriginal(openGLThisptr, texture, box, data);

    if (!captureAfterDraw || !state || !g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.currentFB)
        return;

    state->awaitingBlurBackground = false;
    auto readback = readRenderPassFramebufferRegion(*g_pHyprOpenGL->m_renderData.currentFB, state->cropX, state->cropY, state->width, state->height);
    if (readback.pixels.empty())
        return;

    state->readback = std::move(readback);
    state->captured = true;
    state->blurCaptured = true;
}

void hkRenderTextureWithBlurInternal(void* openGLThisptr, SP<CTexture> texture, const CBox& box, const CHyprOpenGLImpl::STextureRenderData& data) {
    auto* context = g_realBackgroundRenderHookContext;
    auto* state = findRealBackgroundStateForWindow(currentRenderWindow());

    if (!context || !state || state->blurCaptured || !g_renderTextureWithBlurInternalOriginal) {
        if (g_renderTextureWithBlurInternalOriginal)
            g_renderTextureWithBlurInternalOriginal(openGLThisptr, texture, box, data);
        return;
    }

    auto* previousActiveBlurState = context->activeBlurState;
    const bool previousAwaitingBlurBackground = state->awaitingBlurBackground;
    context->activeBlurState = state;
    state->awaitingBlurBackground = true;

    g_renderTextureWithBlurInternalOriginal(openGLThisptr, texture, box, data);

    state->awaitingBlurBackground = previousAwaitingBlurBackground;
    context->activeBlurState = previousActiveBlurState;
}

void repairTopTransparentSeam(RgbaReadback& readback) {
    if (readback.width <= 0 || readback.height <= 2 || readback.pixels.empty())
        return;

    constexpr unsigned char MAX_SEAM_ALPHA = 4;
    constexpr unsigned char MIN_WINDOW_ALPHA = 128;

    const int minRepairColumns = std::max(16, readback.width / 3);
    const int minRepairSpan = std::max(16, readback.width / 2);
    const int maxScanY = std::min(readback.height - 1, 96);

    for (int y = 1; y < maxScanY; ++y) {
        int first = -1;
        int last = -1;
        int repairColumns = 0;

        for (int x = 0; x < readback.width; ++x) {
            if (alphaAt(readback, x, y) > MAX_SEAM_ALPHA || alphaAt(readback, x, y - 1) < MIN_WINDOW_ALPHA || alphaAt(readback, x, y + 1) < MIN_WINDOW_ALPHA)
                continue;

            if (first < 0)
                first = x;
            last = x;
            ++repairColumns;
        }

        if (repairColumns < minRepairColumns || first < 0 || last - first + 1 < minRepairSpan)
            continue;

        for (int x = first; x <= last; ++x) {
            if (alphaAt(readback, x, y) > MAX_SEAM_ALPHA || alphaAt(readback, x, y - 1) < MIN_WINDOW_ALPHA || alphaAt(readback, x, y + 1) < MIN_WINDOW_ALPHA)
                continue;

            const auto src = (static_cast<std::size_t>(y + 1) * readback.width + x) * 4U;
            const auto dst = (static_cast<std::size_t>(y) * readback.width + x) * 4U;
            std::copy(readback.pixels.data() + src, readback.pixels.data() + src + 4U, readback.pixels.data() + dst);
        }

        return;
    }
}

void unpremultiplyAlpha(RgbaReadback& readback) {
    if (readback.pixels.empty())
        return;

    for (std::size_t i = 0; i + 3 < readback.pixels.size(); i += 4U) {
        const auto alpha = readback.pixels[i + 3];
        if (alpha == 0) {
            readback.pixels[i] = 0;
            readback.pixels[i + 1] = 0;
            readback.pixels[i + 2] = 0;
            continue;
        }
        if (alpha == 255)
            continue;

        for (int channel = 0; channel < 3; ++channel) {
            const int straight = (static_cast<int>(readback.pixels[i + channel]) * 255 + alpha / 2) / alpha;
            readback.pixels[i + channel] = static_cast<unsigned char>(std::min(255, straight));
        }
    }
}

bool writeRgbaFramebufferRegion(CFramebuffer& framebuffer, const std::filesystem::path& path, int cropX, int cropTopY, int cropWidth, int cropHeight) {
    auto readback = readRgbaFramebufferRegion(framebuffer, cropX, cropTopY, cropWidth, cropHeight);
    return !readback.pixels.empty() && writeRgbaFile(path, readback.pixels);
}

bool writeRgbaFramebuffer(CFramebuffer& framebuffer, const std::filesystem::path& path) {
    return writeRgbaFramebufferRegion(framebuffer,
                                      path,
                                      0,
                                      0,
                                      positiveRoundedIntFromDouble(framebuffer.m_size.x),
                                      positiveRoundedIntFromDouble(framebuffer.m_size.y));
}

CBox renderedWindowBox(const PHLWINDOW& window, CBox box) {
    if (window->m_workspace && !window->m_pinned)
        box.translate(window->m_workspace->m_renderOffset->value());
    box.translate(window->m_floatingOffset);
    return box;
}

CBox renderedWindowGoalMainSurfaceBox(const PHLWINDOW& window) {
    if (!window)
        return {};

    CBox box{window->m_realPosition->goal().x, window->m_realPosition->goal().y, window->m_realSize->goal().x, window->m_realSize->goal().y};
    return renderedWindowBox(window, box);
}

std::string pointerId(const void* ptr) {
    std::ostringstream out;
    out << std::hex << reinterpret_cast<std::uintptr_t>(ptr);
    return out.str();
}

class WindowAnimationGoalOverride {
  public:
    explicit WindowAnimationGoalOverride(const PHLWINDOW& window) : m_window(window) {
        if (!m_window || !m_window->m_realPosition || !m_window->m_realSize)
            return;

        m_position = m_window->m_realPosition->value();
        m_size = m_window->m_realSize->value();
        m_active = true;
        setPositionOffset({});
    }

    void setPositionOffset(const Vector2D& offset) {
        if (!m_active || !m_window || !m_window->m_realPosition || !m_window->m_realSize)
            return;

        m_window->m_realPosition->value() = m_window->m_realPosition->goal() + offset;
        m_window->m_realSize->value() = m_window->m_realSize->goal();
        m_window->updateWindowDecos();
    }

    ~WindowAnimationGoalOverride() {
        if (!m_active || !m_window || !m_window->m_realPosition || !m_window->m_realSize)
            return;

        m_window->m_realPosition->value() = m_position;
        m_window->m_realSize->value() = m_size;
        m_window->updateWindowDecos();
    }

    WindowAnimationGoalOverride(const WindowAnimationGoalOverride&) = delete;
    WindowAnimationGoalOverride& operator=(const WindowAnimationGoalOverride&) = delete;

  private:
    PHLWINDOW m_window;
    Vector2D  m_position;
    Vector2D  m_size;
    bool      m_active = false;
};

bool renderMonitorArtifact(const PHLMONITOR& monitor, const Time::steady_tp& frozenTime, const std::filesystem::path& path, int& width, int& height) {
    if (!monitor || !monitor->m_activeWorkspace || !g_pHyprRenderer || !g_pHyprOpenGL)
        return false;

    width = positiveRoundedIntFromDouble(monitor->m_pixelSize.x);
    height = positiveRoundedIntFromDouble(monitor->m_pixelSize.y);
    std::size_t framebufferBytes = 0;
    if (!checkedRgbaByteSize(width, height, framebufferBytes))
        return false;

    CFramebuffer framebuffer;
    if (!framebuffer.alloc(width, height, monitor->m_output->state->state().drmFormat))
        return false;

    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprOpenGL->m_renderData.blockScreenShader;
    CRegion fakeDamage{0, 0, width, height};

    g_pHyprRenderer->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &framebuffer)) {
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
        return false;
    }

    g_pHyprOpenGL->clear(CHyprColor{0.0, 0.0, 0.0, 1.0});
    g_pHyprRenderer->renderWorkspace(monitor, monitor->m_activeWorkspace, frozenTime, CBox{0, 0, static_cast<double>(width), static_cast<double>(height)});
    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
    g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockShader;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;

    return writeRgbaFramebuffer(framebuffer, path);
}

bool renderWindowArtifact(const PHLWINDOW& window,
                          const PHLMONITOR& monitor,
                          const Time::steady_tp& frozenTime,
                          bool decorate,
                          const std::filesystem::path& path,
                          int& width,
                          int& height,
                          CBox& artifactBox) {
    if (!window || !monitor || !g_pHyprRenderer || !g_pHyprOpenGL)
        return false;

    WindowAnimationGoalOverride windowGoal(window);
    const CBox fullBox = renderedWindowBox(window, window->getFullWindowBoundingBox());
    CBox sourceCropBox = fullBox.copy().translate(-monitor->m_position).scale(monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale).round();
    width = positiveIntFromDouble(sourceCropBox.w);
    height = positiveIntFromDouble(sourceCropBox.h);
    const int framebufferWidth = positiveRoundedIntFromDouble(monitor->m_pixelSize.x);
    const int framebufferHeight = positiveRoundedIntFromDouble(monitor->m_pixelSize.y);
    std::size_t framebufferBytes = 0;
    if (!checkedRgbaByteSize(framebufferWidth, framebufferHeight, framebufferBytes))
        return false;
    const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;
    const int targetCropX = width < framebufferWidth ? (framebufferWidth - width) / 2 : 0;
    const int targetCropY = height < framebufferHeight ? (framebufferHeight - height) / 2 : 0;
    const Vector2D renderOffset = monitor->m_position + Vector2D{targetCropX / scale, targetCropY / scale} - fullBox.pos();
    windowGoal.setPositionOffset(renderOffset);
    CBox renderCropBox = fullBox.copy().translate(renderOffset).translate(-monitor->m_position).scale(scale).round();

    CFramebuffer framebuffer;
    const auto drmFormat = monitor->m_output && monitor->m_output->state ? monitor->m_output->state->state().drmFormat : DRM_FORMAT_ABGR8888;
    if (!framebuffer.alloc(framebufferWidth, framebufferHeight, DRM_FORMAT_ABGR8888) && !framebuffer.alloc(framebufferWidth, framebufferHeight, drmFormat))
        return false;

    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprOpenGL->m_renderData.blockScreenShader;
    const bool previousRenderingSnapshot = g_pHyprRenderer->m_bRenderingSnapshot;

    const auto renderIntoFramebuffer = [&]() {
        CRegion fakeDamage{0, 0, framebufferWidth, framebufferHeight};
        g_pHyprRenderer->makeEGLCurrent();
        g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
        if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &framebuffer)) {
            g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
            return false;
        }

        g_pHyprRenderer->m_bRenderingSnapshot = true;
        FullSurfaceVisibleRegionOverride fullVisibleRegion(window);
        g_pHyprOpenGL->clear(CHyprColor{0.0, 0.0, 0.0, 0.0});
        g_pHyprRenderer->renderWindow(window, monitor, frozenTime, decorate, RENDER_PASS_ALL, false, false);
        g_pHyprRenderer->m_bRenderingSnapshot = previousRenderingSnapshot;

        g_pHyprOpenGL->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();
        g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockShader;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
        return true;
    };

    if (!renderIntoFramebuffer())
        return false;

    const int cropX = clampedIntFromDouble(renderCropBox.x);
    const int cropY = clampedIntFromDouble(renderCropBox.y);
    auto      readback = readRgbaFramebufferRegion(framebuffer, 0, 0, framebufferWidth, framebufferHeight);
    if (readback.pixels.empty())
        return false;

    PixelBounds bounds;
    if (findAlphaBounds(readback, bounds))
        readback = cropReadback(readback, bounds);
    else
        readback = readRgbaFramebufferRegion(framebuffer, cropX, cropY, width, height);
    if (readback.pixels.empty())
        return false;

    width = readback.width;
    height = readback.height;
    artifactBox = CBox{fullBox.x + (readback.cropX - cropX) / scale, fullBox.y + (readback.cropTopY - cropY) / scale, width / scale, height / scale};

    repairTopTransparentSeam(readback);
    unpremultiplyAlpha(readback);

    return writeRgbaFile(path, readback.pixels);
}

void* findFunctionByDemangledName(const std::string& lookupName, const std::string& demangledNeedle) {
    const auto matches = HyprlandAPI::findFunctionsByName(g_pluginHandle, lookupName);
    const auto it = std::find_if(matches.begin(), matches.end(), [&](const SFunctionMatch& match) {
        return match.demangled.find(demangledNeedle) != std::string::npos;
    });

    if (it != matches.end())
        return it->address;
    return matches.empty() ? nullptr : matches.front().address;
}

void* findRenderWindowFunction() {
    return findFunctionByDemangledName("renderWindow", "CHyprRenderer::renderWindow(");
}

void* findRenderTextureInternalFunction() {
    return findFunctionByDemangledName("renderTextureInternal", "CHyprOpenGLImpl::renderTextureInternal(");
}

void* findRenderTextureWithBlurInternalFunction() {
    return findFunctionByDemangledName("renderTextureWithBlurInternal", "CHyprOpenGLImpl::renderTextureWithBlurInternal(");
}

void renderRealBackgroundArtifactsForMonitor(const PHLMONITOR& monitor,
                                             const Time::steady_tp& frozenTime,
                                             const std::vector<PendingRealBackgroundCapture*>& requests,
                                             CaptureSession& session) {
    if (!monitor || !monitor->m_activeWorkspace || requests.empty() || !g_pHyprRenderer || !g_pHyprOpenGL)
        return;

    const int framebufferWidth = positiveRoundedIntFromDouble(monitor->m_pixelSize.x);
    const int framebufferHeight = positiveRoundedIntFromDouble(monitor->m_pixelSize.y);
    std::size_t framebufferBytes = 0;
    if (!checkedRgbaByteSize(framebufferWidth, framebufferHeight, framebufferBytes))
        return;
    const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;

    std::vector<RealBackgroundCaptureState> states;
    states.reserve(requests.size());
    for (const auto* request : requests) {
        if (!request || !request->window)
            continue;

        CBox cropBox = request->artifactBox.copy().translate(-monitor->m_position).scale(scale).round();
        RealBackgroundCaptureState state;
        state.window = request->window;
        state.cropX = clampedIntFromDouble(cropBox.x);
        state.cropY = clampedIntFromDouble(cropBox.y);
        state.width = positiveIntFromDouble(cropBox.w);
        state.height = positiveIntFromDouble(cropBox.h);
        states.push_back(std::move(state));
    }
    if (states.empty())
        return;

    CFramebuffer framebuffer;
    const auto drmFormat = monitor->m_output && monitor->m_output->state ? monitor->m_output->state->state().drmFormat : DRM_FORMAT_ABGR8888;
    if (!framebuffer.alloc(framebufferWidth, framebufferHeight, DRM_FORMAT_ABGR8888) && !framebuffer.alloc(framebufferWidth, framebufferHeight, drmFormat))
        return;

    void* renderWindowSource = findRenderWindowFunction();
    if (!renderWindowSource)
        return;

    void* renderTextureInternalSource = findRenderTextureInternalFunction();
    void* renderTextureWithBlurInternalSource = findRenderTextureWithBlurInternalFunction();

    const auto removeHook = [](CFunctionHook* hook) {
        if (!hook)
            return;
        hook->unhook();
        HyprlandAPI::removeFunctionHook(g_pluginHandle, hook);
    };

    CFunctionHook* renderWindowHook = HyprlandAPI::createFunctionHook(g_pluginHandle, renderWindowSource, reinterpret_cast<void*>(&hkRenderWindow));
    if (!renderWindowHook)
        return;

    if (!renderWindowHook->hook()) {
        removeHook(renderWindowHook);
        return;
    }

    g_renderWindowOriginal = reinterpret_cast<RenderWindowFn>(renderWindowHook->m_original);
    if (!g_renderWindowOriginal) {
        removeHook(renderWindowHook);
        return;
    }

    CFunctionHook* renderTextureInternalHook = nullptr;
    CFunctionHook* renderTextureWithBlurInternalHook = nullptr;
    if (renderTextureInternalSource && renderTextureWithBlurInternalSource) {
        renderTextureInternalHook =
            HyprlandAPI::createFunctionHook(g_pluginHandle, renderTextureInternalSource, reinterpret_cast<void*>(&hkRenderTextureInternal));
        renderTextureWithBlurInternalHook =
            HyprlandAPI::createFunctionHook(g_pluginHandle, renderTextureWithBlurInternalSource, reinterpret_cast<void*>(&hkRenderTextureWithBlurInternal));

        const bool textureHooksReady = renderTextureInternalHook && renderTextureWithBlurInternalHook && renderTextureInternalHook->hook() &&
            renderTextureWithBlurInternalHook->hook();

        if (textureHooksReady) {
            g_renderTextureInternalOriginal = reinterpret_cast<RenderTextureInternalFn>(renderTextureInternalHook->m_original);
            g_renderTextureWithBlurInternalOriginal =
                reinterpret_cast<RenderTextureWithBlurInternalFn>(renderTextureWithBlurInternalHook->m_original);
        }

        if (!textureHooksReady || !g_renderTextureInternalOriginal || !g_renderTextureWithBlurInternalOriginal) {
            g_renderTextureInternalOriginal = nullptr;
            g_renderTextureWithBlurInternalOriginal = nullptr;
            removeHook(renderTextureWithBlurInternalHook);
            removeHook(renderTextureInternalHook);
            renderTextureWithBlurInternalHook = nullptr;
            renderTextureInternalHook = nullptr;
        }
    }

    RealBackgroundRenderHookContext hookContext{.monitor = monitor, .states = &states};
    g_realBackgroundRenderHookContext = &hookContext;
    const auto cleanupHook = [&]() {
        g_realBackgroundRenderHookContext = nullptr;
        g_renderWindowOriginal = nullptr;
        g_renderTextureInternalOriginal = nullptr;
        g_renderTextureWithBlurInternalOriginal = nullptr;
        removeHook(renderTextureWithBlurInternalHook);
        removeHook(renderTextureInternalHook);
        removeHook(renderWindowHook);
    };

    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprOpenGL->m_renderData.blockScreenShader;
    CRegion fakeDamage{0, 0, framebufferWidth, framebufferHeight};

    g_pHyprRenderer->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &framebuffer)) {
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
        cleanupHook();
        return;
    }

    g_pHyprOpenGL->clear(CHyprColor{0.0, 0.0, 0.0, 1.0});
    g_pHyprRenderer->renderWorkspace(monitor, monitor->m_activeWorkspace, frozenTime, CBox{0, 0, static_cast<double>(framebufferWidth), static_cast<double>(framebufferHeight)});
    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
    cleanupHook();
    g_pHyprRenderer->m_renderPass.clear();
    g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockShader;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;

    for (std::size_t i = 0; i < states.size(); ++i) {
        auto& state = states[i];
        if (!state.captured || state.readback.pixels.empty() || i >= requests.size())
            continue;

        auto* request = requests[i];
        if (!request || request->windowIndex >= session.windows.size())
            continue;

        if (!writeRgbaFile(request->path, state.readback.pixels))
            continue;

        auto& info = session.windows[request->windowIndex];
        info.realBackgroundPath = request->path.string();
        info.realBackgroundWidth = state.readback.width;
        info.realBackgroundHeight = state.readback.height;
        info.realBackgroundTopDown = true;
    }
}

bool shouldCaptureWindow(const PHLWINDOW& window) {
    if (!window || !window->m_isMapped || window->isHidden() || !window->m_workspace)
        return false;

    if (g_pHyprRenderer)
        return g_pHyprRenderer->shouldRenderWindow(window);

    return window->m_pinned || window->m_workspace->isVisible();
}

std::vector<PHLWINDOW> windowsInRenderOrder() {
    std::vector<PHLWINDOW> ordered;
    if (!g_pCompositor)
        return ordered;

    ordered.reserve(g_pCompositor->m_windows.size());
    const auto appendPass = [&](bool special, bool floating) {
        for (const auto& window : g_pCompositor->m_windows) {
            if (!shouldCaptureWindow(window) || (window->m_pinned && window->m_isFloating) || window->onSpecialWorkspace() != special ||
                window->m_isFloating != floating)
                continue;

            ordered.push_back(window);
        }
    };

    appendPass(false, false);
    appendPass(false, true);
    appendPass(true, false);
    appendPass(true, true);

    for (const auto& window : g_pCompositor->m_windows) {
        if (!shouldCaptureWindow(window) || !window->m_pinned || !window->m_isFloating)
            continue;

        ordered.push_back(window);
    }

    return ordered;
}

} // namespace

CaptureSession captureCompositorArtifacts(const CaptureDefaults& defaults) {
    CaptureSession session;
    session.id = makeSessionId();
    session.defaults = defaults;
    if (g_pInputManager) {
        const auto cursor = g_pInputManager->getMouseCoordsInternal();
        if (std::isfinite(cursor.x) && std::isfinite(cursor.y))
            session.cursorPosition = Point{.x = cursor.x, .y = cursor.y};
    }
    const auto root = artifactRoot(session.id);
    if (root.empty())
        return session;
    const auto frozenTime = Time::steadyNow();
    const bool renderDecorations = defaults.fushionMode || defaults.windowBorder == DecorationPolicy::Keep || defaults.windowShadow == DecorationPolicy::Keep;

    int monitorIndex = 0;
    for (const auto& monitor : g_pCompositor->m_monitors) {
        if (session.monitors.size() >= MAX_SESSION_MONITORS)
            break;
        if (!monitor)
            continue;
        MonitorInfo info;
        info.name = monitor->m_name;
        info.logicalGeometry = monitorRect(monitor);
        info.scale = monitor->m_scale;
        info.transform = static_cast<int>(monitor->m_transform);
        const auto path = root / ("monitor-" + std::to_string(monitorIndex++) + ".rgba");
        if (renderMonitorArtifact(monitor, frozenTime, path, info.artifactWidth, info.artifactHeight))
            info.artifactPath = path.string();
        session.monitors.push_back(std::move(info));
    }

    int z = 0;
    std::vector<PendingRealBackgroundCapture> pendingRealBackgrounds;
    for (const auto& window : windowsInRenderOrder()) {
        if (session.windows.size() >= MAX_SESSION_WINDOWS)
            break;
        const CBox fullBox = renderedWindowBox(window, window->getFullWindowBoundingBox());
        const Rect full = toRect(fullBox);
        auto monitor = window->m_monitor.lock();
        if (!monitor)
            continue;

        bool visible = false;
        for (const auto& mon : session.monitors)
            visible = visible || intersects(full, mon.logicalGeometry);
        if (!visible)
            continue;

        WindowInfo info;
        info.address = "0x" + pointerId(window.get());
        info.title = boundedString(window->m_title, MAX_WINDOW_METADATA_BYTES);
        info.appClass = boundedString(window->m_class, MAX_WINDOW_METADATA_BYTES);
        info.zIndex = z++;
        const bool dontRound = window->isEffectiveInternalFSMode(FSMODE_FULLSCREEN);
        info.rounding = dontRound ? 0.0 : std::max(0.0F, window->rounding());
        info.roundingPower = dontRound ? 2.0 : std::clamp(static_cast<double>(window->roundingPower()), 1.0, 10.0);
        info.borderSize = dontRound || window->m_X11DoesntWantBorders ? 0.0 : std::max(0, window->getRealBorderSize());
        const auto path = root / ("window-" + pointerId(window.get()) + ".rgba");
        CBox artifactBox;
        const std::size_t windowIndex = session.windows.size();
        if (renderWindowArtifact(window, monitor, frozenTime, renderDecorations, path, info.artifactWidth, info.artifactHeight, artifactBox)) {
            info.artifactPath = path.string();
            info.fullGeometry = toRect(artifactBox);
            pendingRealBackgrounds.push_back({
                .window = window,
                .monitor = monitor,
                .artifactBox = artifactBox,
                .path = root / ("window-real-" + pointerId(window.get()) + ".rgba"),
                .windowIndex = windowIndex,
            });
        } else
            info.fullGeometry = full;
        info.visibleGeometry = toRect(renderedWindowGoalMainSurfaceBox(window));
        session.windows.push_back(std::move(info));
    }

    for (const auto& monitor : g_pCompositor->m_monitors) {
        if (!monitor)
            continue;

        std::vector<PendingRealBackgroundCapture*> monitorRequests;
        for (auto& request : pendingRealBackgrounds) {
            if (request.monitor && request.monitor.get() == monitor.get())
                monitorRequests.push_back(&request);
        }
        renderRealBackgroundArtifactsForMonitor(monitor, frozenTime, monitorRequests, session);
    }

    return session;
}

std::string writeCompositorSessionJsonFile(const CaptureSession& session, std::string_view json) {
    if (json.empty() || json.size() > MAX_SESSION_JSON_BYTES)
        return {};

    const auto root = artifactRoot(session.id);
    if (root.empty())
        return {};

    const auto path = root / "session.json";
    std::vector<unsigned char> bytes(json.begin(), json.end());
    if (!writeRgbaFile(path, bytes))
        return {};

    return path.string();
}

void cleanupCompositorArtifacts(const CaptureSession& session) {
    std::vector<std::filesystem::path> parents;
    const auto removeArtifact = [&](const std::string& rawPath) {
        if (rawPath.empty())
            return;

        const std::filesystem::path path(rawPath);
        std::error_code             ec;
        std::filesystem::remove(path, ec);
        rememberParent(parents, path);
    };

    for (const auto& monitor : session.monitors)
        removeArtifact(monitor.artifactPath);

    for (const auto& window : session.windows) {
        removeArtifact(window.artifactPath);
        removeArtifact(window.realBackgroundPath);
    }

    for (const auto& root : artifactRootCandidates(session.id)) {
        removeArtifact((root / "session.json").string());
        rememberParent(parents, root);
    }

    std::sort(parents.begin(), parents.end(), [](const auto& left, const auto& right) {
        return left.native().size() > right.native().size();
    });

    for (const auto& parent : parents) {
        std::error_code ec;
        std::filesystem::remove(parent, ec);
    }
}

} // namespace hyprcapture
