#include "ui/capture_overlay.hpp"

#include "ui/result_thumbnail.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>

#include <cstdlib>

namespace {

QString qString(const std::string& value) {
    return QString::fromStdString(value);
}

QColor followSystemColor() {
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    return scheme == Qt::ColorScheme::Light ? QColor(245, 245, 245) : QColor(17, 19, 23);
}

} // namespace

CaptureOverlay::CaptureOverlay(hyprshot::CaptureDefaults defaults, bool quick, QWidget* parent)
    : QMainWindow(parent), m_defaults(std::move(defaults)), m_mode(m_defaults.mode), m_quick(quick) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);

    captureScreensBeforeOverlay();
    setGeometry(m_desktopGeometry.isValid() ? m_desktopGeometry : QRect(0, 0, 1280, 720));

    buildToolbar();
    if (m_quick)
        QTimer::singleShot(0, this, &CaptureOverlay::finishCapture);
}

void CaptureOverlay::captureScreensBeforeOverlay() {
    const auto screens = QGuiApplication::screens();
    for (const auto* screen : screens)
        m_desktopGeometry = m_desktopGeometry.united(screen->geometry());

    if (!m_desktopGeometry.isValid())
        return;

    m_desktopImage = QImage(m_desktopGeometry.size(), QImage::Format_ARGB32_Premultiplied);
    m_desktopImage.fill(QColor(30, 34, 38));

    QPainter painter(&m_desktopImage);
    for (auto* screen : screens) {
        const QPixmap pixmap = screen->grabWindow(0);
        if (pixmap.isNull())
            continue;
        const QPoint target = screen->geometry().topLeft() - m_desktopGeometry.topLeft();
        painter.drawPixmap(target, pixmap);
    }
}

void CaptureOverlay::buildToolbar() {
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName("toolbar");
    m_toolbar->setStyleSheet(
        "#toolbar { background: rgba(26, 29, 33, 230); border: 1px solid rgba(255,255,255,70); border-radius: 8px; }"
        "QPushButton { color: white; padding: 6px 10px; border: none; border-radius: 5px; }"
        "QPushButton:checked { background: rgba(255,255,255,45); }"
        "QComboBox, QCheckBox, QLabel { color: white; }");

    auto* layout = new QHBoxLayout(m_toolbar);
    layout->setContentsMargins(10, 7, 10, 7);

    auto* group = new QButtonGroup(this);
    const auto addMode = [&](const QString& label, hyprshot::CaptureMode mode) {
        auto* button = new QPushButton(label, m_toolbar);
        button->setCheckable(true);
        button->setChecked(mode == m_mode);
        group->addButton(button);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, mode] { setMode(mode); });
    };
    addMode("Full", hyprshot::CaptureMode::Fullscreen);
    addMode("Region", hyprshot::CaptureMode::Region);
    addMode("Window", hyprshot::CaptureMode::Window);

    m_fullscreenScope = new QComboBox(m_toolbar);
    m_fullscreenScope->addItems({"all", "current", "per-monitor"});
    m_fullscreenScope->setCurrentText(qString(hyprshot::toString(m_defaults.fullscreenScope)));
    layout->addWidget(m_fullscreenScope);

    m_windowBackground = new QComboBox(m_toolbar);
    m_windowBackground->addItems({"follow-system", "white", "black", "real", "transparent"});
    m_windowBackground->setCurrentText(qString(hyprshot::toString(m_defaults.windowBackground)));
    layout->addWidget(m_windowBackground);

    m_save = new QCheckBox("Save", m_toolbar);
    m_save->setChecked(m_defaults.save);
    layout->addWidget(m_save);

    m_clipboard = new QCheckBox("Clipboard", m_toolbar);
    m_clipboard->setChecked(m_defaults.clipboard);
    layout->addWidget(m_clipboard);

    m_thumbnail = new QCheckBox("Thumbnail", m_toolbar);
    m_thumbnail->setChecked(m_defaults.showThumbnail);
    layout->addWidget(m_thumbnail);

    auto* capture = new QPushButton("Capture", m_toolbar);
    layout->addWidget(capture);
    connect(capture, &QPushButton::clicked, this, &CaptureOverlay::finishCapture);

    auto* cancel = new QPushButton("Cancel", m_toolbar);
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this, &CaptureOverlay::cancelCapture);

    m_status = new QLabel(m_toolbar);
    layout->addWidget(m_status);

    m_toolbar->adjustSize();
    m_toolbar->move((width() - m_toolbar->width()) / 2, 28);
}

void CaptureOverlay::setMode(hyprshot::CaptureMode mode) {
    m_mode = mode;
    update();
}

void CaptureOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 0, 0, 80));

    if (m_mode == hyprshot::CaptureMode::Region && (m_dragging || !normalizedSelection().isNull())) {
        const QRect sel = normalizedSelection();
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.fillRect(sel, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setPen(QPen(QColor(255, 255, 255, 230), 2));
        painter.drawRect(sel.adjusted(0, 0, -1, -1));
    } else if (m_mode == hyprshot::CaptureMode::Window) {
        const QPoint pos = mapFromGlobal(QCursor::pos());
        const QRect target(pos.x() - 180, pos.y() - 110, 360, 220);
        painter.setPen(QPen(QColor(255, 255, 255, 220), 2));
        painter.drawRoundedRect(target, 8, 8);
    }
}

void CaptureOverlay::mousePressEvent(QMouseEvent* event) {
    if (m_toolbar->geometry().contains(event->pos()))
        return;
    if (event->button() != Qt::LeftButton)
        return;

    m_dragging = true;
    m_dragStart = event->pos();
    m_dragEnd = event->pos();
    update();
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging)
        return;
    m_dragEnd = event->pos();
    update();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = false;
    m_dragEnd = event->pos();
    if (m_mode != hyprshot::CaptureMode::Region || normalizedSelection().width() > 4)
        finishCapture();
}

void CaptureOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        cancelCapture();
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        finishCapture();
    }
}

QRect CaptureOverlay::normalizedSelection() const {
    return QRect(m_dragStart, m_dragEnd).normalized();
}

QRect CaptureOverlay::captureRectForMode() const {
    if (m_mode == hyprshot::CaptureMode::Region && normalizedSelection().isValid())
        return normalizedSelection();
    if (m_mode == hyprshot::CaptureMode::Window) {
        const QPoint pos = mapFromGlobal(QCursor::pos());
        return QRect(pos.x() - 180, pos.y() - 110, 360, 220).intersected(rect());
    }
    return rect();
}

QImage CaptureOverlay::renderResultImage() const {
    const QRect cap = captureRectForMode();
    QImage image(cap.size().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    const auto bg = hyprshot::parseWindowBackground(m_windowBackground->currentText().toStdString(), m_defaults.windowBackground);
    const QRect desktopSource = cap.translated(geometry().topLeft() - m_desktopGeometry.topLeft());

    if (m_mode != hyprshot::CaptureMode::Window && !m_desktopImage.isNull()) {
        painter.drawImage(image.rect(), m_desktopImage, desktopSource);
    } else if (m_mode == hyprshot::CaptureMode::Window && bg == hyprshot::WindowBackground::Real && !m_desktopImage.isNull()) {
        painter.drawImage(image.rect(), m_desktopImage, desktopSource);
    } else if (m_mode == hyprshot::CaptureMode::Window && bg != hyprshot::WindowBackground::Transparent) {
        if (bg == hyprshot::WindowBackground::White)
            painter.fillRect(image.rect(), Qt::white);
        else if (bg == hyprshot::WindowBackground::Black)
            painter.fillRect(image.rect(), Qt::black);
        else if (bg == hyprshot::WindowBackground::FollowSystem)
            painter.fillRect(image.rect(), followSystemColor());
        else
            painter.fillRect(image.rect(), QColor(30, 34, 38));
    } else {
        painter.fillRect(image.rect(), QColor(30, 34, 38));
    }

    if (m_mode == hyprshot::CaptureMode::Window && !m_desktopImage.isNull()) {
        painter.drawImage(image.rect(), m_desktopImage, desktopSource);
        if (bg == hyprshot::WindowBackground::Transparent) {
            painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            painter.fillRect(image.rect(), QColor(255, 255, 255, 230));
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
    }
    return image;
}

void CaptureOverlay::finishCapture() {
    const auto image = renderResultImage();
    saveImage(image);
}

void CaptureOverlay::saveImage(const QImage& image) {
    QString savedPath;
    if (m_save->isChecked()) {
        const auto dirPath = hyprshot::expandUserPath(m_defaults.saveDir);
        QDir dir(QString::fromStdString(dirPath.string()));
        if (!dir.exists())
            dir.mkpath(".");
        savedPath = dir.filePath(QString::fromStdString(hyprshot::makeTimestampedFilename(m_defaults.filenameTemplate)));
        image.save(savedPath, "PNG");
    }

    if (m_clipboard->isChecked())
        QGuiApplication::clipboard()->setImage(image);

    if (m_thumbnail->isChecked())
        showThumbnail(image, savedPath);

    hide();
    if (!m_thumbnail->isChecked())
        qApp->quit();
}

void CaptureOverlay::showThumbnail(const QImage& image, const QString& path) {
    auto* thumb = new ResultThumbnail(QPixmap::fromImage(image), path, static_cast<int>(m_defaults.thumbnailTimeoutMs));
    const QRect screen = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->availableGeometry() : geometry();
    thumb->move(screen.right() - thumb->width() - 24, screen.bottom() - thumb->height() - 24);
    thumb->show();
    connect(thumb, &QObject::destroyed, qApp, &QCoreApplication::quit);
}

void CaptureOverlay::cancelCapture() {
    close();
    qApp->quit();
}
