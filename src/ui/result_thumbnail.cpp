#include "ui/result_thumbnail.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDrag>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStyleHints>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

ResultThumbnail::ResultThumbnail(const QPixmap& pixmap, QString path, int timeoutMs, QWidget* parent) : QWidget(parent), m_path(std::move(path)) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setFocusPolicy(Qt::NoFocus);
    setObjectName("thumbnail");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_StyledBackground);

    const auto palette = QApplication::palette();
    const QColor bg = palette.color(QPalette::Window);
    const QColor fg = palette.color(QPalette::WindowText);
    const QColor highlight = palette.color(QPalette::Highlight);
    setStyleSheet(QStringLiteral(
                      "#thumbnail { background: rgba(%1,%2,%3,220); border: 1px solid rgba(%4,%5,%6,95); border-radius: 8px; }"
                      "#thumbnailMenu QPushButton { color: rgba(%4,%5,%6,255); background: transparent; padding: 7px 10px; border: none; border-radius: 5px; text-align: left; }"
                      "#thumbnailMenu QPushButton:hover { background: rgba(%7,%8,%9,75); }")
                      .arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(fg.red()).arg(fg.green()).arg(fg.blue()).arg(highlight.red()).arg(highlight.green()).arg(highlight.blue()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    m_imageLabel = new QLabel(this);
    m_imageLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_imageLabel->setPixmap(pixmap.scaled(180, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout->addWidget(m_imageLabel);

    m_menuPanel = new QWidget(this);
    m_menuPanel->setObjectName("thumbnailMenu");
    auto* menuLayout = new QVBoxLayout(m_menuPanel);
    menuLayout->setContentsMargins(0, 0, 0, 0);
    menuLayout->setSpacing(2);
    const auto addAction = [&](const QString& text, auto&& callback) {
        auto* buttonWidget = new QPushButton(text, m_menuPanel);
        connect(buttonWidget, &QPushButton::clicked, this, std::forward<decltype(callback)>(callback));
        menuLayout->addWidget(buttonWidget);
    };
    addAction("Open", [this] {
        if (!m_path.isEmpty() && openPath(m_path))
            close();
    });
    addAction("Copy image", [this] {
        const auto currentPixmap = m_imageLabel->pixmap();
        if (!currentPixmap.isNull())
            QGuiApplication::clipboard()->setPixmap(currentPixmap);
    });
    addAction("Show in folder", [this] {
        if (!m_path.isEmpty())
            openPath(QFileInfo(m_path).absolutePath());
    });
    addAction("Close", [this] { close(); });
    m_menuPanel->hide();
    layout->addWidget(m_menuPanel);

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
    m_draggingFile = false;
    m_pressGlobal = event->globalPosition().toPoint();
}

void ResultThumbnail::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton) || m_draggingFile)
        return;

    const QPoint current = event->globalPosition().toPoint();
    const QPoint delta = current - m_pressGlobal;
    if (delta.manhattanLength() <= QGuiApplication::styleHints()->startDragDistance())
        return;

    m_dragMoved = true;
    startFileDrag();
}

void ResultThumbnail::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !m_dragMoved && !m_path.isEmpty() && openPath(m_path))
        close();
}

void ResultThumbnail::contextMenuEvent(QContextMenuEvent* event) {
    event->accept();
    toggleMenu();
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

void ResultThumbnail::toggleMenu() {
    m_closeTimer.stop();
    m_menuPanel->setVisible(!m_menuPanel->isVisible());
    applyLayerSize();
}

void ResultThumbnail::applyLayerSize() {
    adjustSize();
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle()))
        layerWindow->setDesiredSize(size());
}

void ResultThumbnail::startFileDrag() {
    if (m_path.isEmpty())
        return;

    m_draggingFile = true;
    m_closeTimer.stop();

    auto* drag = new QDrag(this);
    auto* mimeData = new QMimeData;
    mimeData->setUrls({QUrl::fromLocalFile(m_path)});

    const auto currentPixmap = m_imageLabel->pixmap();
    if (!currentPixmap.isNull()) {
        mimeData->setImageData(currentPixmap.toImage());
        drag->setPixmap(currentPixmap.scaled(180, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        drag->setHotSpot(QPoint(drag->pixmap().width() / 2, drag->pixmap().height() / 2));
    }

    drag->setMimeData(mimeData);
    const auto action = drag->exec(Qt::CopyAction, Qt::CopyAction);
    m_draggingFile = false;
    if (action != Qt::IgnoreAction)
        close();
}

void ResultThumbnail::enterEvent(QEnterEvent*) {
    m_closeTimer.stop();
}

void ResultThumbnail::leaveEvent(QEvent*) {
    if (m_closeTimer.interval() > 0)
        m_closeTimer.start();
}
