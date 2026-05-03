#pragma once

#include "shared/config.hpp"

#include <QImage>
#include <QMainWindow>
#include <QPoint>
#include <QRect>
#include <QString>
#include <vector>

class QButtonGroup;
class QLabel;
class InlineSelect;

class CaptureOverlay final : public QMainWindow {
    Q_OBJECT

  public:
    explicit CaptureOverlay(hyprshot::CaptureDefaults defaults, bool quick, QString sessionJson, QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    struct MonitorArtifact {
        QRect   logicalGeometry;
        QImage  image;
        QString name;
    };

    struct WindowArtifact {
        QRect   fullGeometry;
        QRect   visibleGeometry;
        QImage  image;
        QString title;
        QString appClass;
    };

    void buildToolbar();
    void parseSessionJson(const QString& json);
    void captureScreensBeforeOverlay();
    void setMode(hyprshot::CaptureMode mode);
    void finishCapture();
    void cancelCapture();
    void saveImage(const QImage& image);
    QImage renderResultImage() const;
    QRect normalizedSelection() const;
    QRect captureRectForMode() const;
    QRect globalToLocalRect(const QRect& rect) const;
    QRect localToDesktopSourceRect(const QRect& rect) const;
    QPoint cursorLogicalPosition() const;
    const WindowArtifact* hoveredWindow() const;
    bool windowCaptureAvailable() const;
    void updateStatus();
    void relayoutToolbar();
    void showThumbnail(const QImage& image, const QString& path);

    hyprshot::CaptureDefaults m_defaults;
    hyprshot::CaptureMode     m_mode;
    bool                      m_quick = false;
    bool                      m_dragging = false;
    QPoint                    m_dragStart;
    QPoint                    m_dragEnd;

    QWidget*     m_toolbar = nullptr;
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
