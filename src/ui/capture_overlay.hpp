#pragma once

#include "shared/config.hpp"

#include <QImage>
#include <QMainWindow>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QString>
#include <functional>
#include <vector>

class QButtonGroup;
class QGraphicsOpacityEffect;
class QLabel;
class QPainter;
class QResizeEvent;
class QShowEvent;
class QPropertyAnimation;
class InlineSelect;

namespace hyprcapture::ui {
struct ClipboardSnapshotData;
}

class CaptureOverlay final : public QMainWindow {
    Q_OBJECT
    Q_PROPERTY(double overlayOpacity READ overlayOpacity WRITE setOverlayOpacity)

  public:
    explicit CaptureOverlay(hyprcapture::CaptureDefaults defaults, bool quick, bool record, QString sessionJson, QWidget* parent = nullptr);

  protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct MonitorArtifact {
        QRect   logicalGeometry;
        QImage  image;
        QString name;
    };

    struct WindowArtifact {
        QRect   fullGeometry;
        QRect   visibleGeometry;
        double  rounding = 0.0;
        double  roundingPower = 2.0;
        double  borderSize = 0.0;
        QImage  image;
        QImage  realBackground;
        QString address;
        QString title;
        QString appClass;
    };

    void buildToolbar();
    void parseSessionJson(const QString& json);
    void captureScreensBeforeOverlay();
    void setMode(hyprcapture::CaptureMode mode);
    void updateToolbarControlsForMode();
    void finishCapture();
    void cancelCapture();
    bool startRecording();
    void saveImage(const QImage& image, hyprcapture::ui::ClipboardSnapshotData clipboardSnapshot);
    QImage renderResultImage() const;
    QImage renderDesktopRectAtDisplayResolution(const QRect& globalRect) const;
    void paintDesktop(QPainter& painter, const QRect& target) const;
    QRect normalizedSelection() const;
    QRect captureRectForMode() const;
    QRect fullscreenCaptureRect() const;
    QRect regionCaptureBounds() const;
    QRect localScreenRectAt(const QPoint& localPos) const;
    QPoint clampedToRect(const QPoint& point, const QRect& bounds) const;
    QRect globalToLocalRect(const QRect& rect) const;
    QRect desktopSourceRectForGlobalRect(const QRect& rect) const;
    QRect localToDesktopSourceRect(const QRect& rect) const;
    QPoint cursorLogicalPosition() const;
    void rememberCursorPosition(const QPointF& globalPosition);
    void refreshInitialCursorPosition();
    int monitorCount() const;
    bool hasMultipleMonitors() const;
    void hideOptionPopups();
    hyprcapture::FullscreenScope currentFullscreenScope() const;
    hyprcapture::WindowBackground currentWindowBackground() const;
    hyprcapture::DecorationPolicy currentWindowBorder() const;
    hyprcapture::DecorationPolicy currentWindowShadow() const;
    QRect windowFrameGeometry(const WindowArtifact& window) const;
    double windowFrameRadius(const WindowArtifact& window) const;
    const WindowArtifact* hoveredWindow() const;
    bool windowCaptureAvailable() const;
    void updateStatus();
    void relayoutToolbar();
    void showThumbnail(const QImage& image, const QString& path, const QString& restoreClipboardPath);
    double overlayOpacity() const;
    void setOverlayOpacity(double opacity);
    void startFadeIn();
    void fadeOutThen(std::function<void()> finished);
    void runOverlayFade(double start, double end, std::function<void()> finished);

    hyprcapture::CaptureDefaults m_defaults;
    hyprcapture::CaptureMode     m_mode;
    bool                      m_quick = false;
    bool                      m_record = false;
    bool                      m_dragging = false;
    bool                      m_finishing = false;
    bool                      m_fadeOutStarted = false;
    double                    m_overlayOpacity = 0.0;
    QPoint                    m_dragStart;
    QPoint                    m_dragEnd;
    QPoint                    m_cursorLogicalPosition;
    bool                      m_hasCursorLogicalPosition = false;

    QWidget*     m_toolbar = nullptr;
    QGraphicsOpacityEffect* m_toolbarOpacity = nullptr;
    QPropertyAnimation* m_fadeAnimation = nullptr;
    InlineSelect* m_fullscreenScope = nullptr;
    InlineSelect* m_windowBackground = nullptr;
    QLabel*      m_status = nullptr;
    QImage       m_desktopImage;
    QRect        m_desktopGeometry;
    int          m_sessionMonitorCount = 0;
    int          m_sessionWindowCount = 0;
    std::vector<MonitorArtifact> m_monitorArtifacts;
    std::vector<WindowArtifact>  m_windowArtifacts;
};
