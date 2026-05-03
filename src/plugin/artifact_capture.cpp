#include "plugin/artifact_capture.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef private

#include <GLES2/gl2.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

extern HANDLE g_pluginHandle;

namespace hyprshot {
namespace {

struct RgbaReadback {
    std::vector<unsigned char> pixels;
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

Rect toRect(const CBox& box) {
    return {.x = box.x, .y = box.y, .width = box.w, .height = box.h};
}

Rect monitorRect(const PHLMONITOR& monitor) {
    return {.x = monitor->m_position.x, .y = monitor->m_position.y, .width = monitor->m_size.x, .height = monitor->m_size.y};
}

bool intersects(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
}

std::filesystem::path artifactRoot(const std::string& sessionId) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    auto root = std::filesystem::path(runtime && *runtime ? runtime : "/tmp") / "hyprshot" / sessionId;
    std::filesystem::create_directories(root);
    return root;
}

bool writeRgbaFile(const std::filesystem::path& path, const std::vector<unsigned char>& pixels) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return out.good();
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
    cropped.width = bounds.width;
    cropped.height = bounds.height;
    cropped.pixels.assign(static_cast<std::size_t>(bounds.width) * static_cast<std::size_t>(bounds.height) * 4U, 0);

    const std::size_t rowBytes = static_cast<std::size_t>(bounds.width) * 4U;
    for (int y = 0; y < bounds.height; ++y) {
        const auto* src = readback.pixels.data() + (static_cast<std::size_t>(bounds.y + y) * readback.width + bounds.x) * 4U;
        auto*       dst = cropped.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        std::copy(src, src + rowBytes, dst);
    }

    return cropped;
}

RgbaReadback readRgbaFramebufferRegion(CFramebuffer& framebuffer, int cropX, int cropTopY, int cropWidth, int cropHeight) {
    const int framebufferWidth = static_cast<int>(std::lround(framebuffer.m_size.x));
    const int framebufferHeight = static_cast<int>(std::lround(framebuffer.m_size.y));
    if (framebufferWidth <= 0 || framebufferHeight <= 0 || cropWidth <= 0 || cropHeight <= 0)
        return {};

    RgbaReadback readback;
    readback.width = cropWidth;
    readback.height = cropHeight;
    readback.pixels.assign(static_cast<std::size_t>(cropWidth) * static_cast<std::size_t>(cropHeight) * 4U, 0);
    const int srcX = std::clamp(cropX, 0, framebufferWidth);
    const int srcTopY = std::clamp(cropTopY, 0, framebufferHeight);
    const int srcRight = std::clamp(cropX + cropWidth, 0, framebufferWidth);
    const int srcBottom = std::clamp(cropTopY + cropHeight, 0, framebufferHeight);
    const int srcWidth = srcRight - srcX;
    const int srcHeight = srcBottom - srcTopY;

    if (srcWidth > 0 && srcHeight > 0) {
        std::vector<unsigned char> rows(static_cast<std::size_t>(srcWidth) * static_cast<std::size_t>(srcHeight) * 4U);
        GLint previousReadFramebuffer = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer.getFBID());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(srcX, framebufferHeight - srcTopY - srcHeight, srcWidth, srcHeight, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);

        const int dstX = srcX - cropX;
        const int dstY = srcTopY - cropTopY;
        for (int y = 0; y < srcHeight; ++y) {
            const auto* src = rows.data() + static_cast<std::size_t>(y) * srcWidth * 4U;
            auto*       dst = readback.pixels.data() + (static_cast<std::size_t>(dstY + y) * cropWidth + dstX) * 4U;
            std::copy(src, src + static_cast<std::size_t>(srcWidth) * 4U, dst);
        }
    }

    return readback;
}

class RealBackgroundCapturePass final : public IPassElement {
  public:
    explicit RealBackgroundCapturePass(RealBackgroundCaptureState* state) : m_state(state) {}

    void draw(const CRegion&) override {
        if (!m_state || m_state->captured || !g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.currentFB)
            return;

        m_state->readback = readRgbaFramebufferRegion(*g_pHyprOpenGL->m_renderData.currentFB, m_state->cropX, m_state->cropY, m_state->width, m_state->height);
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
    auto readback = readRgbaFramebufferRegion(*g_pHyprOpenGL->m_renderData.currentFB, state->cropX, state->cropY, state->width, state->height);
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
                                      static_cast<int>(std::lround(framebuffer.m_size.x)),
                                      static_cast<int>(std::lround(framebuffer.m_size.y)));
}

CBox renderedWindowBox(const PHLWINDOW& window, CBox box) {
    if (window->m_workspace && !window->m_pinned)
        box.translate(window->m_workspace->m_renderOffset->value());
    box.translate(window->m_floatingOffset);
    return box;
}

std::string pointerId(const void* ptr) {
    std::ostringstream out;
    out << std::hex << reinterpret_cast<std::uintptr_t>(ptr);
    return out.str();
}

bool renderMonitorArtifact(const PHLMONITOR& monitor, const Time::steady_tp& frozenTime, const std::filesystem::path& path, int& width, int& height) {
    if (!monitor || !monitor->m_activeWorkspace || !g_pHyprRenderer || !g_pHyprOpenGL)
        return false;

    width = std::max(1, static_cast<int>(std::lround(monitor->m_pixelSize.x)));
    height = std::max(1, static_cast<int>(std::lround(monitor->m_pixelSize.y)));

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

    const CBox fullBox = renderedWindowBox(window, window->getFullWindowBoundingBox());
    CBox cropBox = fullBox.copy().translate(-monitor->m_position).scale(monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale).round();
    width = std::max(1, static_cast<int>(cropBox.w));
    height = std::max(1, static_cast<int>(cropBox.h));
    const int framebufferWidth = std::max(1, static_cast<int>(std::lround(monitor->m_pixelSize.x)));
    const int framebufferHeight = std::max(1, static_cast<int>(std::lround(monitor->m_pixelSize.y)));
    const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;

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

    const int cropX = static_cast<int>(cropBox.x);
    const int cropY = static_cast<int>(cropBox.y);
    const int expandedCropX = cropX - WINDOW_ARTIFACT_CROP_PADDING;
    const int expandedCropY = cropY - WINDOW_ARTIFACT_CROP_PADDING;
    auto      readback = readRgbaFramebufferRegion(framebuffer,
                                                   expandedCropX,
                                                   expandedCropY,
                                                   width + WINDOW_ARTIFACT_CROP_PADDING * 2,
                                                   height + WINDOW_ARTIFACT_CROP_PADDING * 2);
    if (readback.pixels.empty())
        return false;

    int actualCropX = cropX;
    int actualCropY = cropY;
    PixelBounds bounds;
    if (findAlphaBounds(readback, bounds)) {
        readback = cropReadback(readback, bounds);
        actualCropX = expandedCropX + bounds.x;
        actualCropY = expandedCropY + bounds.y;
    } else
        readback = readRgbaFramebufferRegion(framebuffer, cropX, cropY, width, height);

    width = readback.width;
    height = readback.height;
    artifactBox = CBox{monitor->m_position.x + actualCropX / scale, monitor->m_position.y + actualCropY / scale, width / scale, height / scale};

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

    const int framebufferWidth = std::max(1, static_cast<int>(std::lround(monitor->m_pixelSize.x)));
    const int framebufferHeight = std::max(1, static_cast<int>(std::lround(monitor->m_pixelSize.y)));
    const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;

    std::vector<RealBackgroundCaptureState> states;
    states.reserve(requests.size());
    for (const auto* request : requests) {
        if (!request || !request->window)
            continue;

        CBox cropBox = request->artifactBox.copy().translate(-monitor->m_position).scale(scale).round();
        RealBackgroundCaptureState state;
        state.window = request->window;
        state.cropX = static_cast<int>(cropBox.x);
        state.cropY = static_cast<int>(cropBox.y);
        state.width = std::max(1, static_cast<int>(cropBox.w));
        state.height = std::max(1, static_cast<int>(cropBox.h));
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
    const auto root = artifactRoot(session.id);
    const auto frozenTime = Time::steadyNow();
    const bool renderDecorations = defaults.windowBorder == DecorationPolicy::Keep || defaults.windowShadow == DecorationPolicy::Keep;

    int monitorIndex = 0;
    for (const auto& monitor : g_pCompositor->m_monitors) {
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
        info.title = window->m_title;
        info.appClass = window->m_class;
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
        info.visibleGeometry = toRect(renderedWindowBox(window, window->getWindowMainSurfaceBox()));
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

} // namespace hyprshot
