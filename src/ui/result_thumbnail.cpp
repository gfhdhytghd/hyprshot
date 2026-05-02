#include "ui/result_thumbnail.hpp"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>

ResultThumbnail::ResultThumbnail(const QPixmap& pixmap, QString path, int timeoutMs, QWidget* parent) : QLabel(parent), m_path(std::move(path)) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setPixmap(pixmap.scaled(180, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    setStyleSheet("QLabel { padding: 6px; background: rgba(20, 24, 28, 190); border: 1px solid rgba(255,255,255,90); border-radius: 8px; }");
    adjustSize();

    if (timeoutMs > 0) {
        m_closeTimer.setSingleShot(true);
        connect(&m_closeTimer, &QTimer::timeout, this, &QWidget::close);
        m_closeTimer.start(timeoutMs);
    }
}

void ResultThumbnail::mousePressEvent(QMouseEvent* event) {
    m_dragStart = event->globalPosition().toPoint() - frameGeometry().topLeft();
}

void ResultThumbnail::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton)
        move(event->globalPosition().toPoint() - m_dragStart);
}

void ResultThumbnail::mouseDoubleClickEvent(QMouseEvent*) {
    if (!m_path.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_path));
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
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_path));
    } else if (chosen == copyAction) {
        const auto currentPixmap = pixmap();
        if (!currentPixmap.isNull())
            QGuiApplication::clipboard()->setPixmap(currentPixmap);
    } else if (chosen == showAction && !m_path.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_path).absolutePath()));
    } else if (chosen == closeAction) {
        close();
    }
}

void ResultThumbnail::enterEvent(QEnterEvent*) {
    m_closeTimer.stop();
}

void ResultThumbnail::leaveEvent(QEvent*) {
    if (m_closeTimer.interval() > 0)
        m_closeTimer.start();
}
