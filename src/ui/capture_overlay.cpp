#include "ui/capture_overlay.hpp"

#include "ui/result_thumbnail.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFrame>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStringList>
#include <QStyleHints>
#include <QVBoxLayout>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdlib>

class InlineSelect final : public QWidget {
  public:
    explicit InlineSelect(QWidget* popupParent, QWidget* parent = nullptr);

    void addItems(const QStringList& items);
    void setCurrentText(const QString& text);
    QString currentText() const;
    void hidePopup();
    bool isPopupVisible() const;

  private:
    void showPopup();
    void choose(const QString& text);

    QWidget*     m_popupParent = nullptr;
    QPushButton* m_button = nullptr;
    QFrame*      m_panel = nullptr;
    QVBoxLayout* m_panelLayout = nullptr;
    QStringList  m_items;
    QString      m_current;
};

namespace {

InlineSelect* g_openSelect = nullptr;

QString qString(const std::string& value) {
    return QString::fromStdString(value);
}

QColor followSystemColor() {
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    return scheme == Qt::ColorScheme::Light ? QColor(245, 245, 245) : QColor(17, 19, 23);
}

QString cssRgba(QColor color, int alpha = -1) {
    if (alpha >= 0)
        color.setAlpha(alpha);
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

QColor mixedColor(QColor a, QColor b, double amount) {
    const auto mix = [&](int av, int bv) { return static_cast<int>(std::round(av * (1.0 - amount) + bv * amount)); };
    return QColor(mix(a.red(), b.red()), mix(a.green(), b.green()), mix(a.blue(), b.blue()), mix(a.alpha(), b.alpha()));
}

QString toolbarStyleSheet(const QPalette& palette) {
    const QColor window = palette.color(QPalette::Window);
    const QColor button = palette.color(QPalette::Button);
    const QColor text = palette.color(QPalette::WindowText);
    const QColor highlight = palette.color(QPalette::Highlight);
    const QColor highlightedText = palette.color(QPalette::HighlightedText);
    const QColor border = mixedColor(text, window, 0.55);
    const QColor hover = mixedColor(button, highlight, 0.16);
    const QColor checked = mixedColor(button, highlight, 0.32);

    return QStringLiteral(
               "#toolbar { background: %1; border: 1px solid %2; border-radius: 8px; }"
               "QPushButton { color: %3; background: transparent; padding: 6px 10px; border: none; border-radius: 5px; }"
               "QPushButton:hover { background: %4; }"
               "QPushButton:checked { color: %3; background: %5; }"
               "QPushButton:pressed { color: %6; background: %7; }"
               "QLabel { color: %3; }")
        .arg(cssRgba(window, 238), cssRgba(border, 150), cssRgba(text), cssRgba(hover, 180), cssRgba(checked, 220), cssRgba(highlightedText), cssRgba(highlight), cssRgba(button, 170));
}

QString popupStyleSheet(const QPalette& palette) {
    const QColor window = palette.color(QPalette::Window);
    const QColor button = palette.color(QPalette::Button);
    const QColor text = palette.color(QPalette::WindowText);
    const QColor highlight = palette.color(QPalette::Highlight);
    const QColor highlightedText = palette.color(QPalette::HighlightedText);
    const QColor border = mixedColor(text, window, 0.55);
    const QColor hover = mixedColor(button, highlight, 0.16);

    return QStringLiteral(
               "#inlineSelectPopup { background: %1; border: 1px solid %2; border-radius: 7px; }"
               "#inlineSelectPopup QPushButton { color: %3; background: transparent; padding: 7px 12px; border: none; border-radius: 5px; text-align: left; }"
               "#inlineSelectPopup QPushButton:hover { background: %4; }"
               "#inlineSelectPopup QPushButton:checked { color: %5; background: %6; }")
        .arg(cssRgba(window, 246), cssRgba(border, 150), cssRgba(text), cssRgba(hover, 210), cssRgba(highlightedText), cssRgba(highlight));
}

QRect jsonRect(const QJsonObject& obj) {
    return QRect(QPoint(static_cast<int>(std::floor(obj.value("x").toDouble())), static_cast<int>(std::floor(obj.value("y").toDouble()))),
                 QSize(std::max(1, static_cast<int>(std::ceil(obj.value("width").toDouble()))), std::max(1, static_cast<int>(std::ceil(obj.value("height").toDouble())))));
}

bool artifactTopDown(const QJsonObject& obj) {
    // Older plugin builds emitted raw GL readback without orientation metadata.
    // The observed legacy artifact layout is bottom-up.
    return obj.contains("artifactTopDown") ? obj.value("artifactTopDown").toBool(true) : false;
}

QImage loadRawRgba(const QString& path, int width, int height, bool topDown) {
    QFile file(path);
    if (path.isEmpty() || width <= 0 || height <= 0 || !file.open(QIODevice::ReadOnly))
        return {};

    const QByteArray bytes = file.readAll();
    const qsizetype expected = static_cast<qsizetype>(width) * static_cast<qsizetype>(height) * 4;
    if (bytes.size() < expected)
        return {};

    QImage image(reinterpret_cast<const uchar*>(bytes.constData()), width, height, width * 4, QImage::Format_RGBA8888);
    QImage copy = image.copy();
    return topDown ? copy : copy.flipped(Qt::Vertical);
}

QRect projectedImageRect(const QRect& logicalRect, const QRect& fullGeometry, const QSize& imageSize) {
    if (!logicalRect.isValid() || !fullGeometry.isValid() || imageSize.isEmpty())
        return {};

    const double scaleX = static_cast<double>(imageSize.width()) / std::max(1, fullGeometry.width());
    const double scaleY = static_cast<double>(imageSize.height()) / std::max(1, fullGeometry.height());
    QRect rect(QPoint(static_cast<int>(std::floor((logicalRect.x() - fullGeometry.x()) * scaleX)),
                      static_cast<int>(std::floor((logicalRect.y() - fullGeometry.y()) * scaleY))),
               QSize(std::max(1, static_cast<int>(std::ceil(logicalRect.width() * scaleX))),
                     std::max(1, static_cast<int>(std::ceil(logicalRect.height() * scaleY)))));

    // Some Hyprland fake-render paths draw the window below its nominal crop and
    // the plugin shifts the readback up. Keep the expected visible size instead
    // of losing the bottom rows when that makes the projected top negative.
    if (rect.x() < 0)
        rect.moveLeft(0);
    if (rect.y() < 0)
        rect.moveTop(0);
    return rect.intersected(QRect(QPoint(0, 0), imageSize));
}

int transparentPixelsInRow(const QImage& image, const QRect& span, int y) {
    if (image.format() != QImage::Format_RGBA8888 || y < 0 || y >= image.height())
        return 0;

    int transparent = 0;
    const auto* row = image.constScanLine(y);
    for (int x = span.left(); x <= span.right(); ++x) {
        const auto* px = row + static_cast<qsizetype>(x) * 4;
        if (px[3] == 0)
            ++transparent;
    }
    return transparent;
}

void copyPatchPixel(QImage& target, const QImage& patch, int x, int y) {
    auto* dst = target.scanLine(y) + static_cast<qsizetype>(x) * 4;
    const auto* src = patch.constScanLine(y) + static_cast<qsizetype>(x) * 4;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = 255;
}

void repairMissingWindowTail(QImage& image, const QRect& fullGeometry, const QRect& visibleGeometry, const QImage& desktopImage, const QRect& desktopGeometry) {
    if (image.isNull() || !fullGeometry.isValid() || !visibleGeometry.isValid() || desktopImage.isNull() || !desktopGeometry.isValid())
        return;

    if (image.format() != QImage::Format_RGBA8888)
        image = image.convertToFormat(QImage::Format_RGBA8888);

    const QRect visibleImageRect = projectedImageRect(visibleGeometry, fullGeometry, image.size());
    if (!visibleImageRect.isValid() || visibleImageRect.width() < 8 || visibleImageRect.height() < 8)
        return;

    int tailStart = -1;
    for (int y = visibleImageRect.bottom(); y >= visibleImageRect.top(); --y) {
        const int transparent = transparentPixelsInRow(image, visibleImageRect, y);
        if (transparent * 10 >= visibleImageRect.width() * 9)
            tailStart = y;
        else
            break;
    }
    if (tailStart < 0 || visibleImageRect.bottom() - tailStart + 1 < 8)
        return;

    QImage visiblePatch(image.size(), QImage::Format_RGBA8888);
    visiblePatch.fill(Qt::transparent);
    {
        QPainter patchPainter(&visiblePatch);
        patchPainter.drawImage(visibleImageRect, desktopImage, QRect(visibleGeometry.topLeft() - desktopGeometry.topLeft(), visibleGeometry.size()));
    }

    const QRect visibleTailRect(visibleImageRect.left(), tailStart, visibleImageRect.width(), visibleImageRect.bottom() - tailStart + 1);
    for (int y = visibleTailRect.top(); y <= visibleTailRect.bottom(); ++y) {
        for (int x = visibleTailRect.left(); x <= visibleTailRect.right(); ++x) {
            auto* dst = image.scanLine(y) + static_cast<qsizetype>(x) * 4;
            if (dst[3] != 0)
                continue;
            const auto* src = visiblePatch.constScanLine(y) + static_cast<qsizetype>(x) * 4;
            if (src[3] == 0)
                continue;
            copyPatchPixel(image, visiblePatch, x, y);
        }
    }
}

} // namespace

InlineSelect::InlineSelect(QWidget* popupParent, QWidget* parent) : QWidget(parent), m_popupParent(popupParent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_button = new QPushButton(this);
    m_button->setCheckable(true);
    m_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(m_button);
    connect(m_button, &QPushButton::clicked, this, [this] {
        if (isPopupVisible())
            hidePopup();
        else
            showPopup();
    });

    m_panel = new QFrame(m_popupParent);
    m_panel->setObjectName("inlineSelectPopup");
    m_panel->setAttribute(Qt::WA_StyledBackground);
    m_panel->setStyleSheet(popupStyleSheet(QApplication::palette()));
    m_panel->hide();

    m_panelLayout = new QVBoxLayout(m_panel);
    m_panelLayout->setContentsMargins(5, 5, 5, 5);
    m_panelLayout->setSpacing(2);
}

void InlineSelect::addItems(const QStringList& items) {
    m_items = items;
    while (auto* item = m_panelLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const auto& item : m_items) {
        auto* button = new QPushButton(item, m_panel);
        button->setCheckable(true);
        connect(button, &QPushButton::clicked, this, [this, item] { choose(item); });
        m_panelLayout->addWidget(button);
    }

    int width = 0;
    const auto metrics = m_button->fontMetrics();
    for (const auto& item : m_items)
        width = std::max(width, metrics.horizontalAdvance(item + QStringLiteral("  ▾")) + 34);
    m_button->setMinimumWidth(width);
    m_panel->setMinimumWidth(width);

    if (m_current.isEmpty() && !m_items.isEmpty())
        setCurrentText(m_items.first());
}

void InlineSelect::setCurrentText(const QString& text) {
    m_current = text;
    m_button->setText(text + QStringLiteral("  ▾"));
    for (auto* button : m_panel->findChildren<QPushButton*>())
        button->setChecked(button->text() == text);
}

QString InlineSelect::currentText() const {
    return m_current;
}

void InlineSelect::hidePopup() {
    m_panel->hide();
    m_button->setChecked(false);
    if (g_openSelect == this)
        g_openSelect = nullptr;
}

bool InlineSelect::isPopupVisible() const {
    return m_panel->isVisible();
}

void InlineSelect::showPopup() {
    if (g_openSelect && g_openSelect != this)
        g_openSelect->hidePopup();
    m_panel->adjustSize();
    QPoint pos = mapTo(m_popupParent, QPoint(0, height() + 5));
    if (pos.x() + m_panel->width() > m_popupParent->width() - 8)
        pos.setX(std::max(8, m_popupParent->width() - m_panel->width() - 8));
    if (pos.y() + m_panel->height() > m_popupParent->height() - 8)
        pos.setY(std::max(8, mapTo(m_popupParent, QPoint(0, 0)).y() - m_panel->height() - 5));
    m_panel->move(pos);
    m_panel->raise();
    m_panel->show();
    m_button->setChecked(true);
    g_openSelect = this;
}

void InlineSelect::choose(const QString& text) {
    setCurrentText(text);
    hidePopup();
}

CaptureOverlay::CaptureOverlay(hyprshot::CaptureDefaults defaults, bool quick, QString sessionJson, QWidget* parent)
    : QMainWindow(parent), m_defaults(std::move(defaults)), m_mode(m_defaults.mode), m_quick(quick) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);

    parseSessionJson(sessionJson);
    captureScreensBeforeOverlay();
    setGeometry(m_desktopGeometry.isValid() ? m_desktopGeometry : QRect(0, 0, 1280, 720));

    buildToolbar();
    winId();
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle())) {
        layerWindow->setScope("hyprshot-ui");
        layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        layerWindow->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorTop} | LayerShellQt::Window::AnchorBottom |
                                LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
        layerWindow->setExclusiveZone(-1);
        layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        layerWindow->setActivateOnShow(true);
        layerWindow->setDesiredSize(QSize(0, 0));
    }
    if (m_quick)
        QTimer::singleShot(0, this, &CaptureOverlay::finishCapture);
}

void CaptureOverlay::parseSessionJson(const QString& json) {
    const auto doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return;

    const auto root = doc.object();
    const auto monitors = root.value("monitors").toArray();
    const auto windows = root.value("windows").toArray();
    m_sessionMonitorCount = monitors.size();
    m_sessionWindowCount = windows.size();

    for (const auto value : monitors) {
        const auto obj = value.toObject();
        MonitorArtifact artifact;
        artifact.name = obj.value("name").toString();
        artifact.logicalGeometry = jsonRect(obj.value("geometry").toObject());
        artifact.image = loadRawRgba(obj.value("artifactPath").toString(), obj.value("artifactWidth").toInt(), obj.value("artifactHeight").toInt(), artifactTopDown(obj));
        if (!artifact.logicalGeometry.isValid())
            continue;
        m_desktopGeometry = m_desktopGeometry.united(artifact.logicalGeometry);
        if (!artifact.image.isNull())
            m_monitorArtifacts.push_back(std::move(artifact));
    }

    for (const auto value : windows) {
        const auto obj = value.toObject();
        WindowArtifact artifact;
        artifact.title = obj.value("title").toString();
        artifact.appClass = obj.value("class").toString();
        artifact.visibleGeometry = jsonRect(obj.value("visibleGeometry").toObject());
        artifact.fullGeometry = jsonRect(obj.value("fullGeometry").toObject());
        artifact.image = loadRawRgba(obj.value("artifactPath").toString(), obj.value("artifactWidth").toInt(), obj.value("artifactHeight").toInt(), artifactTopDown(obj));
        if (artifact.fullGeometry.isValid() && !artifact.image.isNull())
            m_windowArtifacts.push_back(std::move(artifact));
    }
}

void CaptureOverlay::captureScreensBeforeOverlay() {
    if (!m_desktopGeometry.isValid())
        for (const auto* screen : QGuiApplication::screens())
            m_desktopGeometry = m_desktopGeometry.united(screen->geometry());

    if (!m_desktopGeometry.isValid())
        return;

    m_desktopImage = QImage(m_desktopGeometry.size(), QImage::Format_RGBA8888);
    m_desktopImage.fill(QColor(30, 34, 38));

    QPainter painter(&m_desktopImage);
    if (!m_monitorArtifacts.empty()) {
        for (const auto& artifact : m_monitorArtifacts) {
            const QPoint target = artifact.logicalGeometry.topLeft() - m_desktopGeometry.topLeft();
            painter.drawImage(QRect(target, artifact.logicalGeometry.size()), artifact.image);
        }
        return;
    }

    QProcess grim;
    grim.start("grim", {"-t", "png", "-"});
    if (grim.waitForFinished(1500) && grim.exitStatus() == QProcess::NormalExit && grim.exitCode() == 0) {
        QImage grimImage;
        if (grimImage.loadFromData(grim.readAllStandardOutput(), "PNG") && !grimImage.isNull()) {
            painter.drawImage(m_desktopImage.rect(), grimImage);
            return;
        }
    }

    const auto screens = QGuiApplication::screens();
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
    m_toolbar->setAttribute(Qt::WA_StyledBackground);
    m_toolbar->setStyleSheet(toolbarStyleSheet(QApplication::palette()));

    auto* layout = new QHBoxLayout(m_toolbar);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSizeConstraint(QLayout::SetFixedSize);

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

    m_fullscreenScope = new InlineSelect(this, m_toolbar);
    m_fullscreenScope->addItems(QStringList{"all", "current", "per-monitor"});
    m_fullscreenScope->setCurrentText(qString(hyprshot::toString(m_defaults.fullscreenScope)));
    layout->addWidget(m_fullscreenScope);

    m_windowBackground = new InlineSelect(this, m_toolbar);
    m_windowBackground->addItems(QStringList{"follow-system", "white", "black", "real", "transparent"});
    m_windowBackground->setCurrentText(qString(hyprshot::toString(m_defaults.windowBackground)));
    layout->addWidget(m_windowBackground);

    auto* capture = new QPushButton("Capture", m_toolbar);
    layout->addWidget(capture);
    connect(capture, &QPushButton::clicked, this, &CaptureOverlay::finishCapture);

    auto* cancel = new QPushButton("Cancel", m_toolbar);
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this, &CaptureOverlay::cancelCapture);

    m_status = new QLabel(m_toolbar);
    m_status->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(m_status);
    updateStatus();
    relayoutToolbar();
}

void CaptureOverlay::setMode(hyprshot::CaptureMode mode) {
    m_mode = mode;
    updateStatus();
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
        const auto* window = hoveredWindow();
        for (const auto& candidate : m_windowArtifacts) {
            const QRect target = globalToLocalRect(windowFrameGeometry(candidate));
            painter.setPen(QPen(&candidate == window ? QColor(255, 255, 255, 240) : QColor(255, 255, 255, 110), &candidate == window ? 3 : 1));
            painter.drawRoundedRect(target, 8, 8);
        }
    }
}

void CaptureOverlay::mousePressEvent(QMouseEvent* event) {
    if (m_toolbar->geometry().contains(event->pos()))
        return;
    if (m_fullscreenScope)
        m_fullscreenScope->hidePopup();
    if (m_windowBackground)
        m_windowBackground->hidePopup();
    if (event->button() != Qt::LeftButton)
        return;

    if (m_mode == hyprshot::CaptureMode::Window)
        return;

    m_dragging = true;
    m_dragStart = event->pos();
    m_dragEnd = event->pos();
    update();
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (m_mode == hyprshot::CaptureMode::Window) {
        updateStatus();
        update();
        return;
    }
    if (!m_dragging)
        return;
    m_dragEnd = event->pos();
    update();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    if (m_mode == hyprshot::CaptureMode::Window) {
        finishCapture();
        return;
    }
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
        if (const auto* window = hoveredWindow())
            return globalToLocalRect(windowFrameGeometry(*window));
        return {};
    }
    return rect();
}

QRect CaptureOverlay::globalToLocalRect(const QRect& rect) const {
    return QRect(mapFromGlobal(rect.topLeft()), rect.size());
}

QRect CaptureOverlay::localToDesktopSourceRect(const QRect& rect) const {
    return QRect(mapToGlobal(rect.topLeft()) - m_desktopGeometry.topLeft(), rect.size());
}

QPoint CaptureOverlay::cursorLogicalPosition() const {
    return mapToGlobal(mapFromGlobal(QCursor::pos()));
}

QRect CaptureOverlay::windowFrameGeometry(const WindowArtifact& window) const {
    return window.visibleGeometry.isValid() ? window.visibleGeometry : window.fullGeometry;
}

const CaptureOverlay::WindowArtifact* CaptureOverlay::hoveredWindow() const {
    const QPoint global = cursorLogicalPosition();
    for (auto it = m_windowArtifacts.rbegin(); it != m_windowArtifacts.rend(); ++it) {
        if (windowFrameGeometry(*it).contains(global))
            return &*it;
    }
    return nullptr;
}

bool CaptureOverlay::windowCaptureAvailable() const {
    return !m_windowArtifacts.empty();
}

void CaptureOverlay::updateStatus() {
    if (!m_status)
        return;

    if (m_mode != hyprshot::CaptureMode::Window) {
        m_status->clear();
        relayoutToolbar();
        return;
    }

    if (!windowCaptureAvailable()) {
        if (m_sessionMonitorCount == 0 && m_sessionWindowCount == 0)
            m_status->setText("plugin reload needed");
        else if (m_sessionWindowCount > 0)
            m_status->setText("window artifact failed");
        else
            m_status->setText("no visible windows");
        relayoutToolbar();
        return;
    }

    const auto* window = hoveredWindow();
    m_status->setText(window ? QString("%1").arg(window->appClass.isEmpty() ? window->title : window->appClass) : QString("choose window"));
    relayoutToolbar();
}

void CaptureOverlay::relayoutToolbar() {
    if (!m_toolbar)
        return;

    m_toolbar->setMinimumWidth(0);
    m_toolbar->setMaximumWidth(QWIDGETSIZE_MAX);
    m_toolbar->adjustSize();
    const int maxWidth = std::max(1, width() - 32);
    if (m_toolbar->width() > maxWidth)
        m_toolbar->setFixedWidth(maxWidth);
    else
        m_toolbar->setFixedWidth(m_toolbar->sizeHint().width());
    m_toolbar->move(std::max(16, (width() - m_toolbar->width()) / 2), 28);
}

QImage CaptureOverlay::renderResultImage() const {
    const auto bg = hyprshot::parseWindowBackground(m_windowBackground->currentText().toStdString(), m_defaults.windowBackground);
    if (m_mode == hyprshot::CaptureMode::Window) {
        const auto* windowArtifact = hoveredWindow();
        if (!windowArtifact || windowArtifact->image.isNull())
            return {};

        QRect artifactSource = windowArtifact->image.rect();
        QRect logicalSource = windowArtifact->fullGeometry;
        const bool cropDecorations = m_defaults.windowBorder == hyprshot::DecorationPolicy::Remove || m_defaults.windowShadow == hyprshot::DecorationPolicy::Remove;
        if (cropDecorations && windowArtifact->visibleGeometry.isValid() && windowArtifact->fullGeometry.contains(windowArtifact->visibleGeometry)) {
            const double scaleX = static_cast<double>(windowArtifact->image.width()) / std::max(1, windowArtifact->fullGeometry.width());
            const double scaleY = static_cast<double>(windowArtifact->image.height()) / std::max(1, windowArtifact->fullGeometry.height());
            artifactSource = QRect(QPoint(static_cast<int>(std::floor((windowArtifact->visibleGeometry.x() - windowArtifact->fullGeometry.x()) * scaleX)),
                                          static_cast<int>(std::floor((windowArtifact->visibleGeometry.y() - windowArtifact->fullGeometry.y()) * scaleY))),
                                   QSize(std::max(1, static_cast<int>(std::ceil(windowArtifact->visibleGeometry.width() * scaleX))),
                                         std::max(1, static_cast<int>(std::ceil(windowArtifact->visibleGeometry.height() * scaleY)))))
                                 .intersected(windowArtifact->image.rect());
            logicalSource = windowArtifact->visibleGeometry;
        }

        QImage repairedArtifact = windowArtifact->image;
        repairMissingWindowTail(repairedArtifact, windowArtifact->fullGeometry, windowArtifact->visibleGeometry, m_desktopImage, m_desktopGeometry);

        QImage image(artifactSource.size().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);

        QPainter painter(&image);
        const QRect desktopSource = QRect(logicalSource.topLeft() - m_desktopGeometry.topLeft(), logicalSource.size());
        if (bg == hyprshot::WindowBackground::Real && !m_desktopImage.isNull())
            painter.drawImage(image.rect(), m_desktopImage, desktopSource);
        else if (bg == hyprshot::WindowBackground::White)
            painter.fillRect(image.rect(), Qt::white);
        else if (bg == hyprshot::WindowBackground::Black)
            painter.fillRect(image.rect(), Qt::black);
        else if (bg == hyprshot::WindowBackground::FollowSystem)
            painter.fillRect(image.rect(), followSystemColor());
        else if (bg != hyprshot::WindowBackground::Transparent)
            painter.fillRect(image.rect(), QColor(30, 34, 38));

        painter.drawImage(image.rect(), repairedArtifact, artifactSource);
        return image;
    }

    const QRect cap = captureRectForMode();
    if (!cap.isValid())
        return {};
    QImage image(cap.size().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    const QRect desktopSource = localToDesktopSourceRect(cap);

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

    return image;
}

void CaptureOverlay::finishCapture() {
    const auto image = renderResultImage();
    if (image.isNull()) {
        updateStatus();
        return;
    }
    saveImage(image);
}

void CaptureOverlay::saveImage(const QImage& image) {
    QString savedPath;
    if (m_defaults.save) {
        const auto dirPath = hyprshot::expandUserPath(m_defaults.saveDir);
        QDir dir(QString::fromStdString(dirPath.string()));
        if (!dir.exists())
            dir.mkpath(".");
        savedPath = dir.filePath(QString::fromStdString(hyprshot::makeTimestampedFilename(m_defaults.filenameTemplate)));
        image.save(savedPath, "PNG");
    }

    if (m_defaults.clipboard)
        QGuiApplication::clipboard()->setImage(image);

    if (m_defaults.showThumbnail)
        showThumbnail(image, savedPath);

    hide();
    if (!m_defaults.showThumbnail)
        qApp->quit();
}

void CaptureOverlay::showThumbnail(const QImage& image, const QString& path) {
    QString thumbPath = path;
    if (thumbPath.isEmpty()) {
        const auto runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        QDir dir(runtimeDir.isEmpty() ? QDir::tempPath() : runtimeDir);
        if (!dir.exists("hyprshot"))
            dir.mkpath("hyprshot");
        thumbPath = dir.filePath(QStringLiteral("hyprshot/thumbnail-%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
        image.save(thumbPath, "PNG");
    }

    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                            {"--thumbnail-window", thumbPath, "--thumbnail-timeout-ms", QString::number(m_defaults.thumbnailTimeoutMs)});
    qApp->quit();
}

void CaptureOverlay::cancelCapture() {
    close();
    qApp->quit();
}
