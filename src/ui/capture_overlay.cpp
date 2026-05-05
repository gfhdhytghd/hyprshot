#include "ui/capture_overlay.hpp"

#include "ui/clipboard_utils.hpp"
#include "ui/result_thumbnail.hpp"
#include "ui/watermark.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QFileInfo>
#include <QGuiApplication>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMetaObject>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QSizePolicy>
#include <QStringList>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QThread>
#include <QVBoxLayout>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

class InlineSelect final : public QWidget {
  public:
    explicit InlineSelect(QWidget* popupParent, QWidget* parent = nullptr);

    void addItems(const QStringList& items);
    void setPrefix(const QString& prefix);
    void setCurrentText(const QString& text);
    QString currentText() const;
    void setControlVisible(bool visible);
    void hidePopup();
    bool isPopupVisible() const;

  private:
    QString buttonText(const QString& text) const;
    void updateButtonIcon();
    void showPopup();
    void choose(const QString& text);

    QWidget*     m_popupParent = nullptr;
    QPushButton* m_button = nullptr;
    QFrame*      m_panel = nullptr;
    QVBoxLayout* m_panelLayout = nullptr;
    int          m_buttonWidth = 0;
    QStringList  m_items;
    QString      m_current;
    QString      m_prefix;
};

namespace {

InlineSelect* g_openSelect = nullptr;
constexpr int kWindowBackgroundMinAlpha = 32;
constexpr int kWindowShadowMaxRgb = 32;
constexpr int kWindowShadowMaxAlpha = 223;
constexpr int kWindowBackgroundInteriorRadius = 1;
constexpr double kWindowFrameFallbackRadius = 8.0;
constexpr int kOverlayFadeDurationMs = 100;
constexpr int kModeIconSize = 24;
constexpr int kCancelIconSize = 16;
constexpr int kSelectArrowIconSize = 12;
constexpr int kMaxArtifactDimension = 32768;
constexpr qint64 kMaxArtifactBytes = 512LL * 1024LL * 1024LL;
constexpr int kMaxLogicalCoordinate = 1'000'000;
constexpr int kMaxSessionMonitors = 64;
constexpr int kMaxSessionWindows = 512;

struct CaptureOutputResult {
    QString savedPath;
    QString restoreClipboardPath;
    bool    showThumbnail = false;
    bool    clipboardRequested = false;
    bool    clipboardCopied = false;
};

const char* kFullscreenSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M128 266.666667v490.666666a53.393333 53.393333 0 0 0 53.333333 53.333334h661.333334a53.393333 53.393333 0 0 0 53.333333-53.333334V266.666667a53.393333 53.393333 0 0 0-53.333333-53.333334H181.333333a53.393333 53.393333 0 0 0-53.333333 53.333334z m725.333333 0v490.666666a10.666667 10.666667 0 0 1-10.666666 10.666667H181.333333a10.666667 10.666667 0 0 1-10.666666-10.666667V266.666667a10.666667 10.666667 0 0 1 10.666666-10.666667h661.333334a10.666667 10.666667 0 0 1 10.666666 10.666667z m-597.333333 608a21.333333 21.333333 0 0 1-21.333333 21.333333H96a53.393333 53.393333 0 0 1-53.333333-53.333333v-138.666667a21.333333 21.333333 0 0 1 42.666666 0v138.666667a10.666667 10.666667 0 0 0 10.666667 10.666666h138.666667a21.333333 21.333333 0 0 1 21.333333 21.333334zM42.666667 320V181.333333a53.393333 53.393333 0 0 1 53.333333-53.333333h138.666667a21.333333 21.333333 0 0 1 0 42.666667H96a10.666667 10.666667 0 0 0-10.666667 10.666666v138.666667a21.333333 21.333333 0 0 1-42.666666 0z m938.666666-138.666667v138.666667a21.333333 21.333333 0 0 1-42.666666 0V181.333333a10.666667 10.666667 0 0 0-10.666667-10.666666h-138.666667a21.333333 21.333333 0 0 1 0-42.666667h138.666667a53.393333 53.393333 0 0 1 53.333333 53.333333z m0 522.666667v138.666667a53.393333 53.393333 0 0 1-53.333333 53.333333h-138.666667a21.333333 21.333333 0 0 1 0-42.666667h138.666667a10.666667 10.666667 0 0 0 10.666667-10.666666v-138.666667a21.333333 21.333333 0 0 1 42.666666 0z" fill="#000000"/></svg>)";
const char* kWindowSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M808.125883 243.195881 134.874315 243.195881c-30.608112 0-55.513338 24.905226-55.513338 55.520501l0 505.178641c0 30.615275 24.905226 55.520501 55.513338 55.520501L808.125883 859.415524c30.607088 0 55.512315-24.905226 55.512315-55.520501L863.638197 298.716382C863.638197 268.101107 838.733994 243.195881 808.125883 243.195881zM835.629283 803.895023c0 15.167444-12.338003 27.510564-27.503401 27.510564L134.874315 831.405587c-15.167444 0-27.504424-12.343119-27.504424-27.510564L107.369891 383.246591l728.259392 0L835.629283 803.895023zM835.629283 355.236654 107.370915 355.236654l0-56.519248c0-15.173584 12.33698-27.510564 27.504424-27.510564L808.125883 271.206842c15.165398 0 27.503401 12.33698 27.503401 27.510564L835.629283 355.236654zM920.166655 131.156132 274.924002 131.156132c-30.608112 0-55.513338 24.905226-55.513338 55.514361l0 28.515451c0 7.734148 6.263657 14.004969 14.005992 14.004969 7.740288 0 14.005992-6.27082 14.005992-14.004969l0-28.515451c0-15.167444 12.33698-27.504424 27.503401-27.504424L920.167678 159.166069c15.165398 0 27.503401 12.33698 27.503401 27.504424l0 519.188726c0 15.167444-12.338003 27.511587-27.503401 27.511587l-28.516474 0c-7.739265 0-14.004969 6.27082-14.004969 14.004969 0 7.736195 6.263657 14.007015 14.004969 14.007015l28.516474 0c30.607088 0 55.512315-24.905226 55.512315-55.521524L975.679993 186.670493C975.67897 156.061358 950.773743 131.156132 920.166655 131.156132zM219.410664 299.216779l-56.019875 0c-7.740288 0-14.005992 6.27082-14.005992 13.998829 0 7.740288 6.263657 14.011108 14.005992 14.011108l56.019875 0c7.740288 0 14.005992-6.27082 14.005992-14.011108C233.415632 305.487599 227.151975 299.216779 219.410664 299.216779zM331.450413 299.216779l-56.019875 0c-7.741311 0-14.005992 6.27082-14.005992 13.998829 0 7.740288 6.262634 14.011108 14.005992 14.011108l56.019875 0c7.739265 0 14.004969-6.27082 14.004969-14.011108C345.455381 305.487599 339.191724 299.216779 331.450413 299.216779zM443.490162 299.216779l-56.018851 0c-7.741311 0-14.007015 6.27082-14.007015 13.998829 0 7.740288 6.263657 14.011108 14.007015 14.011108l56.018851 0c7.740288 0 14.005992-6.27082 14.005992-14.011108C457.49513 305.487599 451.231473 299.216779 443.490162 299.216779z" fill="#000000"/></svg>)";
const char* kRegionSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M960 256V64H768v64H256V64H64v192h64v512H64v192h192v-64h512v64h192V768h-64V256z m-128 512h-64v64H256v-64h-64V256h64v-64h512v64h64z" fill="#000000"/></svg>)";
const char* kCancelSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M883.8304 41.01546667L512.00213333 412.84693333 140.1696 41.01546667c-27.38026667-27.3792-71.77386667-27.3792-99.1552 0-27.37813333 27.3792-27.37813333 71.77066667 0 99.1552l371.8336 371.83146666L41.0144 883.82933333c-27.37813333 27.38026667-27.37813333 71.776 0 99.15413334 27.38133333 27.38133333 71.776 27.38133333 99.1552 0L512.00213333 611.15733333l371.82933334 371.82613334c27.37813333 27.38133333 71.77386667 27.38133333 99.15306666 0 27.3792-27.37813333 27.3792-71.77386667 0-99.15413334L611.15733333 512.00213333 982.98453333 140.17066667c27.3792-27.38133333 27.3792-71.776 0-99.1552-27.3792-27.38133333-71.7696-27.38133333-99.15413333 0z m0 0" fill="#333333"/></svg>)";
const char* kSelectArrowSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M827.733333 411.733333L526.933333 712.533333c-8.533333 8.533333-21.333333 8.533333-29.866666 0L196.266667 411.733333c-17.066667-17.066667-17.066667-42.666667 0-59.733333 17.066667-17.066667 42.666667-17.066667 59.733333 0l256 256 256-256c17.066667-17.066667 42.666667-17.066667 59.733333 0s17.066667 42.666667 0 59.733333z"/></svg>)";

QString qString(const std::string& value) {
    return QString::fromStdString(value);
}

bool timingEnabled() {
    return qEnvironmentVariableIsSet("HYPRCAPTURE_TIMING") || qEnvironmentVariableIsSet("HYPRCAPTURE_TIMING_FILE");
}

void traceTiming(const QString& event, qint64 elapsedMs = -1) {
    if (!timingEnabled())
        return;

    QString line = QStringLiteral("%1 pid=%2 %3")
                       .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
                       .arg(QCoreApplication::applicationPid())
                       .arg(event);
    if (elapsedMs >= 0)
        line += QStringLiteral(" elapsed_ms=%1").arg(elapsedMs);
    line += QLatin1Char('\n');

    const QString path = qEnvironmentVariable("HYPRCAPTURE_TIMING_FILE");
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append))
            file.write(line.toUtf8());
        return;
    }

    fputs(line.toLocal8Bit().constData(), stderr);
}

bool savePng(const QImage& image, const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.remove();
        return false;
    }

    QImageWriter writer(&file, "PNG");
    writer.setQuality(75);
    if (!writer.write(image)) {
        file.remove();
        return false;
    }
    return true;
}

QString uniqueOutputPath(const QDir& dir, const QString& rawFilename) {
    QFileInfo info(QFileInfo(rawFilename).fileName());
    QString   base = info.completeBaseName();
    QString   suffix = info.suffix();
    if (base.isEmpty())
        base = QStringLiteral("Screenshot");
    if (suffix.isEmpty())
        suffix = QStringLiteral("png");

    for (int i = 0; i < 1000; ++i) {
        const QString filename = i == 0 ? QStringLiteral("%1.%2").arg(base, suffix) : QStringLiteral("%1-%2.%3").arg(base).arg(i).arg(suffix);
        const QString path = dir.filePath(filename);
        if (!QFileInfo::exists(path))
            return path;
    }

    return dir.filePath(QStringLiteral("%1-%2.%3").arg(base).arg(QDateTime::currentMSecsSinceEpoch()).arg(suffix));
}

void cleanupArtifactFiles(const QStringList& paths) {
    QStringList dirs;
    for (const QString& path : paths) {
        if (!hyprcapture::ui::isPrivateRuntimePath(path))
            continue;
        QFile::remove(path);
        const QString dir = QFileInfo(path).absolutePath();
        if (!dirs.contains(dir))
            dirs.push_back(dir);
    }

    for (const QString& dir : dirs) {
        if (!hyprcapture::ui::isPrivateRuntimePath(dir))
            continue;
        QDir parent(QFileInfo(dir).absolutePath());
        parent.rmdir(QFileInfo(dir).fileName());
    }
}

CaptureOutputResult writeCaptureOutput(const QImage& image,
                                        const hyprcapture::CaptureDefaults& defaults,
                                        const hyprcapture::ui::ClipboardSnapshotData& clipboardSnapshot) {
    CaptureOutputResult result;
    result.showThumbnail = defaults.showThumbnail;

    if (defaults.clipboard && defaults.showThumbnail) {
        QElapsedTimer timer;
        timer.start();
        result.restoreClipboardPath = hyprcapture::ui::saveClipboardSnapshotData(clipboardSnapshot);
        traceTiming(QStringLiteral("clipboard_snapshot_save"), timer.elapsed());
    }

    if (defaults.save) {
        QElapsedTimer timer;
        timer.start();
        const auto dirPath = hyprcapture::expandUserPath(defaults.saveDir);
        QDir       dir(QString::fromStdString(dirPath.string()));
        if (!dir.exists())
            dir.mkpath(".");
        const QString path = uniqueOutputPath(dir, QString::fromStdString(hyprcapture::makeTimestampedFilename(defaults.filenameTemplate)));
        if (savePng(image, path))
            result.savedPath = path;
        traceTiming(QStringLiteral("image_save"), timer.elapsed());
    }

    if (result.showThumbnail && result.savedPath.isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        const QString path = hyprcapture::ui::runtimeFile("thumbnail", ".png");
        if (hyprcapture::ui::savePrivatePng(image, path))
            result.savedPath = path;
        traceTiming(QStringLiteral("thumbnail_temp_save"), timer.elapsed());
    }

    result.clipboardRequested = defaults.clipboard;
    if (defaults.clipboard) {
        QElapsedTimer timer;
        timer.start();
        if (result.savedPath.isEmpty() || !hyprcapture::ui::copyImageFileToClipboardDetached(result.savedPath))
            result.clipboardCopied = hyprcapture::ui::copyImageToClipboardDetached(image);
        else
            result.clipboardCopied = true;
        traceTiming(QStringLiteral("clipboard_copy_start"), timer.elapsed());
    }

    return result;
}

double maxScreenDevicePixelRatio() {
    double dpr = 1.0;
    for (const auto* screen : QGuiApplication::screens())
        dpr = std::max(dpr, screen ? screen->devicePixelRatio() : 1.0);
    return dpr;
}

QIcon iconFromSvg(const char* svg, int logicalSize = kModeIconSize, double rotationDegrees = 0.0) {
    QSvgRenderer renderer{QByteArray(svg)};
    const double dpr = maxScreenDevicePixelRatio();
    QPixmap pixmap(QSize(std::max(1, static_cast<int>(std::ceil(logicalSize * dpr))),
                         std::max(1, static_cast<int>(std::ceil(logicalSize * dpr)))));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(logicalSize / 2.0, logicalSize / 2.0);
    if (rotationDegrees != 0.0)
        painter.rotate(rotationDegrees);
    renderer.render(&painter, QRectF(-logicalSize / 2.0, -logicalSize / 2.0, logicalSize, logicalSize));
    return QIcon(pixmap);
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
    const QColor modeChecked = mixedColor(text, window, 0.72);

    return QStringLiteral(
               "#toolbar { background: %1; border: 1px solid %2; border-radius: 8px; }"
               "QPushButton { color: %3; background: transparent; padding: 6px 10px; border: none; border-radius: 5px; outline: none; }"
               "QPushButton:hover { background: %4; }"
               "QPushButton:checked { color: %3; background: %5; }"
               "QPushButton:pressed { color: %6; background: %7; }"
               "QPushButton#captureModeButton { padding: 4px 6px; background: transparent; border: none; outline: none; }"
               "QPushButton#captureModeButton:hover { background: transparent; }"
               "QPushButton#captureModeButton:checked { background: %8; border-radius: 7px; }"
               "QPushButton#captureModeButton:pressed { background: %8; border-radius: 7px; }"
               "QLabel { color: %3; }")
        .arg(cssRgba(window, 238),
             cssRgba(border, 150),
             cssRgba(text),
             cssRgba(hover, 180),
             cssRgba(checked, 220),
             cssRgba(highlightedText),
             cssRgba(highlight),
             cssRgba(modeChecked));
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

bool imageSizeWithinBounds(const QSize& size) {
    if (size.isEmpty() || size.width() <= 0 || size.height() <= 0 || size.width() > kMaxArtifactDimension || size.height() > kMaxArtifactDimension)
        return false;

    const auto width = static_cast<qint64>(size.width());
    const auto height = static_cast<qint64>(size.height());
    if (width > std::numeric_limits<qint64>::max() / height)
        return false;
    const auto pixels = width * height;
    if (pixels > std::numeric_limits<qint64>::max() / 4)
        return false;
    return pixels * 4 <= kMaxArtifactBytes;
}

QImage boundedImage(const QSize& size, QImage::Format format) {
    if (!imageSizeWithinBounds(size))
        return {};
    return QImage(size, format);
}

QSize boundedScaledSize(int width, int height, double scaleX, double scaleY) {
    if (width <= 0 || height <= 0 || !std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0)
        return {};

    const double scaledWidth = std::ceil(width * scaleX);
    const double scaledHeight = std::ceil(height * scaleY);
    if (!std::isfinite(scaledWidth) || !std::isfinite(scaledHeight) || scaledWidth < 1.0 || scaledHeight < 1.0 ||
        scaledWidth > kMaxArtifactDimension || scaledHeight > kMaxArtifactDimension)
        return {};

    const QSize size(static_cast<int>(scaledWidth), static_cast<int>(scaledHeight));
    return imageSizeWithinBounds(size) ? size : QSize{};
}

bool boundedDoubleToInt(double value, int minimum, int maximum, bool ceilValue, int& out) {
    if (!std::isfinite(value))
        return false;
    const double rounded = ceilValue ? std::ceil(value) : std::floor(value);
    if (rounded < minimum || rounded > maximum)
        return false;
    out = static_cast<int>(rounded);
    return true;
}

QRect jsonRect(const QJsonObject& obj) {
    if (!obj.contains("x") || !obj.contains("y") || !obj.contains("width") || !obj.contains("height"))
        return {};

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (!boundedDoubleToInt(obj.value("x").toDouble(), -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, x) ||
        !boundedDoubleToInt(obj.value("y").toDouble(), -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, y) ||
        !boundedDoubleToInt(obj.value("width").toDouble(), 1, kMaxArtifactDimension, true, width) ||
        !boundedDoubleToInt(obj.value("height").toDouble(), 1, kMaxArtifactDimension, true, height))
        return {};

    return QRect(QPoint(x, y), QSize(width, height));
}

bool jsonPoint(const QJsonObject& obj, QPoint& point) {
    if (!obj.contains("x") || !obj.contains("y"))
        return false;

    int x = 0;
    int y = 0;
    if (!boundedDoubleToInt(obj.value("x").toDouble(), -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, x) ||
        !boundedDoubleToInt(obj.value("y").toDouble(), -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, y))
        return false;

    point = QPoint(x, y);
    return true;
}

bool artifactTopDown(const QJsonObject& obj) {
    // Older plugin builds emitted raw GL readback without orientation metadata.
    // The observed legacy artifact layout is bottom-up.
    return obj.contains("artifactTopDown") ? obj.value("artifactTopDown").toBool(true) : false;
}

QImage loadRawRgba(const QString& path, int width, int height, bool topDown) {
    QFile file(path);
    if (path.isEmpty() || width <= 0 || height <= 0 || width > kMaxArtifactDimension || height > kMaxArtifactDimension)
        return {};

    const qint64 expected = static_cast<qint64>(width) * static_cast<qint64>(height) * 4;
    if (expected <= 0 || expected > kMaxArtifactBytes || !hyprcapture::ui::isPrivateRuntimeFile(path, expected) || !file.open(QIODevice::ReadOnly))
        return {};

    const QByteArray bytes = file.readAll();
    if (bytes.size() != expected)
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

QRect artifactRectToLogicalRect(const QRect& artifactRect, const QSize& artifactSize, const QRect& fullGeometry) {
    if (!artifactRect.isValid() || artifactSize.isEmpty() || !fullGeometry.isValid())
        return {};

    const double scaleX = static_cast<double>(fullGeometry.width()) / std::max(1, artifactSize.width());
    const double scaleY = static_cast<double>(fullGeometry.height()) / std::max(1, artifactSize.height());
    const int x1 = fullGeometry.x() + static_cast<int>(std::floor(artifactRect.x() * scaleX));
    const int y1 = fullGeometry.y() + static_cast<int>(std::floor(artifactRect.y() * scaleY));
    const int x2 = fullGeometry.x() + static_cast<int>(std::ceil((artifactRect.x() + artifactRect.width()) * scaleX));
    const int y2 = fullGeometry.y() + static_cast<int>(std::ceil((artifactRect.y() + artifactRect.height()) * scaleY));
    return QRect(QPoint(x1, y1), QSize(std::max(1, x2 - x1), std::max(1, y2 - y1))).intersected(fullGeometry);
}

QRect logicalRectToImageRect(const QRect& logicalRect, const QRect& logicalGeometry, const QSize& imageSize) {
    const QRect clipped = logicalRect.intersected(logicalGeometry);
    if (!clipped.isValid() || !logicalGeometry.isValid() || imageSize.isEmpty())
        return {};

    const double scaleX = static_cast<double>(imageSize.width()) / std::max(1, logicalGeometry.width());
    const double scaleY = static_cast<double>(imageSize.height()) / std::max(1, logicalGeometry.height());
    const int x1 = static_cast<int>(std::floor((clipped.x() - logicalGeometry.x()) * scaleX));
    const int y1 = static_cast<int>(std::floor((clipped.y() - logicalGeometry.y()) * scaleY));
    const int x2 = static_cast<int>(std::ceil((clipped.x() + clipped.width() - logicalGeometry.x()) * scaleX));
    const int y2 = static_cast<int>(std::ceil((clipped.y() + clipped.height() - logicalGeometry.y()) * scaleY));
    return QRect(QPoint(x1, y1), QSize(std::max(1, x2 - x1), std::max(1, y2 - y1))).intersected(QRect(QPoint(0, 0), imageSize));
}

QRect logicalRectToOutputRect(const QRect& logicalRect, const QRect& outputLogicalGeometry, double scaleX, double scaleY) {
    if (!logicalRect.isValid() || !outputLogicalGeometry.isValid() || scaleX <= 0.0 || scaleY <= 0.0)
        return {};

    const int x1 = static_cast<int>(std::floor((logicalRect.x() - outputLogicalGeometry.x()) * scaleX));
    const int y1 = static_cast<int>(std::floor((logicalRect.y() - outputLogicalGeometry.y()) * scaleY));
    const int x2 = static_cast<int>(std::ceil((logicalRect.x() + logicalRect.width() - outputLogicalGeometry.x()) * scaleX));
    const int y2 = static_cast<int>(std::ceil((logicalRect.y() + logicalRect.height() - outputLogicalGeometry.y()) * scaleY));
    return QRect(QPoint(x1, y1), QSize(std::max(1, x2 - x1), std::max(1, y2 - y1)));
}

QPointF superellipsePoint(const QPointF& center, double radius, double power, double signX, double signY, double theta) {
    const double exponent = 2.0 / std::clamp(power, 1.0, 10.0);
    const double x = std::pow(std::max(0.0, std::cos(theta)), exponent) * radius;
    const double y = std::pow(std::max(0.0, std::sin(theta)), exponent) * radius;
    return {center.x() + signX * x, center.y() + signY * y};
}

void appendSuperellipseCorner(QPainterPath& path,
                              const QPointF& center,
                              double radius,
                              double power,
                              double signX,
                              double signY,
                              double startTheta,
                              double endTheta) {
    const int steps = std::clamp(static_cast<int>(std::ceil(radius / 2.0)), 8, 32);
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        path.lineTo(superellipsePoint(center, radius, power, signX, signY, startTheta + (endTheta - startTheta) * t));
    }
}

QPainterPath roundedWindowFramePath(const QRectF& rawRect, double radius, double power) {
    const QRectF rect = rawRect.normalized();
    QPainterPath path;
    if (!rect.isValid())
        return path;

    radius = std::clamp(radius, 0.0, std::min(rect.width(), rect.height()) / 2.0);
    if (radius <= 0.0) {
        path.addRect(rect);
        return path;
    }

    constexpr double halfPi = 1.57079632679489661923;
    const double left = rect.left();
    const double top = rect.top();
    const double right = rect.right();
    const double bottom = rect.bottom();

    path.moveTo(left + radius, top);
    path.lineTo(right - radius, top);
    appendSuperellipseCorner(path, {right - radius, top + radius}, radius, power, 1.0, -1.0, halfPi, 0.0);
    path.lineTo(right, bottom - radius);
    appendSuperellipseCorner(path, {right - radius, bottom - radius}, radius, power, 1.0, 1.0, 0.0, halfPi);
    path.lineTo(left + radius, bottom);
    appendSuperellipseCorner(path, {left + radius, bottom - radius}, radius, power, -1.0, 1.0, halfPi, 0.0);
    path.lineTo(left, top + radius);
    appendSuperellipseCorner(path, {left + radius, top + radius}, radius, power, -1.0, -1.0, 0.0, halfPi);
    path.closeSubpath();
    return path;
}

bool paintWindowBackground(QImage& background,
                           hyprcapture::WindowBackground bg,
                           const QImage& desktopImage,
                           const QRect& desktopSource) {
    if (background.isNull() || bg == hyprcapture::WindowBackground::Transparent)
        return false;

    QPainter backgroundPainter(&background);
    if (bg == hyprcapture::WindowBackground::Real) {
        if (desktopImage.isNull() || !desktopSource.isValid())
            return false;
        backgroundPainter.drawImage(background.rect(), desktopImage, desktopSource);
        return true;
    }

    if (bg == hyprcapture::WindowBackground::White)
        backgroundPainter.fillRect(background.rect(), Qt::white);
    else if (bg == hyprcapture::WindowBackground::Black)
        backgroundPainter.fillRect(background.rect(), Qt::black);
    else if (bg == hyprcapture::WindowBackground::FollowSystem)
        backgroundPainter.fillRect(background.rect(), followSystemColor());
    else
        backgroundPainter.fillRect(background.rect(), QColor(30, 34, 38));
    return true;
}

void reconstructRealWindowBackground(QImage& background, const QImage& artifact, const QRect& artifactSource) {
    if (background.format() != QImage::Format_RGBA8888 || artifact.format() != QImage::Format_RGBA8888 || background.isNull() || artifact.isNull())
        return;

    // The desktop snapshot already contains the selected window. Invert the
    // source-over blend so the window artifact is not composited twice.
    for (int y = 0; y < background.height(); ++y) {
        auto* dst = background.scanLine(y);
        const int sy = artifactSource.y() + y;
        if (sy < 0 || sy >= artifact.height())
            continue;

        const auto* src = artifact.constScanLine(sy);
        for (int x = 0; x < background.width(); ++x) {
            const int sx = artifactSource.x() + x;
            if (sx < 0 || sx >= artifact.width())
                continue;

            auto* dstPx = dst + static_cast<qsizetype>(x) * 4;
            const auto* srcPx = src + static_cast<qsizetype>(sx) * 4;
            const int alpha = srcPx[3];
            if (alpha <= 0 || alpha >= 255)
                continue;

            const int inverseAlpha = 255 - alpha;
            for (int channel = 0; channel < 3; ++channel) {
                const int value = (dstPx[channel] * 255 - srcPx[channel] * alpha + inverseAlpha / 2) / inverseAlpha;
                dstPx[channel] = static_cast<uchar>(std::clamp(value, 0, 255));
            }
            dstPx[3] = 255;
        }
    }
}

bool isWindowContentPixel(const uchar* px) {
    const int alpha = px[3];
    if (alpha < kWindowBackgroundMinAlpha)
        return false;

    const int maxRgb = std::max({px[0], px[1], px[2]});
    return maxRgb > kWindowShadowMaxRgb || alpha > kWindowShadowMaxAlpha;
}

bool isWindowContentPixelAt(const QImage& artifact, int x, int y) {
    if (x < 0 || x >= artifact.width() || y < 0 || y >= artifact.height())
        return false;

    const auto* px = artifact.constScanLine(y) + static_cast<qsizetype>(x) * 4;
    return isWindowContentPixel(px);
}

bool isWindowInteriorPixel(const QImage& artifact, int x, int y) {
    if (!isWindowContentPixelAt(artifact, x, y))
        return false;

    for (int dy = -kWindowBackgroundInteriorRadius; dy <= kWindowBackgroundInteriorRadius; ++dy) {
        for (int dx = -kWindowBackgroundInteriorRadius; dx <= kWindowBackgroundInteriorRadius; ++dx) {
            if (!isWindowContentPixelAt(artifact, x + dx, y + dy))
                return false;
        }
    }

    return true;
}

void applyWindowContentAlphaMask(QImage& background, const QImage& artifact, const QRect& artifactSource) {
    if (background.format() != QImage::Format_RGBA8888 || artifact.format() != QImage::Format_RGBA8888 || background.isNull() || artifact.isNull())
        return;

    for (int y = 0; y < background.height(); ++y) {
        auto* dst = background.scanLine(y);
        const int sy = artifactSource.y() + y;
        if (sy < 0 || sy >= artifact.height()) {
            std::fill(dst, dst + static_cast<qsizetype>(background.width()) * 4, 0);
            continue;
        }

        for (int x = 0; x < background.width(); ++x) {
            auto* dstPx = dst + static_cast<qsizetype>(x) * 4;
            const int sx = artifactSource.x() + x;
            if (sx < 0 || sx >= artifact.width()) {
                dstPx[0] = 0;
                dstPx[1] = 0;
                dstPx[2] = 0;
                dstPx[3] = 0;
                continue;
            }

            if (!isWindowInteriorPixel(artifact, sx, sy)) {
                dstPx[0] = 0;
                dstPx[1] = 0;
                dstPx[2] = 0;
                dstPx[3] = 0;
            } else {
                dstPx[3] = 255;
            }
        }
    }
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
    m_button->setLayoutDirection(Qt::RightToLeft);
    m_button->setIconSize(QSize(kSelectArrowIconSize, kSelectArrowIconSize));
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
    updateButtonIcon();
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
        width = std::max(width, metrics.horizontalAdvance(buttonText(item)) + 42);
    m_buttonWidth = width;
    m_button->setMinimumWidth(m_buttonWidth);
    m_panel->setMinimumWidth(width);

    if (m_current.isEmpty() && !m_items.isEmpty())
        setCurrentText(m_items.first());
}

void InlineSelect::setPrefix(const QString& prefix) {
    m_prefix = prefix;
    if (!m_current.isEmpty())
        m_button->setText(buttonText(m_current));
}

void InlineSelect::setCurrentText(const QString& text) {
    m_current = text;
    m_button->setText(buttonText(text));
    for (auto* button : m_panel->findChildren<QPushButton*>())
        button->setChecked(button->text() == text);
}

QString InlineSelect::currentText() const {
    return m_current;
}

void InlineSelect::setControlVisible(bool visible) {
    if (!visible) {
        hidePopup();
        m_button->hide();
        m_button->setMinimumSize(0, 0);
        m_button->setMaximumSize(0, 0);
        setFixedSize(0, 0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        setVisible(false);
        updateGeometry();
        return;
    }

    setVisible(visible);
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_button->show();
    m_button->setMinimumWidth(m_buttonWidth);
    m_button->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    adjustSize();
    updateGeometry();
}

void InlineSelect::hidePopup() {
    m_panel->hide();
    m_button->setChecked(false);
    updateButtonIcon();
    if (g_openSelect == this)
        g_openSelect = nullptr;
}

bool InlineSelect::isPopupVisible() const {
    return m_panel->isVisible();
}

QString InlineSelect::buttonText(const QString& text) const {
    if (m_prefix.isEmpty())
        return text;
    return m_prefix + QStringLiteral(": ") + text;
}

void InlineSelect::updateButtonIcon() {
    if (!m_button)
        return;
    const double rotationDegrees = isPopupVisible() ? 180.0 : 0.0;
    m_button->setIcon(iconFromSvg(kSelectArrowSvg, kSelectArrowIconSize, rotationDegrees));
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
    updateButtonIcon();
    g_openSelect = this;
}

void InlineSelect::choose(const QString& text) {
    setCurrentText(text);
    hidePopup();
}

CaptureOverlay::CaptureOverlay(hyprcapture::CaptureDefaults defaults, bool quick, QString sessionJson, QWidget* parent)
    : QMainWindow(parent), m_defaults(std::move(defaults)), m_mode(m_defaults.mode), m_quick(quick) {
    QElapsedTimer constructorTimer;
    constructorTimer.start();
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);

    QElapsedTimer parseTimer;
    parseTimer.start();
    parseSessionJson(sessionJson);
    traceTiming(QStringLiteral("parse_session"), parseTimer.elapsed());

    QElapsedTimer preCaptureTimer;
    preCaptureTimer.start();
    captureScreensBeforeOverlay();
    traceTiming(QStringLiteral("prepare_desktop"), preCaptureTimer.elapsed());

    setGeometry(m_desktopGeometry.isValid() ? m_desktopGeometry : QRect(0, 0, 1280, 720));

    QElapsedTimer toolbarTimer;
    toolbarTimer.start();
    buildToolbar();
    traceTiming(QStringLiteral("build_toolbar"), toolbarTimer.elapsed());

    winId();
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle())) {
        layerWindow->setScope("hyprcapture-ui");
        layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        layerWindow->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorTop} | LayerShellQt::Window::AnchorBottom |
                                LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
        layerWindow->setExclusiveZone(-1);
        layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        layerWindow->setActivateOnShow(true);
        layerWindow->setDesiredSize(QSize(0, 0));
    }
    traceTiming(QStringLiteral("overlay_construct"), constructorTimer.elapsed());
    if (m_quick)
        QTimer::singleShot(0, this, &CaptureOverlay::finishCapture);
}

void CaptureOverlay::parseSessionJson(const QString& json) {
    const auto doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return;

    const auto root = doc.object();
    QPoint cursorPosition;
    if (jsonPoint(root.value("cursorPosition").toObject(), cursorPosition)) {
        m_cursorLogicalPosition = cursorPosition;
        m_hasCursorLogicalPosition = true;
    }

    const auto defaultValues = root.value("defaults").toObject();
    if (defaultValues.contains("fushionMode"))
        m_defaults.fushionMode = defaultValues.value("fushionMode").toBool(m_defaults.fushionMode);
    if (defaultValues.contains("fusionMode"))
        m_defaults.fushionMode = defaultValues.value("fusionMode").toBool(m_defaults.fushionMode);
    if (defaultValues.contains("watermark"))
        m_defaults.watermark = defaultValues.value("watermark").toString(QString::fromStdString(m_defaults.watermark)).toStdString();
    if (defaultValues.contains("watermarkPosition"))
        m_defaults.watermarkPosition =
            hyprcapture::parseWatermarkPosition(defaultValues.value("watermarkPosition").toString(QString::fromStdString(hyprcapture::toString(m_defaults.watermarkPosition))).toStdString(),
                                                m_defaults.watermarkPosition);
    if (defaultValues.contains("watermarkWidth"))
        m_defaults.watermarkWidth = defaultValues.value("watermarkWidth").toString(QString::fromStdString(m_defaults.watermarkWidth)).toStdString();
    if (defaultValues.contains("watermarkOffset"))
        m_defaults.watermarkOffset = defaultValues.value("watermarkOffset").toString(QString::fromStdString(m_defaults.watermarkOffset)).toStdString();

    const auto monitors = root.value("monitors").toArray();
    const auto windows = root.value("windows").toArray();
    m_sessionMonitorCount = std::min(static_cast<int>(monitors.size()), kMaxSessionMonitors);
    m_sessionWindowCount = std::min(static_cast<int>(windows.size()), kMaxSessionWindows);
    QStringList artifactFiles;

    for (qsizetype i = 0; i < monitors.size() && i < kMaxSessionMonitors; ++i) {
        const auto value = monitors.at(i);
        const auto obj = value.toObject();
        MonitorArtifact artifact;
        artifact.name = obj.value("name").toString();
        artifact.logicalGeometry = jsonRect(obj.value("geometry").toObject());
        const QString artifactPath = obj.value("artifactPath").toString();
        artifact.image = loadRawRgba(artifactPath, obj.value("artifactWidth").toInt(), obj.value("artifactHeight").toInt(), artifactTopDown(obj));
        artifactFiles.push_back(artifactPath);
        if (!artifact.logicalGeometry.isValid())
            continue;
        m_desktopGeometry = m_desktopGeometry.united(artifact.logicalGeometry);
        if (!artifact.image.isNull())
            m_monitorArtifacts.push_back(std::move(artifact));
    }

    for (qsizetype i = 0; i < windows.size() && i < kMaxSessionWindows; ++i) {
        const auto value = windows.at(i);
        const auto obj = value.toObject();
        WindowArtifact artifact;
        artifact.title = obj.value("title").toString();
        artifact.appClass = obj.value("class").toString();
        artifact.visibleGeometry = jsonRect(obj.value("visibleGeometry").toObject());
        artifact.fullGeometry = jsonRect(obj.value("fullGeometry").toObject());
        artifact.rounding = obj.contains("rounding") ? obj.value("rounding").toDouble(0.0) : kWindowFrameFallbackRadius;
        artifact.roundingPower = obj.value("roundingPower").toDouble(2.0);
        artifact.borderSize = obj.value("borderSize").toDouble(0.0);
        const QString artifactPath = obj.value("artifactPath").toString();
        const QString realBackgroundPath = obj.value("realBackgroundPath").toString();
        artifact.image = loadRawRgba(artifactPath, obj.value("artifactWidth").toInt(), obj.value("artifactHeight").toInt(), artifactTopDown(obj));
        artifact.realBackground = loadRawRgba(realBackgroundPath,
                                              obj.value("realBackgroundWidth").toInt(),
                                              obj.value("realBackgroundHeight").toInt(),
                                              obj.contains("realBackgroundTopDown") ? obj.value("realBackgroundTopDown").toBool(true) : artifactTopDown(obj));
        artifactFiles.push_back(artifactPath);
        artifactFiles.push_back(realBackgroundPath);
        if (artifact.fullGeometry.isValid() && !artifact.image.isNull())
            m_windowArtifacts.push_back(std::move(artifact));
    }

    cleanupArtifactFiles(artifactFiles);
}

void CaptureOverlay::captureScreensBeforeOverlay() {
    if (!m_desktopGeometry.isValid())
        for (const auto* screen : QGuiApplication::screens())
            m_desktopGeometry = m_desktopGeometry.united(screen->geometry());

    if (!m_desktopGeometry.isValid())
        return;

    if (!m_monitorArtifacts.empty()) {
        double scaleX = 1.0;
        double scaleY = 1.0;
        for (const auto& artifact : m_monitorArtifacts) {
            if (!artifact.image.isNull() && artifact.logicalGeometry.isValid()) {
                scaleX = std::max(scaleX, static_cast<double>(artifact.image.width()) / std::max(1, artifact.logicalGeometry.width()));
                scaleY = std::max(scaleY, static_cast<double>(artifact.image.height()) / std::max(1, artifact.logicalGeometry.height()));
            }
        }

        const QSize imageSize = boundedScaledSize(m_desktopGeometry.width(), m_desktopGeometry.height(), scaleX, scaleY);
        m_desktopImage = boundedImage(imageSize, QImage::Format_RGBA8888);
        if (m_desktopImage.isNull())
            return;
        m_desktopImage.fill(QColor(30, 34, 38));

        QPainter painter(&m_desktopImage);
        for (const auto& artifact : m_monitorArtifacts) {
            const QRect target = logicalRectToOutputRect(artifact.logicalGeometry, m_desktopGeometry, scaleX, scaleY).intersected(m_desktopImage.rect());
            if (target.isValid())
                painter.drawImage(target, artifact.image);
        }
        return;
    }

    const QString grimProgram = hyprcapture::ui::trustedSystemProgram(QStringLiteral("grim"));
    QProcess      grim;
    if (!grimProgram.isEmpty()) {
        grim.setProcessEnvironment(hyprcapture::ui::trustedProcessEnvironment());
        grim.start(grimProgram, {"-t", "png", "-"});
    }
    if (grim.waitForFinished(1500) && grim.exitStatus() == QProcess::NormalExit && grim.exitCode() == 0) {
        QImage grimImage;
        if (grimImage.loadFromData(grim.readAllStandardOutput(), "PNG") && !grimImage.isNull()) {
            const QImage converted = grimImage.convertToFormat(QImage::Format_RGBA8888);
            if (imageSizeWithinBounds(converted.size())) {
                m_desktopImage = converted;
                return;
            }
        }
    }

    const double scale = maxScreenDevicePixelRatio();
    const QSize imageSize = boundedScaledSize(m_desktopGeometry.width(), m_desktopGeometry.height(), scale, scale);
    m_desktopImage = boundedImage(imageSize, QImage::Format_RGBA8888);
    if (m_desktopImage.isNull())
        return;
    m_desktopImage.fill(QColor(30, 34, 38));

    QPainter painter(&m_desktopImage);
    const auto screens = QGuiApplication::screens();
    for (auto* screen : screens) {
        const QPixmap pixmap = screen->grabWindow(0);
        if (pixmap.isNull())
            continue;
        const QRect target = logicalRectToOutputRect(screen->geometry(), m_desktopGeometry, scale, scale).intersected(m_desktopImage.rect());
        if (target.isValid())
            painter.drawPixmap(target, pixmap);
    }
}

void CaptureOverlay::buildToolbar() {
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName("toolbar");
    m_toolbar->setAttribute(Qt::WA_StyledBackground);
    m_toolbar->setStyleSheet(toolbarStyleSheet(QApplication::palette()));
    m_toolbarOpacity = new QGraphicsOpacityEffect(m_toolbar);
    m_toolbarOpacity->setOpacity(m_overlayOpacity);
    m_toolbar->setGraphicsEffect(m_toolbarOpacity);

    auto* layout = new QHBoxLayout(m_toolbar);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    auto* group = new QButtonGroup(this);
    const auto addMode = [&](const QString& tooltip, hyprcapture::CaptureMode mode, const QIcon& icon) {
        auto* button = new QPushButton(m_toolbar);
        button->setObjectName("captureModeButton");
        button->setFlat(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIcon(icon);
        button->setIconSize(QSize(kModeIconSize, kModeIconSize));
        button->setFixedSize(36, 32);
        button->setToolTip(tooltip);
        button->setAccessibleName(tooltip);
        button->setCheckable(true);
        button->setChecked(mode == m_mode);
        group->addButton(button);
        layout->addWidget(button);
        if (m_defaults.fushionMode && mode != hyprcapture::CaptureMode::Fullscreen) {
            button->hide();
            button->setFixedSize(0, 0);
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        }
        connect(button, &QPushButton::clicked, this, [this, mode] {
            const bool wasActiveMode = m_mode == mode;
            setMode(mode);
            if (mode == hyprcapture::CaptureMode::Fullscreen && (m_defaults.fushionMode || wasActiveMode))
                finishCapture();
        });
    };
    addMode("Fullscreen", hyprcapture::CaptureMode::Fullscreen, iconFromSvg(kFullscreenSvg));
    addMode("Region", hyprcapture::CaptureMode::Region, iconFromSvg(kRegionSvg));
    addMode("Window", hyprcapture::CaptureMode::Window, iconFromSvg(kWindowSvg));

    m_fullscreenScope = new InlineSelect(this, m_toolbar);
    m_fullscreenScope->setPrefix("Full");
    m_fullscreenScope->addItems(QStringList{"all", "current", "per-monitor"});
    m_fullscreenScope->setCurrentText(qString(hyprcapture::toString(m_defaults.fullscreenScope)));
    layout->addWidget(m_fullscreenScope);

    m_windowBackground = new InlineSelect(this, m_toolbar);
    m_windowBackground->setPrefix("Bg");
    m_windowBackground->addItems(QStringList{"follow-system", "white", "black", "real", "transparent"});
    m_windowBackground->setCurrentText(qString(hyprcapture::toString(m_defaults.windowBackground)));
    layout->addWidget(m_windowBackground);

    auto* cancel = new QPushButton(m_toolbar);
    cancel->setFlat(true);
    cancel->setFocusPolicy(Qt::NoFocus);
    cancel->setIcon(iconFromSvg(kCancelSvg));
    cancel->setIconSize(QSize(kCancelIconSize, kCancelIconSize));
    cancel->setFixedSize(36, 32);
    cancel->setToolTip("Cancel");
    cancel->setAccessibleName("Cancel");
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this, &CaptureOverlay::cancelCapture);

    m_status = new QLabel(m_toolbar);
    m_status->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(m_status);
    updateToolbarControlsForMode();
    updateStatus();
    relayoutToolbar();
}

double CaptureOverlay::overlayOpacity() const {
    return m_overlayOpacity;
}

void CaptureOverlay::setOverlayOpacity(double opacity) {
    m_overlayOpacity = std::clamp(opacity, 0.0, 1.0);
    if (m_toolbarOpacity)
        m_toolbarOpacity->setOpacity(m_overlayOpacity);
    update();
}

void CaptureOverlay::runOverlayFade(double start, double end, std::function<void()> finished) {
    if (m_fadeAnimation) {
        m_fadeAnimation->stop();
        m_fadeAnimation->deleteLater();
        m_fadeAnimation = nullptr;
    }

    setOverlayOpacity(start);

    auto* animation = new QPropertyAnimation(this, "overlayOpacity", this);
    m_fadeAnimation = animation;
    animation->setDuration(kOverlayFadeDurationMs);
    animation->setStartValue(start);
    animation->setEndValue(end);
    animation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this, animation, finished = std::move(finished)]() mutable {
        if (m_fadeAnimation == animation)
            m_fadeAnimation = nullptr;
        animation->deleteLater();
        if (finished)
            finished();
    });
    animation->start();
}

void CaptureOverlay::startFadeIn() {
    runOverlayFade(m_overlayOpacity, 1.0, {});
}

void CaptureOverlay::fadeOutThen(std::function<void()> finished) {
    if (m_fadeOutStarted)
        return;
    m_fadeOutStarted = true;

    hideOptionPopups();

    runOverlayFade(m_overlayOpacity, 0.0, [this, finished = std::move(finished)]() mutable {
        hide();
        if (finished)
            finished();
    });
}

void CaptureOverlay::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!m_fadeOutStarted && m_overlayOpacity < 1.0)
        startFadeIn();
    QTimer::singleShot(0, this, &CaptureOverlay::refreshInitialCursorPosition);
}

void CaptureOverlay::hideOptionPopups() {
    if (m_fullscreenScope)
        m_fullscreenScope->hidePopup();
    if (m_windowBackground)
        m_windowBackground->hidePopup();
}

void CaptureOverlay::setMode(hyprcapture::CaptureMode mode) {
    hideOptionPopups();

    m_mode = mode;
    updateToolbarControlsForMode();
    updateStatus();
    update();
}

void CaptureOverlay::updateToolbarControlsForMode() {
    if (m_fullscreenScope) {
        const bool visible = hasMultipleMonitors() && (m_defaults.fushionMode || m_mode == hyprcapture::CaptureMode::Fullscreen);
        m_fullscreenScope->setControlVisible(visible);
    }

    if (m_windowBackground) {
        const bool visible = m_defaults.fushionMode || m_mode == hyprcapture::CaptureMode::Window;
        m_windowBackground->setControlVisible(visible);
    }

    relayoutToolbar();
}

int CaptureOverlay::monitorCount() const {
    const int qtScreens = QGuiApplication::screens().size();
    if (qtScreens > 0)
        return qtScreens;
    return m_sessionMonitorCount;
}

bool CaptureOverlay::hasMultipleMonitors() const {
    return monitorCount() > 1;
}

hyprcapture::FullscreenScope CaptureOverlay::currentFullscreenScope() const {
    if (!m_fullscreenScope || !hasMultipleMonitors())
        return hyprcapture::FullscreenScope::All;
    return hyprcapture::parseFullscreenScope(m_fullscreenScope->currentText().toStdString(), m_defaults.fullscreenScope);
}

hyprcapture::WindowBackground CaptureOverlay::currentWindowBackground() const {
    if (!m_windowBackground)
        return m_defaults.windowBackground;
    return hyprcapture::parseWindowBackground(m_windowBackground->currentText().toStdString(), m_defaults.windowBackground);
}

hyprcapture::DecorationPolicy CaptureOverlay::currentWindowBorder() const {
    return m_defaults.windowBorder;
}

hyprcapture::DecorationPolicy CaptureOverlay::currentWindowShadow() const {
    return m_defaults.windowShadow;
}

void CaptureOverlay::paintDesktop(QPainter& painter, const QRect& target) const {
    if (!target.isValid())
        return;

    if (!m_monitorArtifacts.empty()) {
        painter.save();
        painter.setClipRect(target);
        painter.fillRect(target, QColor(30, 34, 38));

        const QRect globalTarget(QPoint(mapToGlobal(target.topLeft())), target.size());
        for (const auto& artifact : m_monitorArtifacts) {
            if (artifact.image.isNull() || !artifact.logicalGeometry.isValid())
                continue;

            const QRect logicalPart = globalTarget.intersected(artifact.logicalGeometry);
            if (!logicalPart.isValid())
                continue;

            const QRect source = logicalRectToImageRect(logicalPart, artifact.logicalGeometry, artifact.image.size());
            const QRect localTarget = globalToLocalRect(logicalPart).intersected(target);
            if (source.isValid() && localTarget.isValid())
                painter.drawImage(localTarget, artifact.image, source);
        }
        painter.restore();
        return;
    }

    if (m_desktopImage.isNull()) {
        painter.fillRect(target, QColor(30, 34, 38));
        return;
    }

    const QRect source = localToDesktopSourceRect(target);
    if (source.isValid())
        painter.drawImage(target, m_desktopImage, source);
    else
        painter.fillRect(target, QColor(30, 34, 38));
}

void CaptureOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setOpacity(m_overlayOpacity);

    paintDesktop(painter, rect());
    painter.fillRect(rect(), QColor(0, 0, 0, 80));

    const bool fusionGesture = m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen;
    const QRect sel = normalizedSelection().intersected(regionCaptureBounds());
    const bool selectionVisible = m_dragging || (sel.width() > 4 && sel.height() > 4);
    if ((m_mode == hyprcapture::CaptureMode::Region || fusionGesture) && selectionVisible) {
        paintDesktop(painter, sel);
        painter.setPen(QPen(QColor(255, 255, 255, 230), 2));
        painter.drawRect(sel.adjusted(0, 0, -1, -1));
    } else if (m_mode == hyprcapture::CaptureMode::Window || fusionGesture) {
        const auto* window = hoveredWindow();
        for (const auto& candidate : m_windowArtifacts) {
            const QRect target = globalToLocalRect(windowFrameGeometry(candidate));
            const int penWidth = &candidate == window ? 3 : 1;
            QPen pen(&candidate == window ? QColor(255, 255, 255, 240) : QColor(255, 255, 255, 110), penWidth);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const QRectF alignedTarget = QRectF(target).adjusted(0.5, 0.5, -0.5, -0.5);
            painter.drawPath(roundedWindowFramePath(alignedTarget, windowFrameRadius(candidate), candidate.roundingPower));
        }
    }
}

void CaptureOverlay::mousePressEvent(QMouseEvent* event) {
    rememberCursorPosition(event->globalPosition());
    if (m_toolbar->geometry().contains(event->pos()))
        return;
    hideOptionPopups();
    if (event->button() != Qt::LeftButton)
        return;

    if (m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen) {
        m_mode = hyprcapture::CaptureMode::Region;
        m_dragging = true;
        m_dragStart = event->pos();
        m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
        updateStatus();
        update();
        return;
    }

    if (m_mode == hyprcapture::CaptureMode::Fullscreen) {
        finishCapture();
        return;
    }

    if (m_mode == hyprcapture::CaptureMode::Window)
        return;

    m_dragging = true;
    m_dragStart = event->pos();
    m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
    update();
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent* event) {
    rememberCursorPosition(event->globalPosition());
    if (m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen) {
        if (m_dragging)
            m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
        updateStatus();
        update();
        return;
    }

    if (m_mode == hyprcapture::CaptureMode::Window) {
        updateStatus();
        update();
        return;
    }
    if (!m_dragging)
        return;
    m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
    update();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent* event) {
    rememberCursorPosition(event->globalPosition());
    if (event->button() != Qt::LeftButton)
        return;

    if (m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen) {
        if (!m_dragging)
            return;

        m_dragging = false;
        m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
        const QRect selection = normalizedSelection().intersected(regionCaptureBounds());
        if (selection.width() > 4 && selection.height() > 4) {
            m_mode = hyprcapture::CaptureMode::Region;
            finishCapture();
            return;
        }

        if (hoveredWindow()) {
            m_mode = hyprcapture::CaptureMode::Window;
            finishCapture();
            return;
        }

        updateStatus();
        update();
        return;
    }

    if (m_mode == hyprcapture::CaptureMode::Window) {
        finishCapture();
        return;
    }
    m_dragging = false;
    m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
    const QRect selection = normalizedSelection().intersected(regionCaptureBounds());
    if (m_mode != hyprcapture::CaptureMode::Region || (selection.width() > 4 && selection.height() > 4))
        finishCapture();
}

void CaptureOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        cancelCapture();
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen) {
            const QRect selection = normalizedSelection().intersected(regionCaptureBounds());
            if (selection.width() > 4 && selection.height() > 4) {
                m_mode = hyprcapture::CaptureMode::Region;
            } else if (hoveredWindow()) {
                m_mode = hyprcapture::CaptureMode::Window;
            } else {
                return;
            }
        }
        finishCapture();
    }
}

void CaptureOverlay::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    relayoutToolbar();
}

QRect CaptureOverlay::normalizedSelection() const {
    return QRect(m_dragStart, m_dragEnd).normalized();
}

QRect CaptureOverlay::captureRectForMode() const {
    if (m_mode == hyprcapture::CaptureMode::Region && normalizedSelection().isValid())
        return normalizedSelection().intersected(regionCaptureBounds());
    if (m_mode == hyprcapture::CaptureMode::Window) {
        if (const auto* window = hoveredWindow())
            return globalToLocalRect(windowFrameGeometry(*window));
        return {};
    }
    return fullscreenCaptureRect();
}

QRect CaptureOverlay::fullscreenCaptureRect() const {
    if (currentFullscreenScope() == hyprcapture::FullscreenScope::Current)
        return localScreenRectAt(mapFromGlobal(cursorLogicalPosition()));
    return rect();
}

QRect CaptureOverlay::regionCaptureBounds() const {
    return rect();
}

QRect CaptureOverlay::localScreenRectAt(const QPoint& localPos) const {
    QScreen* screen = QGuiApplication::screenAt(mapToGlobal(localPos));
    if (!screen)
        screen = QGuiApplication::screenAt(cursorLogicalPosition());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    return screen ? globalToLocalRect(screen->geometry()).intersected(rect()) : rect();
}

QPoint CaptureOverlay::clampedToRect(const QPoint& point, const QRect& bounds) const {
    if (!bounds.isValid())
        return point;
    return QPoint(std::clamp(point.x(), bounds.left(), bounds.right()), std::clamp(point.y(), bounds.top(), bounds.bottom()));
}

QRect CaptureOverlay::globalToLocalRect(const QRect& rect) const {
    return QRect(mapFromGlobal(rect.topLeft()), rect.size());
}

QRect CaptureOverlay::desktopSourceRectForGlobalRect(const QRect& rect) const {
    if (m_desktopImage.isNull() || !m_desktopGeometry.isValid() || !rect.isValid())
        return {};

    const QRect clipped = rect.intersected(m_desktopGeometry);
    if (!clipped.isValid())
        return {};

    const double scaleX = static_cast<double>(m_desktopImage.width()) / std::max(1, m_desktopGeometry.width());
    const double scaleY = static_cast<double>(m_desktopImage.height()) / std::max(1, m_desktopGeometry.height());
    return logicalRectToOutputRect(clipped, m_desktopGeometry, scaleX, scaleY).intersected(m_desktopImage.rect());
}

QRect CaptureOverlay::localToDesktopSourceRect(const QRect& rect) const {
    return desktopSourceRectForGlobalRect(QRect(QPoint(mapToGlobal(rect.topLeft())), rect.size()));
}

QPoint CaptureOverlay::cursorLogicalPosition() const {
    if (m_hasCursorLogicalPosition)
        return m_cursorLogicalPosition;
    return mapToGlobal(mapFromGlobal(QCursor::pos()));
}

void CaptureOverlay::rememberCursorPosition(const QPointF& globalPosition) {
    int x = 0;
    int y = 0;
    if (!boundedDoubleToInt(globalPosition.x(), -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, x) ||
        !boundedDoubleToInt(globalPosition.y(), -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, y))
        return;

    m_cursorLogicalPosition = QPoint(x, y);
    m_hasCursorLogicalPosition = true;
}

void CaptureOverlay::refreshInitialCursorPosition() {
    if (!m_hasCursorLogicalPosition)
        rememberCursorPosition(QCursor::pos());
    updateStatus();
    update();
}

QRect CaptureOverlay::windowFrameGeometry(const WindowArtifact& window) const {
    QRect frame = window.visibleGeometry.isValid() ? window.visibleGeometry : window.fullGeometry;
    const int border = std::max(0, static_cast<int>(std::lround(window.borderSize)));
    if (border > 0)
        frame = frame.adjusted(-border, -border, border, border);
    return frame;
}

double CaptureOverlay::windowFrameRadius(const WindowArtifact& window) const {
    if (window.rounding <= 0.0)
        return 0.0;

    const double power = std::clamp(window.roundingPower, 1.0, 10.0);
    const double border = std::max(0.0, window.borderSize);
    const double correction = border * (std::sqrt(2.0) - 1.0) * std::max(2.0 - power, 0.0);
    return std::max(0.0, window.rounding + border - correction);
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

    const auto setStatusText = [this](const QString& text) {
        if (text.isEmpty()) {
            m_status->clear();
            m_status->hide();
            m_status->setFixedSize(0, 0);
            m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
            return;
        }

        m_status->setVisible(true);
        m_status->setMinimumSize(0, 0);
        m_status->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_status->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_status->setText(text);
        m_status->adjustSize();
    };

    if (m_mode != hyprcapture::CaptureMode::Window) {
        setStatusText({});
        relayoutToolbar();
        return;
    }

    if (!windowCaptureAvailable()) {
        if (m_sessionMonitorCount == 0 && m_sessionWindowCount == 0)
            setStatusText("plugin reload needed");
        else if (m_sessionWindowCount > 0)
            setStatusText("window artifact failed");
        else
            setStatusText("no visible windows");
        relayoutToolbar();
        return;
    }

    if (hoveredWindow())
        setStatusText({});
    else
        setStatusText("choose window");
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
    const int y = std::max(16, height() - m_toolbar->height() - 40);
    m_toolbar->move(std::max(16, (width() - m_toolbar->width()) / 2), y);
}

QImage CaptureOverlay::renderDesktopRectAtDisplayResolution(const QRect& globalRect) const {
    if (!globalRect.isValid() || m_monitorArtifacts.empty())
        return {};

    double scaleX = 0.0;
    double scaleY = 0.0;
    for (const auto& artifact : m_monitorArtifacts) {
        if (artifact.image.isNull() || !artifact.logicalGeometry.isValid() || !artifact.logicalGeometry.intersects(globalRect))
            continue;

        scaleX = std::max(scaleX, static_cast<double>(artifact.image.width()) / std::max(1, artifact.logicalGeometry.width()));
        scaleY = std::max(scaleY, static_cast<double>(artifact.image.height()) / std::max(1, artifact.logicalGeometry.height()));
    }
    if (scaleX <= 0.0 || scaleY <= 0.0)
        return {};

    const QSize outputSize = boundedScaledSize(globalRect.width(), globalRect.height(), scaleX, scaleY);
    QImage image = boundedImage(outputSize, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    image.fill(QColor(30, 34, 38));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    for (const auto& artifact : m_monitorArtifacts) {
        if (artifact.image.isNull() || !artifact.logicalGeometry.isValid())
            continue;

        const QRect logicalPart = globalRect.intersected(artifact.logicalGeometry);
        if (!logicalPart.isValid())
            continue;

        const QRect source = logicalRectToImageRect(logicalPart, artifact.logicalGeometry, artifact.image.size());
        const QRect target = logicalRectToOutputRect(logicalPart, globalRect, scaleX, scaleY).intersected(image.rect());
        if (source.isValid() && target.isValid())
            painter.drawImage(target, artifact.image, source);
    }

    return image;
}

QImage CaptureOverlay::renderResultImage() const {
    const auto bg = currentWindowBackground();
    if (m_mode == hyprcapture::CaptureMode::Window) {
        const auto* windowArtifact = hoveredWindow();
        if (!windowArtifact || windowArtifact->image.isNull())
            return {};

        QRect artifactSource = windowArtifact->image.rect();
        const bool cropDecorations = currentWindowBorder() == hyprcapture::DecorationPolicy::Remove || currentWindowShadow() == hyprcapture::DecorationPolicy::Remove;
        if (cropDecorations && windowArtifact->visibleGeometry.isValid() && windowArtifact->fullGeometry.contains(windowArtifact->visibleGeometry)) {
            const double scaleX = static_cast<double>(windowArtifact->image.width()) / std::max(1, windowArtifact->fullGeometry.width());
            const double scaleY = static_cast<double>(windowArtifact->image.height()) / std::max(1, windowArtifact->fullGeometry.height());
            artifactSource = QRect(QPoint(static_cast<int>(std::floor((windowArtifact->visibleGeometry.x() - windowArtifact->fullGeometry.x()) * scaleX)),
                                          static_cast<int>(std::floor((windowArtifact->visibleGeometry.y() - windowArtifact->fullGeometry.y()) * scaleY))),
                                   QSize(std::max(1, static_cast<int>(std::ceil(windowArtifact->visibleGeometry.width() * scaleX))),
                                         std::max(1, static_cast<int>(std::ceil(windowArtifact->visibleGeometry.height() * scaleY)))))
                                 .intersected(windowArtifact->image.rect());
        }

        QImage repairedArtifact = windowArtifact->image;
        repairMissingWindowTail(repairedArtifact, windowArtifact->fullGeometry, windowArtifact->visibleGeometry, m_desktopImage, m_desktopGeometry);

        QImage image = boundedImage(artifactSource.size().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            return {};
        image.fill(Qt::transparent);

        QPainter painter(&image);
        QImage background = boundedImage(image.size(), QImage::Format_RGBA8888);
        if (background.isNull())
            return {};
        background.fill(Qt::transparent);
        const QRect logicalSource = artifactRectToLogicalRect(artifactSource, repairedArtifact.size(), windowArtifact->fullGeometry);
        const QRect desktopSource = desktopSourceRectForGlobalRect(logicalSource);
        const QImage maskArtifact = repairedArtifact.format() == QImage::Format_RGBA8888 ? repairedArtifact : repairedArtifact.convertToFormat(QImage::Format_RGBA8888);
        bool         paintedBackground = false;
        if (bg == hyprcapture::WindowBackground::Real && !windowArtifact->realBackground.isNull() && logicalSource.isValid()) {
            const QRect backgroundSource = projectedImageRect(logicalSource, windowArtifact->fullGeometry, windowArtifact->realBackground.size());
            if (backgroundSource.isValid()) {
                QPainter backgroundPainter(&background);
                backgroundPainter.drawImage(background.rect(), windowArtifact->realBackground, backgroundSource);
                paintedBackground = true;
            }
        }
        if (!paintedBackground && paintWindowBackground(background, bg, m_desktopImage, desktopSource)) {
            if (bg == hyprcapture::WindowBackground::Real)
                reconstructRealWindowBackground(background, maskArtifact, artifactSource);
            paintedBackground = true;
        }
        if (paintedBackground) {
            applyWindowContentAlphaMask(background, maskArtifact, artifactSource);
            painter.drawImage(QPoint(0, 0), background);
        }

        painter.drawImage(image.rect(), repairedArtifact, artifactSource);
        return image;
    }

    const QRect cap = captureRectForMode();
    if (!cap.isValid())
        return {};

    if (!m_monitorArtifacts.empty()) {
        const QImage highResolution = renderDesktopRectAtDisplayResolution(QRect(mapToGlobal(cap.topLeft()), cap.size()));
        if (!highResolution.isNull())
            return highResolution;
    }

    const QRect desktopSource = localToDesktopSourceRect(cap);
    const QSize outputSize = desktopSource.isValid() ? desktopSource.size() : cap.size();
    QImage image = boundedImage(outputSize.expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    image.fill(Qt::transparent);

    QPainter painter(&image);

    if (m_mode != hyprcapture::CaptureMode::Window && !m_desktopImage.isNull() && desktopSource.isValid()) {
        painter.drawImage(image.rect(), m_desktopImage, desktopSource);
    } else if (m_mode == hyprcapture::CaptureMode::Window && bg == hyprcapture::WindowBackground::Real && !m_desktopImage.isNull()) {
        painter.drawImage(image.rect(), m_desktopImage, desktopSource);
    } else if (m_mode == hyprcapture::CaptureMode::Window && bg != hyprcapture::WindowBackground::Transparent) {
        if (bg == hyprcapture::WindowBackground::White)
            painter.fillRect(image.rect(), Qt::white);
        else if (bg == hyprcapture::WindowBackground::Black)
            painter.fillRect(image.rect(), Qt::black);
        else if (bg == hyprcapture::WindowBackground::FollowSystem)
            painter.fillRect(image.rect(), followSystemColor());
        else
            painter.fillRect(image.rect(), QColor(30, 34, 38));
    } else {
        painter.fillRect(image.rect(), QColor(30, 34, 38));
    }

    return image;
}

void CaptureOverlay::finishCapture() {
    if (m_finishing)
        return;
    m_finishing = true;

    QElapsedTimer renderTimer;
    renderTimer.start();
    auto image = renderResultImage();
    traceTiming(QStringLiteral("render_result"), renderTimer.elapsed());
    if (image.isNull()) {
        m_finishing = false;
        updateStatus();
        return;
    }

    QElapsedTimer watermarkTimer;
    watermarkTimer.start();
    hyprcapture::ui::applyWatermark(image, m_defaults);
    traceTiming(QStringLiteral("apply_watermark"), watermarkTimer.elapsed());

    hyprcapture::ui::ClipboardSnapshotData clipboardSnapshot;
    if (m_defaults.clipboard && m_defaults.showThumbnail) {
        QElapsedTimer snapshotTimer;
        snapshotTimer.start();
        clipboardSnapshot = hyprcapture::ui::captureClipboardSnapshotData();
        traceTiming(QStringLiteral("clipboard_snapshot_collect"), snapshotTimer.elapsed());
    }

    saveImage(image, clipboardSnapshot);
    traceTiming(QStringLiteral("fade_start"));
    fadeOutThen({});
}

void CaptureOverlay::saveImage(const QImage& image, hyprcapture::ui::ClipboardSnapshotData clipboardSnapshot) {
    const auto defaults = m_defaults;
    auto*      worker = QThread::create([this, image, defaults, clipboardSnapshot = std::move(clipboardSnapshot)] {
        QElapsedTimer totalTimer;
        totalTimer.start();
        const CaptureOutputResult result = writeCaptureOutput(image, defaults, clipboardSnapshot);
        traceTiming(QStringLiteral("output_worker_total"), totalTimer.elapsed());
        QMetaObject::invokeMethod(
            this,
            [this, image, result] {
                traceTiming(QStringLiteral("output_ready"));
                if (result.clipboardRequested && !result.clipboardCopied)
                    hyprcapture::ui::copyImageToClipboard(image);
                if (result.showThumbnail) {
                    showThumbnail(image, result.savedPath, result.restoreClipboardPath);
                    return;
                }
                qApp->quit();
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void CaptureOverlay::showThumbnail(const QImage& image, const QString& path, const QString& restoreClipboardPath) {
    QString thumbPath = path;
    if (thumbPath.isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        thumbPath = hyprcapture::ui::runtimeFile("thumbnail", ".png");
        hyprcapture::ui::savePrivatePng(image, thumbPath);
        traceTiming(QStringLiteral("thumbnail_late_save"), timer.elapsed());
    }

    QStringList args{"--thumbnail-window", thumbPath, "--thumbnail-timeout-ms", QString::number(m_defaults.thumbnailTimeoutMs)};
    if (!restoreClipboardPath.isEmpty())
        args << "--restore-clipboard" << restoreClipboardPath;
    QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
    traceTiming(QStringLiteral("thumbnail_started"));
    qApp->quit();
}

void CaptureOverlay::cancelCapture() {
    fadeOutThen([] { qApp->quit(); });
}
