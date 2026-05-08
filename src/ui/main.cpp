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
#include <QPixmap>

#include <algorithm>

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
        {"save-dir", "Save directory.", "path", "~/Pictures/Screenshots"},
        {"filename-template", "Filename strftime template.", "template", "Screenshot-%Y-%m-%d-%H%M%S.png"},
        {"record-filename-template", "Recording filename strftime template.", "template", "Recording-%Y-%m-%d-%H%M%S.mp4"},
        {"record-format", "Recording container format.", "format", "mp4"},
        {"record-codec", "Recording codec.", "codec", "libx264"},
        {"record-preset", "Recording preset.", "preset", "veryfast"},
        {"record-gsr-flags", "Extra gpu-screen-recorder flags.", "flags", ""},
        {"record-window-backend", "Window recording backend.", "backend", "compositor"},
        {"record-fps", "Recording FPS.", "fps", "30"},
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
    defaults.recordFilenameTemplate = parser.value("record-filename-template").toStdString();
    defaults.recordFormat = parser.value("record-format").toStdString();
    defaults.recordCodec = parser.value("record-codec").toStdString();
    defaults.recordPreset = parser.value("record-preset").toStdString();
    defaults.recordGsrFlags = parser.value("record-gsr-flags").toStdString();
    defaults.recordWindowBackend = hyprcapture::parseRecordWindowBackend(parser.value("record-window-backend").toStdString(), defaults.recordWindowBackend);
    defaults.recordFps = boundedInt(parser.value("record-fps"), defaults.recordFps, 1, 240);
    defaults.recordWindowFpsLimit = boundedInt(parser.value("record-window-fps-limit"), defaults.recordWindowFpsLimit, 0, 240);
    defaults.recordWindowRealBgFpsLimit = boundedInt(parser.value("record-window-real-bg-fps-limit"), defaults.recordWindowRealBgFpsLimit, 0, 240);
    defaults.recordMaxSeconds = boundedInt(parser.value("record-max-seconds"), defaults.recordMaxSeconds, 0, 24 * 60 * 60);
    defaults.thumbnailTimeoutMs = boundedInt(parser.value("thumbnail-timeout-ms"), defaults.thumbnailTimeoutMs, 0, MAX_THUMBNAIL_TIMEOUT_MS);
    defaults.watermark = parser.value("watermark").toStdString();
    defaults.watermarkPosition = hyprcapture::parseWatermarkPosition(parser.value("watermark-position").toStdString(), defaults.watermarkPosition);
    defaults.watermarkWidth = parser.value("watermark-width").toStdString();
    defaults.watermarkOffset = parser.value("watermark-offset").toStdString();

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
