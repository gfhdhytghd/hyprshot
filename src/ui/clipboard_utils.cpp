#include "ui/clipboard_utils.hpp"

#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace hyprcapture::ui {
namespace {

QString wlCopyProgram() {
    return QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
}

bool copyBytesWithWlCopy(const QByteArray& bytes, const QString& mimeType) {
    if (bytes.isEmpty())
        return false;

    const QString program = wlCopyProgram();
    if (program.isEmpty())
        return false;

    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("--type"), mimeType});
    process.start();
    if (!process.waitForStarted(1000))
        return false;

    qsizetype written = 0;
    while (written < bytes.size()) {
        const qint64 chunk = process.write(bytes.constData() + written, bytes.size() - written);
        if (chunk <= 0) {
            process.kill();
            process.waitForFinished(500);
            return false;
        }
        written += chunk;
        if (!process.waitForBytesWritten(1000)) {
            process.kill();
            process.waitForFinished(500);
            return false;
        }
    }
    process.closeWriteChannel();

    if (!process.waitForFinished(2500)) {
        process.kill();
        process.waitForFinished(500);
        return false;
    }

    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool copyFileWithWlCopyDetached(const QString& path, const QString& mimeType, bool removeAfterCopy) {
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;

    const QString program = wlCopyProgram();
    if (program.isEmpty())
        return false;

    const QString shell = QStandardPaths::findExecutable(QStringLiteral("sh"));
    if (shell.isEmpty())
        return false;

    const QString script = QStringLiteral(R"("$1" --type "$2" < "$3"; status=$?; if [ "$4" = "1" ]; then rm -f "$3"; fi; exit $status)");
    return QProcess::startDetached(shell,
                                   {QStringLiteral("-c"),
                                    script,
                                    QStringLiteral("hyprcapture-wl-copy"),
                                    program,
                                    mimeType,
                                    path,
                                    removeAfterCopy ? QStringLiteral("1") : QStringLiteral("0")});
}

bool clearWithWlCopy() {
    const QString program = wlCopyProgram();
    if (program.isEmpty())
        return false;

    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("--clear")});
    process.start();
    if (!process.waitForStarted(1000))
        return false;
    if (!process.waitForFinished(2500)) {
        process.kill();
        process.waitForFinished(500);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QByteArray pngBytes(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    if (!image.save(&buffer, "PNG"))
        return {};
    return bytes;
}

bool copyImageToQtClipboard(const QImage& image) {
    auto* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return false;
    clipboard->setImage(image);
    return true;
}

void removeSnapshotFiles(const QJsonObject& obj, const QString& path) {
    if (obj.contains("image"))
        QFile::remove(obj.value("image").toString());
    QFile::remove(path);
}

} // namespace

QString runtimeFile(const QString& prefix, const QString& suffix) {
    QDir dir;
    bool found = false;
    for (const QString& root : {QStringLiteral("/dev/shm"), QDir::tempPath()}) {
        QDir candidate(root);
        if (!candidate.exists())
            continue;
        if ((candidate.exists("hyprcapture") || candidate.mkpath("hyprcapture")) && candidate.cd("hyprcapture")) {
            dir = candidate;
            found = true;
            break;
        }
    }
    if (!found)
        dir = QDir::tempPath();
    return dir.filePath(QStringLiteral("%1-%2%3").arg(prefix).arg(QDateTime::currentMSecsSinceEpoch()).arg(suffix));
}

QString saveClipboardSnapshot() {
    const auto* clipboard = QGuiApplication::clipboard();
    const auto* mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime)
        return {};

    QJsonObject root;
    root.insert("empty", true);
    if (mime->hasText()) {
        root.insert("text", mime->text());
        root.insert("empty", false);
    }
    if (mime->hasHtml()) {
        root.insert("html", mime->html());
        root.insert("empty", false);
    }
    if (mime->hasUrls()) {
        QJsonArray urls;
        for (const auto& url : mime->urls())
            urls.append(url.toString());
        root.insert("urls", urls);
        root.insert("empty", false);
    }
    if (mime->hasColor()) {
        const QColor color = qvariant_cast<QColor>(mime->colorData());
        if (color.isValid()) {
            root.insert("color", color.name(QColor::HexArgb));
            root.insert("empty", false);
        }
    }
    if (mime->hasImage()) {
        QImage image = qvariant_cast<QImage>(mime->imageData());
        if (image.isNull()) {
            const QPixmap pixmap = qvariant_cast<QPixmap>(mime->imageData());
            image = pixmap.toImage();
        }
        if (!image.isNull()) {
            const QString imagePath = runtimeFile("clipboard-image", ".png");
            if (image.save(imagePath, "PNG")) {
                root.insert("image", imagePath);
                root.insert("empty", false);
            }
        }
    }

    const QString path = runtimeFile("clipboard", ".json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return path;
}

bool copyImageToClipboard(const QImage& image) {
    if (image.isNull())
        return false;

    if (copyBytesWithWlCopy(pngBytes(image), QStringLiteral("image/png")))
        return true;

    return copyImageToQtClipboard(image);
}

bool copyImageToClipboardDetached(const QImage& image) {
    if (image.isNull())
        return false;

    const QString path = runtimeFile("clipboard-copy", ".png");
    if (image.save(path, "PNG")) {
        if (copyFileWithWlCopyDetached(path, QStringLiteral("image/png"), true))
            return true;
        QFile::remove(path);
    }

    return false;
}

bool copyImageFileToClipboardDetached(const QString& path) {
    return copyFileWithWlCopyDetached(path, QStringLiteral("image/png"), false);
}

bool copyPixmapToClipboard(const QPixmap& pixmap) {
    if (pixmap.isNull())
        return false;
    return copyImageToClipboard(pixmap.toImage());
}

void restoreClipboardSnapshot(const QString& path) {
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;

    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;

    const auto obj = doc.object();
    if (obj.value("empty").toBool(false)) {
        if (!clearWithWlCopy()) {
            auto* clipboard = QGuiApplication::clipboard();
            if (clipboard)
                clipboard->clear();
        }
        removeSnapshotFiles(obj, path);
        return;
    }

    if (obj.contains("image")) {
        QFile imageFile(obj.value("image").toString());
        if (imageFile.open(QIODevice::ReadOnly) && copyBytesWithWlCopy(imageFile.readAll(), QStringLiteral("image/png"))) {
            removeSnapshotFiles(obj, path);
            return;
        }
    }

    if (obj.contains("text") && copyBytesWithWlCopy(obj.value("text").toString().toUtf8(), QStringLiteral("text/plain"))) {
        removeSnapshotFiles(obj, path);
        return;
    }

    if (obj.contains("html") && copyBytesWithWlCopy(obj.value("html").toString().toUtf8(), QStringLiteral("text/html"))) {
        removeSnapshotFiles(obj, path);
        return;
    }

    if (obj.contains("urls")) {
        QString uriList;
        for (const auto value : obj.value("urls").toArray())
            uriList += value.toString() + QStringLiteral("\r\n");
        if (copyBytesWithWlCopy(uriList.toUtf8(), QStringLiteral("text/uri-list"))) {
            removeSnapshotFiles(obj, path);
            return;
        }
    }

    auto* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;

    auto* mime = new QMimeData;
    if (obj.contains("text"))
        mime->setText(obj.value("text").toString());
    if (obj.contains("html"))
        mime->setHtml(obj.value("html").toString());
    if (obj.contains("urls")) {
        QList<QUrl> urls;
        for (const auto value : obj.value("urls").toArray())
            urls.push_back(QUrl(value.toString()));
        mime->setUrls(urls);
    }
    if (obj.contains("color")) {
        const QColor color(obj.value("color").toString());
        if (color.isValid())
            mime->setColorData(color);
    }
    if (obj.contains("image")) {
        QImage image(obj.value("image").toString());
        if (!image.isNull())
            mime->setImageData(image);
    }
    clipboard->setMimeData(mime);
    removeSnapshotFiles(obj, path);
}

} // namespace hyprcapture::ui
