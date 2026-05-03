#include "plugin/artifact_capture.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/OpenGL.hpp>
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

namespace hyprshot {
namespace {

struct RgbaReadback {
    std::vector<unsigned char> pixels;
    int                        width = 0;
    int                        height = 0;
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

bool nonZeroPixel(const std::vector<unsigned char>& pixels, int width, int x, int y) {
    const auto i = (static_cast<std::size_t>(y) * width + x) * 4U;
    return pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0 || pixels[i + 3] != 0;
}

bool trimZeroMargins(RgbaReadback& readback, int& trimX, int& trimY) {
    trimX = 0;
    trimY = 0;
    if (readback.width <= 0 || readback.height <= 0 || readback.pixels.empty())
        return false;

    int minX = readback.width;
    int minY = readback.height;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < readback.height; ++y) {
        for (int x = 0; x < readback.width; ++x) {
            if (!nonZeroPixel(readback.pixels, readback.width, x, y))
                continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }

    if (maxX < minX || maxY < minY)
        return false;

    const int oldWidth = readback.width;
    const int oldHeight = readback.height;
    const int newWidth = maxX - minX + 1;
    const int newHeight = maxY - minY + 1;
    std::vector<unsigned char> trimmed(static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight) * 4U);
    for (int y = 0; y < newHeight; ++y) {
        const auto* src = readback.pixels.data() + (static_cast<std::size_t>(minY + y) * readback.width + minX) * 4U;
        auto*       dst = trimmed.data() + static_cast<std::size_t>(y) * newWidth * 4U;
        std::copy(src, src + static_cast<std::size_t>(newWidth) * 4U, dst);
    }

    trimX = minX;
    trimY = minY;
    readback.width = newWidth;
    readback.height = newHeight;
    readback.pixels = std::move(trimmed);
    return trimX != 0 || trimY != 0 || newWidth != oldWidth || newHeight != oldHeight;
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
                          int& trimX,
                          int& trimY) {
    if (!window || !monitor || !g_pHyprRenderer || !g_pHyprOpenGL)
        return false;
    trimX = 0;
    trimY = 0;

    const CBox fullBox = renderedWindowBox(window, window->getFullWindowBoundingBox());
    CBox cropBox = fullBox.copy().translate(-monitor->m_position).scale(monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale).round();
    width = std::max(1, static_cast<int>(cropBox.w));
    height = std::max(1, static_cast<int>(cropBox.h));
    const int framebufferWidth = std::max(1, static_cast<int>(std::lround(monitor->m_pixelSize.x)));
    const int framebufferHeight = std::max(1, static_cast<int>(std::lround(monitor->m_pixelSize.y)));

    CFramebuffer framebuffer;
    const auto drmFormat = monitor->m_output && monitor->m_output->state ? monitor->m_output->state->state().drmFormat : DRM_FORMAT_ABGR8888;
    if (!framebuffer.alloc(framebufferWidth, framebufferHeight, DRM_FORMAT_ABGR8888) && !framebuffer.alloc(framebufferWidth, framebufferHeight, drmFormat))
        return false;

    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprOpenGL->m_renderData.blockScreenShader;
    const bool previousRenderingSnapshot = g_pHyprRenderer->m_bRenderingSnapshot;

    CRegion fakeDamage{0, 0, framebufferWidth, framebufferHeight};
    g_pHyprRenderer->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &framebuffer)) {
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
        return false;
    }

    g_pHyprRenderer->m_bRenderingSnapshot = true;
    g_pHyprOpenGL->clear(CHyprColor{0.0, 0.0, 0.0, 0.0});
    g_pHyprRenderer->renderWindow(window, monitor, frozenTime, decorate, RENDER_PASS_ALL, false, false);
    g_pHyprRenderer->m_bRenderingSnapshot = previousRenderingSnapshot;

    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
    g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockShader;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;

    auto readback = readRgbaFramebufferRegion(framebuffer, static_cast<int>(cropBox.x), static_cast<int>(cropBox.y), width, height);
    if (readback.pixels.empty())
        return false;
    trimZeroMargins(readback, trimX, trimY);
    width = readback.width;
    height = readback.height;
    return writeRgbaFile(path, readback.pixels);
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
    for (const auto& window : g_pCompositor->m_windows) {
        if (!window || !window->m_isMapped || window->isHidden() || !window->m_workspace || !window->m_workspace->isVisible())
            continue;

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
        const auto path = root / ("window-" + pointerId(window.get()) + ".rgba");
        int trimX = 0;
        int trimY = 0;
        if (renderWindowArtifact(window, monitor, frozenTime, renderDecorations, path, info.artifactWidth, info.artifactHeight, trimX, trimY)) {
            info.artifactPath = path.string();
            const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;
            info.fullGeometry =
                toRect(CBox{fullBox.x + trimX / scale, fullBox.y + trimY / scale, info.artifactWidth / scale, info.artifactHeight / scale});
        } else
            info.fullGeometry = full;
        info.visibleGeometry = toRect(renderedWindowBox(window, window->getWindowMainSurfaceBox()));
        session.windows.push_back(std::move(info));
    }

    return session;
}

} // namespace hyprshot
