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
#include <vector>

namespace hyprshot {
namespace {

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

bool writeRgbaFramebuffer(CFramebuffer& framebuffer, const std::filesystem::path& path, bool flipY) {
    const int width = static_cast<int>(std::lround(framebuffer.m_size.x));
    const int height = static_cast<int>(std::lround(framebuffer.m_size.y));
    if (width <= 0 || height <= 0)
        return false;

    std::vector<unsigned char> bottomUp(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    std::vector<unsigned char> topDown(bottomUp.size());

    GLint previousReadFramebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer.getFBID());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);

    if (flipY) {
        for (int y = 0; y < height; ++y) {
            const auto* src = bottomUp.data() + static_cast<std::size_t>(height - 1 - y) * width * 4U;
            auto* dst = topDown.data() + static_cast<std::size_t>(y) * width * 4U;
            std::copy(src, src + static_cast<std::size_t>(width) * 4U, dst);
        }
    } else {
        topDown = std::move(bottomUp);
    }

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(topDown.data()), static_cast<std::streamsize>(topDown.size()));
    return out.good();
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

    return writeRgbaFramebuffer(framebuffer, path, true);
}

bool renderWindowArtifact(const PHLWINDOW& window,
                          const PHLMONITOR& monitor,
                          const Time::steady_tp& frozenTime,
                          bool decorate,
                          const std::filesystem::path& path,
                          int& width,
                          int& height) {
    if (!window || !monitor || !g_pHyprRenderer || !g_pHyprOpenGL)
        return false;

    const CBox fullBox = window->getFullWindowBoundingBox();
    const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;
    width = std::max(1, static_cast<int>(std::ceil(fullBox.w * scale)));
    height = std::max(1, static_cast<int>(std::ceil(fullBox.h * scale)));

    CFramebuffer framebuffer;
    const auto drmFormat = monitor->m_output && monitor->m_output->state ? monitor->m_output->state->state().drmFormat : DRM_FORMAT_ABGR8888;
    if (!framebuffer.alloc(width, height, DRM_FORMAT_ABGR8888) && !framebuffer.alloc(width, height, drmFormat))
        return false;

    const auto previousRealPos = window->m_realPosition->value();
    const auto previousRealSize = window->m_realSize->value();
    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprOpenGL->m_renderData.blockScreenShader;

    CRegion fakeDamage{0, 0, width, height};
    g_pHyprRenderer->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &framebuffer)) {
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
        return false;
    }

    g_pHyprOpenGL->clear(CHyprColor{0.0, 0.0, 0.0, 0.0});

    window->m_realPosition->setValueAndWarp(Vector2D{-fullBox.x, -fullBox.y});
    window->m_realSize->setValueAndWarp(previousRealSize);
    g_pHyprRenderer->renderWindow(window, monitor, frozenTime, decorate, RENDER_PASS_ALL, false, false);
    window->m_realPosition->setValueAndWarp(previousRealPos);
    window->m_realSize->setValueAndWarp(previousRealSize);

    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
    g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockShader;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;

    return writeRgbaFramebuffer(framebuffer, path, false);
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

        const CBox fullBox = window->getFullWindowBoundingBox();
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
        info.visibleGeometry = toRect(window->getWindowMainSurfaceBox());
        info.fullGeometry = full;
        info.zIndex = z++;
        const auto path = root / ("window-" + pointerId(window.get()) + ".rgba");
        if (renderWindowArtifact(window, monitor, frozenTime, renderDecorations, path, info.artifactWidth, info.artifactHeight))
            info.artifactPath = path.string();
        session.windows.push_back(std::move(info));
    }

    return session;
}

} // namespace hyprshot
