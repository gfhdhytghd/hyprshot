#include "shared/config.hpp"
#include "ui/capture_overlay.hpp"
#include "ui/result_thumbnail.hpp"

#include <LayerShellQt/Shell>

#include <QApplication>
#include <QCommandLineParser>
#include <QPixmap>

namespace {

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
        {"thumbnail-timeout-ms", "Thumbnail timeout.", "ms", "5000"},
        {"watermark", "Watermark image path or built-in id.", "path|id", ""},
        {"watermark-position", "Watermark position.", "position", "central"},
        {"watermark-width", "Watermark width in pixels or screenshot-width percent.", "width", "20%"},
        {"watermark-offset", "Watermark x/y offset in pixels or percent.", "vec2", "0 0"},
        {"session-json", "Compositor session metadata.", "json", "{}"},
        {"thumbnail-window", "Show a normal thumbnail window for an image path.", "path"},
        {"restore-clipboard", "Clipboard snapshot to restore when deleting the thumbnail image.", "path"},
        {"quick", "Capture immediately."},
    });
    parser.process(app);

    if (hasArgument(argc, argv, "--thumbnail-window")) {
        ResultThumbnail thumbnail(QPixmap(parser.value("thumbnail-window")),
                                  parser.value("thumbnail-window"),
                                  parser.value("restore-clipboard"),
                                  parser.value("thumbnail-timeout-ms").toInt());
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
    defaults.thumbnailTimeoutMs = parser.value("thumbnail-timeout-ms").toLongLong();
    defaults.watermark = parser.value("watermark").toStdString();
    defaults.watermarkPosition = hyprcapture::parseWatermarkPosition(parser.value("watermark-position").toStdString(), defaults.watermarkPosition);
    defaults.watermarkWidth = parser.value("watermark-width").toStdString();
    defaults.watermarkOffset = parser.value("watermark-offset").toStdString();

    CaptureOverlay overlay(defaults, parser.isSet("quick"), parser.value("session-json"));
    overlay.show();
    overlay.activateWindow();
    overlay.raise();

    return app.exec();
}
