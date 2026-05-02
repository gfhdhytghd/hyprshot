#include "ui/result_thumbnail.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QGuiApplication>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStyleHints>

#include <algorithm>

ResultThumbnail::ResultThumbnail(const QPixmap& pixmap, QString path, int timeoutMs, QWidget* parent) : QLabel(parent), m_path(std::move(path)) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_StyledBackground);
    setPixmap(pixmap.scaled(180, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    const auto palette = QApplication::palette();
    const QColor bg = palette.color(QPalette::Window);
    const QColor fg = palette.color(QPalette::WindowText);
    setStyleSheet(QStringLiteral("QLabel { padding: 6px; background: rgba(%1,%2,%3,220); border: 1px solid rgba(%4,%5,%6,95); border-radius: 8px; }")
                      .arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(fg.red()).arg(fg.green()).arg(fg.blue()));
    adjustSize();
    winId();
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle())) {
        layerWindow->setScope("hyprshot-thumbnail");
        layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        layerWindow->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorRight} | LayerShellQt::Window::AnchorBottom);
        layerWindow->setMargins(QMargins(0, 0, 24, 24));
        layerWindow->setExclusiveZone(0);
        layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layerWindow->setActivateOnShow(false);
        layerWindow->setDesiredSize(size());
    }

    if (timeoutMs > 0) {
        m_closeTimer.setSingleShot(true);
        connect(&m_closeTimer, &QTimer::timeout, this, &QWidget::close);
        m_closeTimer.start(timeoutMs);
    }
}

void ResultThumbnail::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;

    m_dragMoved = false;
    m_pressGlobal = event->globalPosition().toPoint();
    m_dragStart = event->globalPosition().toPoint() - frameGeometry().topLeft();
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle()))
        m_dragMargins = layerWindow->margins();
    else
        m_dragMargins = {};
}

void ResultThumbnail::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton))
        return;

    const QPoint current = event->globalPosition().toPoint();
    const QPoint delta = current - m_pressGlobal;
    if (delta.manhattanLength() > QGuiApplication::styleHints()->startDragDistance())
        m_dragMoved = true;

    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle())) {
        layerWindow->setMargins(QMargins(0,
                                         0,
                                         std::max(0, m_dragMargins.right() - delta.x()),
                                         std::max(0, m_dragMargins.bottom() - delta.y())));
    } else {
        move(event->globalPosition().toPoint() - m_dragStart);
    }
}

void ResultThumbnail::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !m_dragMoved && !m_path.isEmpty() && openPath(m_path))
        close();
}

void ResultThumbnail::contextMenuEvent(QContextMenuEvent* event) {
    m_closeTimer.stop();
    QMenu menu(this);
    const auto openAction = menu.addAction("Open");
    const auto copyAction = menu.addAction("Copy image");
    const auto showAction = menu.addAction("Show in folder");
    menu.addSeparator();
    const auto closeAction = menu.addAction("Close");

    const auto chosen = menu.exec(event->globalPos());
    if (chosen == openAction && !m_path.isEmpty()) {
        openPath(m_path);
    } else if (chosen == copyAction) {
        const auto currentPixmap = pixmap();
        if (!currentPixmap.isNull())
            QGuiApplication::clipboard()->setPixmap(currentPixmap);
    } else if (chosen == showAction && !m_path.isEmpty()) {
        openPath(QFileInfo(m_path).absolutePath());
    } else if (chosen == closeAction) {
        close();
    }
}

bool ResultThumbnail::openPath(const QString& path) {
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("QT_WAYLAND_SHELL_INTEGRATION");
    process.setProcessEnvironment(environment);
    process.setProgram("xdg-open");
    process.setArguments({path});
    return process.startDetached();
}

void ResultThumbnail::enterEvent(QEnterEvent*) {
    m_closeTimer.stop();
}

void ResultThumbnail::leaveEvent(QEvent*) {
    if (m_closeTimer.interval() > 0)
        m_closeTimer.start();
}
