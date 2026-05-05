#include "ui/result_thumbnail.hpp"

#include "ui/clipboard_utils.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDrag>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>
#include <QPropertyAnimation>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStyleHints>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kThumbnailMaxWidth = 180;
constexpr int kThumbnailMaxHeight = 120;
constexpr int kThumbnailScreenMargin = 24;
constexpr double kSwipeCloseThreshold = 120.0;
constexpr double kSwipeDeleteThreshold = 90.0;

QColor interpolateColor(const QColor& from, const QColor& to, double amount) {
    amount = std::clamp(amount, 0.0, 1.0);
    const auto mix = [amount](int a, int b) {
        return static_cast<int>(std::round(a * (1.0 - amount) + b * amount));
    };
    return QColor(mix(from.red(), to.red()), mix(from.green(), to.green()), mix(from.blue(), to.blue()), mix(from.alpha(), to.alpha()));
}

} // namespace

class SwipeBackdrop final : public QWidget {
  public:
    explicit SwipeBackdrop(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setDeleteProgress(double progress) {
        m_deleteProgress = std::clamp(progress, 0.0, 1.0);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        if (m_deleteProgress <= 0.0)
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor idle(92, 94, 98, 130);
        const QColor danger(220, 38, 38, 245);
        const QColor color = interpolateColor(idle, danger, m_deleteProgress);
        painter.setPen(QPen(color, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);

        const QPointF center(width() / 2.0, height() / 2.0);
        const double scale = std::min(width(), height()) / 120.0;
        const double w = 34.0 * scale;
        const double h = 42.0 * scale;
        const QRectF body(center.x() - w / 2.0, center.y() - h / 2.0 + 5.0 * scale, w, h);
        painter.drawRoundedRect(body, 4.0 * scale, 4.0 * scale);
        painter.drawLine(QPointF(body.left() - 5.0 * scale, body.top() - 7.0 * scale), QPointF(body.right() + 5.0 * scale, body.top() - 7.0 * scale));
        painter.drawLine(QPointF(center.x() - 9.0 * scale, body.top() - 14.0 * scale), QPointF(center.x() + 9.0 * scale, body.top() - 14.0 * scale));
        painter.drawLine(QPointF(center.x() - 6.0 * scale, body.top() - 14.0 * scale), QPointF(center.x() - 10.0 * scale, body.top() - 7.0 * scale));
        painter.drawLine(QPointF(center.x() + 6.0 * scale, body.top() - 14.0 * scale), QPointF(center.x() + 10.0 * scale, body.top() - 7.0 * scale));
        painter.drawLine(QPointF(center.x() - 9.0 * scale, body.top() + 9.0 * scale), QPointF(center.x() - 9.0 * scale, body.bottom() - 8.0 * scale));
        painter.drawLine(QPointF(center.x(), body.top() + 9.0 * scale), QPointF(center.x(), body.bottom() - 8.0 * scale));
        painter.drawLine(QPointF(center.x() + 9.0 * scale, body.top() + 9.0 * scale), QPointF(center.x() + 9.0 * scale, body.bottom() - 8.0 * scale));
    }

  private:
    double m_deleteProgress = 0.0;
};

namespace {

QPointF wheelGestureDelta(QWheelEvent* event) {
    QPointF delta = event->pixelDelta();
    if (delta.isNull())
        delta = QPointF(event->angleDelta()) / 8.0;
    if (event->inverted())
        delta = -delta;
    return delta;
}

bool canDeleteThumbnailPath(const QString& path) {
    return !path.isEmpty() && hyprcapture::ui::isPrivateRuntimePath(path) && QFileInfo::exists(path);
}

} // namespace

ResultThumbnail::ResultThumbnail(const QPixmap& pixmap, QString path, QString restoreClipboardPath, int timeoutMs, QWidget* parent)
    : QWidget(parent), m_path(std::move(path)), m_restoreClipboardPath(std::move(restoreClipboardPath)) {
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
                      "#thumbnail { background: transparent; border: none; }"
                      "#thumbnailImage { background: rgba(%1,%2,%3,220); border: 1px solid rgba(%4,%5,%6,95); border-radius: 8px; }"
                      "#thumbnailMenu { background: rgba(%1,%2,%3,242); border: 1px solid rgba(%4,%5,%6,90); border-radius: 7px; }"
                      "#thumbnailMenu QPushButton { color: rgba(%4,%5,%6,255); background: transparent; padding: 7px 10px; border: none; border-radius: 5px; text-align: left; }"
                      "#thumbnailMenu QPushButton:hover { background: rgba(%7,%8,%9,75); }")
                      .arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(fg.red()).arg(fg.green()).arg(fg.blue()).arg(highlight.red()).arg(highlight.green()).arg(highlight.blue()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_menuPanel = new QWidget(this);
    m_menuPanel->setObjectName("thumbnailMenu");
    m_menuPanel->setAttribute(Qt::WA_StyledBackground);
    auto* menuLayout = new QVBoxLayout(m_menuPanel);
    menuLayout->setContentsMargins(6, 6, 6, 6);
    menuLayout->setSpacing(2);
    const auto addAction = [&](const QString& text, auto callback) {
        auto* buttonWidget = new QPushButton(text, m_menuPanel);
        connect(buttonWidget, &QPushButton::clicked, this, [this, callback = std::move(callback)]() mutable {
            if (m_menuPanel && m_menuPanel->isVisible()) {
                m_menuPanel->hide();
                applyLayerSize();
                if (m_closeTimer.interval() > 0)
                    m_closeTimer.start();
            }
            callback();
        });
        menuLayout->addWidget(buttonWidget);
    };
    addAction("Open", [this] {
        if (!m_path.isEmpty() && openPath(m_path))
            close();
    });
    addAction("Copy image", [this] {
        const auto currentPixmap = m_imageLabel->pixmap();
        if (!currentPixmap.isNull())
            hyprcapture::ui::copyPixmapToClipboard(currentPixmap);
    });
    addAction("Show in folder", [this] {
        if (!m_path.isEmpty())
            openPath(QFileInfo(m_path).absolutePath());
    });
    if (canDeleteThumbnailPath(m_path))
        addAction("Delete", [this] { deleteAndClose(); });
    addAction("Close", [this] { close(); });
    m_menuPanel->hide();
    layout->addWidget(m_menuPanel, 0, Qt::AlignRight);

    const QPixmap scaledPixmap = pixmap.scaled(kThumbnailMaxWidth, kThumbnailMaxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_card = new QWidget(this);
    m_card->setObjectName("thumbnailImageCard");
    m_card->setFixedSize((scaledPixmap.size() + QSize(kThumbnailScreenMargin, kThumbnailScreenMargin)).expandedTo(QSize(1, 1)));

    m_swipeBackdrop = new SwipeBackdrop(m_card);
    m_swipeBackdrop->setGeometry(QRect(QPoint(0, 0), scaledPixmap.size()));
    m_swipeBackdrop->lower();

    m_imageLabel = new QLabel(m_card);
    m_imageLabel->setObjectName("thumbnailImage");
    m_imageLabel->setAttribute(Qt::WA_StyledBackground);
    m_imageLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_imageLabel->setPixmap(scaledPixmap);
    m_imageLabel->setGeometry(QRect(QPoint(0, 0), scaledPixmap.size()));
    m_imageOrigin = m_imageLabel->pos();
    m_imageLabel->raise();
    layout->addWidget(m_card, 0, Qt::AlignRight);

    adjustSize();
    winId();
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle())) {
        layerWindow->setScope("hyprcapture-thumbnail");
        layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        layerWindow->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorRight} | LayerShellQt::Window::AnchorBottom);
        layerWindow->setMargins(QMargins(0, 0, 0, 0));
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

    m_swipeResetTimer.setSingleShot(true);
    connect(&m_swipeResetTimer, &QTimer::timeout, this, &ResultThumbnail::resetSwipe);
}

void ResultThumbnail::closeEvent(QCloseEvent* event) {
    QWidget::closeEvent(event);
    hyprcapture::ui::discardClipboardSnapshot(m_restoreClipboardPath);
    m_restoreClipboardPath.clear();
    qApp->quit();
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

void ResultThumbnail::wheelEvent(QWheelEvent* event) {
    if (m_swipeCompleting)
        return;

    const QPointF delta = wheelGestureDelta(event);
    if (delta.manhattanLength() <= 0.0)
        return;

    m_closeTimer.stop();
    m_swipeResetTimer.stop();
    m_swipeOffset += delta;
    const int imageWidth = m_imageLabel ? m_imageLabel->width() : m_card->width();
    const int imageHeight = m_imageLabel ? m_imageLabel->height() : m_card->height();

    if (std::abs(m_swipeOffset.x()) >= std::abs(m_swipeOffset.y())) {
        m_swipeOffset.setX(std::clamp(m_swipeOffset.x(), 0.0, static_cast<double>(m_card->width() + 80)));
        m_swipeOffset.setY(0.0);
    } else {
        m_swipeOffset.setX(0.0);
        m_swipeOffset.setY(std::clamp(m_swipeOffset.y(), 0.0, static_cast<double>(m_card->height() + 80)));
    }
    updateSwipeVisual();
    event->accept();

    if (m_swipeOffset.x() >= std::min(kSwipeCloseThreshold, imageWidth * 0.55))
        animateSwipeOut(SwipeAction::Close);
    else if (canDeleteThumbnailPath(m_path) && m_swipeOffset.y() >= std::min(kSwipeDeleteThreshold, imageHeight * 0.55))
        animateSwipeOut(SwipeAction::Delete);
    else
        m_swipeResetTimer.start(180);
}

bool ResultThumbnail::openPath(const QString& path) {
    QProcess process;
    auto environment = hyprcapture::ui::trustedProcessEnvironment();
    environment.remove("QT_WAYLAND_SHELL_INTEGRATION");
    process.setProcessEnvironment(environment);
    const QString program = hyprcapture::ui::trustedSystemProgram(QStringLiteral("xdg-open"));
    if (program.isEmpty())
        return false;
    process.setProgram(program);
    process.setArguments({path});
    return process.startDetached();
}

void ResultThumbnail::toggleMenu() {
    m_closeTimer.stop();
    m_menuPanel->setVisible(!m_menuPanel->isVisible());
    applyLayerSize();
    if (!m_menuPanel->isVisible() && m_closeTimer.interval() > 0)
        m_closeTimer.start();
}

void ResultThumbnail::applyLayerSize() {
    adjustSize();
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle()))
        layerWindow->setDesiredSize(size());
}

void ResultThumbnail::updateSwipeVisual() {
    if (!m_imageLabel || !m_swipeBackdrop)
        return;

    m_imageLabel->move(m_imageOrigin + QPoint(static_cast<int>(std::round(m_swipeOffset.x())), static_cast<int>(std::round(m_swipeOffset.y()))));
    m_swipeBackdrop->setDeleteProgress(m_swipeOffset.y() / std::min(kSwipeDeleteThreshold, m_imageLabel->height() * 0.55));
}

void ResultThumbnail::resetSwipe() {
    if (m_swipeCompleting || !m_imageLabel)
        return;

    auto* animation = new QPropertyAnimation(m_imageLabel, "pos", this);
    animation->setDuration(160);
    animation->setStartValue(m_imageLabel->pos());
    animation->setEndValue(m_imageOrigin);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this] {
        m_swipeOffset = {};
        if (m_swipeBackdrop)
            m_swipeBackdrop->setDeleteProgress(0.0);
        if (m_closeTimer.interval() > 0)
            m_closeTimer.start();
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void ResultThumbnail::animateSwipeOut(SwipeAction action) {
    if (m_swipeCompleting || !m_imageLabel || !m_card)
        return;

    m_swipeCompleting = true;
    m_swipeResetTimer.stop();
    m_closeTimer.stop();

    const QPoint target = action == SwipeAction::Close ? QPoint(m_card->width() + 24, m_imageOrigin.y()) : QPoint(m_imageOrigin.x(), m_card->height() + 24);
    auto* animation = new QPropertyAnimation(m_imageLabel, "pos", this);
    animation->setDuration(190);
    animation->setStartValue(m_imageLabel->pos());
    animation->setEndValue(target);
    animation->setEasingCurve(QEasingCurve::InCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this, action] {
        if (action == SwipeAction::Delete)
            deleteAndClose();
        else
            close();
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void ResultThumbnail::deleteAndClose() {
    if (!canDeleteThumbnailPath(m_path))
        return;
    if (!QFile::remove(m_path))
        return;

    restoreClipboard();
    close();
}

void ResultThumbnail::restoreClipboard() const {
    hyprcapture::ui::restoreClipboardSnapshot(m_restoreClipboardPath);
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
