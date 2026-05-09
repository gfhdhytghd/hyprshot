#include "shared/config.hpp"
#include "ui/capture_overlay.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/result_thumbnail.hpp"

#include <LayerShellQt/Shell>

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPixmap>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr qint64 MAX_SESSION_JSON_BYTES = 8LL * 1024LL * 1024LL;
constexpr qint64 MAX_THUMBNAIL_SOURCE_BYTES = 128LL * 1024LL * 1024LL;
constexpr int    MAX_THUMBNAIL_DIMENSION = 32768;
constexpr int    THUMBNAIL_DECODE_MAX_WIDTH = 360;
constexpr int    THUMBNAIL_DECODE_MAX_HEIGHT = 240;
constexpr int    MAX_THUMBNAIL_TIMEOUT_MS = 60 * 60 * 1000;

bool flagValue(const QCommandLineParser& parser, const QString& name, bool fallback) {
    const auto value = parser.value(name);
    if (value.isEmpty())
        return fallback;
    return value != "0" && value != "false";
}

bool hasArgument(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String(name))
            return true;
    }
    return false;
}

int boundedInt(QString value, int fallback, int minimum, int maximum) {
    bool ok = false;
    const int parsed = value.toInt(&ok);
    if (!ok)
        return fallback;
    return std::clamp(parsed, minimum, maximum);
}

QPixmap loadThumbnailPixmap(const QString& path) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || !info.isFile() || !info.isReadable() || info.size() < 0 || info.size() > MAX_THUMBNAIL_SOURCE_BYTES)
        return {};

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize sourceSize = reader.size();
    if (sourceSize.isEmpty() || sourceSize.width() <= 0 || sourceSize.height() <= 0 || sourceSize.width() > MAX_THUMBNAIL_DIMENSION ||
        sourceSize.height() > MAX_THUMBNAIL_DIMENSION)
        return {};

    reader.setScaledSize(sourceSize.scaled(THUMBNAIL_DECODE_MAX_WIDTH, THUMBNAIL_DECODE_MAX_HEIGHT, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull())
        return {};
    return QPixmap::fromImage(image);
}

bool pathIsUnderDirectory(const QString& path, const QString& root) {
    const QString pathCanonical = QFileInfo(path).canonicalFilePath();
    const QString rootCanonical = QFileInfo(root).canonicalFilePath();
    return !pathCanonical.isEmpty() && !rootCanonical.isEmpty() && pathCanonical.startsWith(rootCanonical + QLatin1Char('/'));
}

bool trustedDirectory(const QString& path) {
    const QByteArray native = QFile::encodeName(QFileInfo(path).canonicalFilePath());
    struct stat      st {};
    return !native.isEmpty() && stat(native.constData(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_uid == 0 || st.st_uid == geteuid()) &&
        (st.st_mode & 0022) == 0;
}

bool isTrustedRecordingResultPath(const QString& path, const hyprcapture::CaptureDefaults& defaults) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || !info.isFile() || !info.isReadable() || info.ownerId() != geteuid())
        return false;

    const QByteArray native = QFile::encodeName(info.canonicalFilePath());
    struct stat      st {};
    if (native.isEmpty() || stat(native.constData(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022) != 0)
        return false;

    const QString recordRoot = QString::fromStdString(hyprcapture::expandUserPath(defaults.recordSaveDir).string());
    return trustedDirectory(recordRoot) && pathIsUnderDirectory(path, recordRoot);
}

QPixmap recordingThumbnailPixmap(const QString& path) {
    constexpr QSize logicalSize(180, 120);
    const qreal dpr = std::clamp(QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : qreal{1.0}, qreal{1.0}, qreal{4.0});
    QPixmap pixmap(QSize(static_cast<int>(std::ceil(logicalSize.width() * dpr)), static_cast<int>(std::ceil(logicalSize.height() * dpr))));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF rect(0.5, 0.5, logicalSize.width() - 1.0, logicalSize.height() - 1.0);
    painter.setPen(QPen(QColor(235, 238, 242, 95), 1.0));
    painter.setBrush(QColor(28, 31, 36, 238));
    painter.drawRoundedRect(rect, 8.0, 8.0);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(235, 238, 242, 235));
    const QPointF center(logicalSize.width() / 2.0, 48.0);
    QPolygonF play;
    play << QPointF(center.x() - 13.0, center.y() - 18.0) << QPointF(center.x() - 13.0, center.y() + 18.0) << QPointF(center.x() + 20.0, center.y());
    painter.drawPolygon(play);

    QFont font = painter.font();
    font.setPointSize(9);
    font.setStyleStrategy(QFont::PreferAntialias);
    painter.setFont(font);
    painter.setPen(QColor(235, 238, 242, 230));
    const QString name = painter.fontMetrics().elidedText(QFileInfo(path).fileName(), Qt::ElideMiddle, logicalSize.width() - 24);
    painter.drawText(QRect(12, 86, logicalSize.width() - 24, 22), Qt::AlignCenter, name);
    return pixmap;
}

int showRecordingResult(const hyprcapture::CaptureDefaults& defaults, const QString& path) {
    if (!isTrustedRecordingResultPath(path, defaults))
        return 1;

    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    QString       restoreClipboardPath;
    if (defaults.clipboard && defaults.showThumbnail)
        restoreClipboardPath = hyprcapture::ui::saveClipboardSnapshot();

    if (defaults.clipboard)
        hyprcapture::ui::copyFileUrlToClipboard(canonicalPath);

    if (!defaults.showThumbnail) {
        hyprcapture::ui::discardClipboardSnapshot(restoreClipboardPath);
        return 0;
    }

    const QString deleteRoot = QString::fromStdString(hyprcapture::expandUserPath(defaults.recordSaveDir).string());
    ResultThumbnail thumbnail(recordingThumbnailPixmap(canonicalPath),
                              canonicalPath,
                              restoreClipboardPath,
                              deleteRoot,
                              static_cast<int>(defaults.thumbnailTimeoutMs),
                              true);
    thumbnail.show();
    return qApp->exec();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
#if LAYERSHELLQTINTERFACE_ENABLE_DEPRECATED_SINCE(6, 6)
    LayerShellQt::Shell::useLayerShell();
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName("hyprcapture-ui");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {{"m", "mode"}, "Capture mode.", "mode", "region"},
        {"fullscreen-scope", "Fullscreen scope.", "scope", "all"},
        {"window-background", "Window background.", "background", "follow-system"},
        {"window-border", "Window border policy.", "policy", "keep"},
        {"window-shadow", "Window shadow policy.", "policy", "keep"},
        {"save", "Save output.", "0|1", "1"},
        {"clipboard", "Copy output.", "0|1", "1"},
        {"thumbnail", "Show thumbnail.", "0|1", "1"},
        {"include-cursor", "Include cursor.", "0|1", "0"},
        {{"fushion-mode", "fusion-mode"}, "Enable fushion toolbar behavior.", "0|1", "0"},
        {"save-dir", "Save directory.", "path", "$XDG_PICTURES_DIR/Screenshots"},
        {"filename-template", "Filename strftime template.", "template", "Screenshot-%Y-%m-%d-%H%M%S.png"},
        {"record-save-dir", "Recording save directory.", "path", "$XDG_VIDEOS_DIR/Screenrecords"},
        {"record-filename-template", "Recording filename strftime template.", "template", "Recording-%Y-%m-%d-%H%M%S.mp4"},
        {"record-format", "Recording container format.", "format", "mp4"},
        {"record-transparent-format", "Transparent window recording container format.", "format", "webm"},
        {"record-codec", "Recording codec.", "codec", "libx264"},
        {"record-transparent-codec", "Transparent window recording codec.", "codec", "auto"},
        {"record-solid-alpha", "Keep alpha outside follow-system/white/black window recording content when supported.", "0|1", "0"},
        {"record-preset", "Recording preset.", "preset", "veryfast"},
        {"record-gsr-flags", "Extra gpu-screen-recorder flags.", "flags", ""},
        {"record-window-backend", "Window recording backend.", "backend", "compositor"},
        {"record-fps", "Recording FPS.", "fps", "30"},
        {"record-fps-options", "Recording FPS choices.", "fps-list", "15 24 30 60"},
        {"record-window-fps-limit", "Window recording FPS cap.", "fps", "12"},
        {"record-window-real-bg-fps-limit", "Real background window recording FPS cap.", "fps", "8"},
        {"record-max-seconds", "Recording max seconds.", "seconds", "0"},
        {"thumbnail-timeout-ms", "Thumbnail timeout.", "ms", "5000"},
        {"watermark", "Watermark image path or built-in id.", "path|id", ""},
        {"watermark-position", "Watermark position.", "position", "central"},
        {"watermark-width", "Watermark width in pixels or screenshot-width percent.", "width", "20%"},
        {"watermark-offset", "Watermark x/y offset in pixels or percent.", "vec2", "0 0"},
        {"session-json", "Compositor session metadata.", "json", "{}"},
        {"session-json-file", "Private compositor session metadata file.", "path"},
        {"thumbnail-window", "Show a normal thumbnail window for an image path.", "path"},
        {"recording-result", "Handle a completed recording result.", "path"},
        {"thumbnail-delete-root", "Directory where thumbnail files may be deleted.", "path"},
        {"restore-clipboard", "Clipboard snapshot to restore when deleting the thumbnail image.", "path"},
        {"quick", "Capture immediately."},
        {"record", "Use the overlay to start a compositor-side recording."},
        {"record-active", "Show recording controls as active."},
    });
    parser.process(app);

    if (hasArgument(argc, argv, "--thumbnail-window")) {
        const QString thumbnailPath = parser.value("thumbnail-window");
        const QPixmap pixmap = loadThumbnailPixmap(thumbnailPath);
        if (pixmap.isNull())
            return 1;
        ResultThumbnail thumbnail(pixmap,
                                  thumbnailPath,
                                  parser.value("restore-clipboard"),
                                  parser.value("thumbnail-delete-root"),
                                  boundedInt(parser.value("thumbnail-timeout-ms"), 5000, 0, MAX_THUMBNAIL_TIMEOUT_MS));
        thumbnail.show();
        return app.exec();
    }

    hyprcapture::CaptureDefaults defaults;
    defaults.mode = hyprcapture::parseCaptureMode(parser.value("mode").toStdString(), defaults.mode);
    defaults.fullscreenScope = hyprcapture::parseFullscreenScope(parser.value("fullscreen-scope").toStdString(), defaults.fullscreenScope);
    defaults.windowBackground = hyprcapture::parseWindowBackground(parser.value("window-background").toStdString(), defaults.windowBackground);
    defaults.windowBorder = hyprcapture::parseDecorationPolicy(parser.value("window-border").toStdString(), defaults.windowBorder);
    defaults.windowShadow = hyprcapture::parseDecorationPolicy(parser.value("window-shadow").toStdString(), defaults.windowShadow);
    defaults.save = flagValue(parser, "save", defaults.save);
    defaults.clipboard = flagValue(parser, "clipboard", defaults.clipboard);
    defaults.showThumbnail = flagValue(parser, "thumbnail", defaults.showThumbnail);
    defaults.includeCursor = flagValue(parser, "include-cursor", defaults.includeCursor);
    defaults.fushionMode = flagValue(parser, "fushion-mode", defaults.fushionMode);
    defaults.saveDir = parser.value("save-dir").toStdString();
    defaults.filenameTemplate = parser.value("filename-template").toStdString();
    defaults.recordSaveDir = parser.value("record-save-dir").toStdString();
    defaults.recordFilenameTemplate = parser.value("record-filename-template").toStdString();
    defaults.recordFormat = parser.value("record-format").toStdString();
    defaults.recordTransparentFormat = parser.value("record-transparent-format").toStdString();
    defaults.recordCodec = parser.value("record-codec").toStdString();
    defaults.recordTransparentCodec = parser.value("record-transparent-codec").toStdString();
    defaults.recordSolidAlpha = flagValue(parser, "record-solid-alpha", defaults.recordSolidAlpha);
    defaults.recordPreset = parser.value("record-preset").toStdString();
    defaults.recordGsrFlags = parser.value("record-gsr-flags").toStdString();
    defaults.recordWindowBackend = hyprcapture::parseRecordWindowBackend(parser.value("record-window-backend").toStdString(), defaults.recordWindowBackend);
    defaults.recordFps = boundedInt(parser.value("record-fps"), defaults.recordFps, 1, 240);
    defaults.recordFpsOptions = parser.value("record-fps-options").toStdString();
    defaults.recordWindowFpsLimit = boundedInt(parser.value("record-window-fps-limit"), defaults.recordWindowFpsLimit, 0, 240);
    defaults.recordWindowRealBgFpsLimit = boundedInt(parser.value("record-window-real-bg-fps-limit"), defaults.recordWindowRealBgFpsLimit, 0, 240);
    defaults.recordMaxSeconds = boundedInt(parser.value("record-max-seconds"), defaults.recordMaxSeconds, 0, 24 * 60 * 60);
    defaults.thumbnailTimeoutMs = boundedInt(parser.value("thumbnail-timeout-ms"), defaults.thumbnailTimeoutMs, 0, MAX_THUMBNAIL_TIMEOUT_MS);
    defaults.watermark = parser.value("watermark").toStdString();
    defaults.watermarkPosition = hyprcapture::parseWatermarkPosition(parser.value("watermark-position").toStdString(), defaults.watermarkPosition);
    defaults.watermarkWidth = parser.value("watermark-width").toStdString();
    defaults.watermarkOffset = parser.value("watermark-offset").toStdString();

    if (hasArgument(argc, argv, "--recording-result"))
        return showRecordingResult(defaults, parser.value("recording-result"));

    QString sessionJson = parser.value("session-json");
    if (parser.isSet("session-json-file")) {
        const QString path = parser.value("session-json-file");
        QFile         file(path);
        if (hyprcapture::ui::isPrivateRuntimeFile(path, MAX_SESSION_JSON_BYTES) && file.open(QIODevice::ReadOnly))
            sessionJson = QString::fromUtf8(file.readAll());
        if (hyprcapture::ui::isPrivateRuntimePath(path))
            QFile::remove(path);
    }

    CaptureOverlay overlay(defaults, parser.isSet("quick"), parser.isSet("record"), parser.isSet("record-active"), sessionJson);
    overlay.show();
    overlay.activateWindow();
    overlay.raise();

    return app.exec();
}
