#pragma once

#include "shared/config.hpp"

#include <QImage>
#include <QMainWindow>
#include <QPoint>
#include <QRect>
#include <QString>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;

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
    const WindowArtifact* hoveredWindow() const;
    void showThumbnail(const QImage& image, const QString& path);

    hyprshot::CaptureDefaults m_defaults;
    hyprshot::CaptureMode     m_mode;
    bool                      m_quick = false;
    bool                      m_dragging = false;
    QPoint                    m_dragStart;
    QPoint                    m_dragEnd;

    QWidget*     m_toolbar = nullptr;
    QComboBox*   m_fullscreenScope = nullptr;
    QComboBox*   m_windowBackground = nullptr;
    QCheckBox*   m_save = nullptr;
    QCheckBox*   m_clipboard = nullptr;
    QCheckBox*   m_thumbnail = nullptr;
    QLabel*      m_status = nullptr;
    QImage       m_desktopImage;
    QRect        m_desktopGeometry;
    std::vector<MonitorArtifact> m_monitorArtifacts;
    std::vector<WindowArtifact>  m_windowArtifacts;
};
