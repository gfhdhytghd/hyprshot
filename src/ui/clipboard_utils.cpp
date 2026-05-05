#include "ui/clipboard_utils.hpp"

#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPixmap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QUrl>

#include <atomic>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace hyprcapture::ui {
namespace {

constexpr qint64 kMaxClipboardSnapshotBytes = 16 * 1024 * 1024;
constexpr qint64 kMaxClipboardImageBytes = 128 * 1024 * 1024;

bool isTrustedOwner(uid_t uid) {
    return uid == 0 || uid == geteuid();
}

bool hasWritableGroupOrOther(mode_t mode) {
    return (mode & 0022) != 0;
}

bool parentChainTrusted(const QString& path) {
    QFileInfo info(path);
    QDir      current = info.absoluteDir();
    while (!current.path().isEmpty()) {
        const QByteArray native = QFile::encodeName(current.absolutePath());
        struct stat      st {};
        if (stat(native.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || !isTrustedOwner(st.st_uid) || hasWritableGroupOrOther(st.st_mode))
            return false;
        if (current.isRoot())
            break;
        if (!current.cdUp())
            break;
    }
    return true;
}

QString trustedExecutablePath(const QString& path) {
    const QFileInfo info(path);
    const QString   canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isAbsolute() || !parentChainTrusted(canonical))
        return {};

    const QByteArray native = QFile::encodeName(canonical);
    struct stat      st {};
    if (stat(native.constData(), &st) != 0 || !S_ISREG(st.st_mode) || !isTrustedOwner(st.st_uid) || hasWritableGroupOrOther(st.st_mode))
        return {};
    if (access(native.constData(), X_OK) != 0)
        return {};
    return canonical;
}

QString trustedSystemProgramImpl(const QString& name) {
    for (const QString& dir : {QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin"), QStringLiteral("/bin")}) {
        const QString trusted = trustedExecutablePath(QDir(dir).filePath(name));
        if (!trusted.isEmpty())
            return trusted;
    }
    return {};
}

QString wlCopyProgram() {
    return trustedSystemProgramImpl(QStringLiteral("wl-copy"));
}

bool ensurePrivateDirectory(const QString& path) {
    const QByteArray native = QFile::encodeName(path);
    if (native.isEmpty())
        return false;

    if (mkdir(native.constData(), 0700) != 0 && errno != EEXIST)
        return false;

    struct stat st {};
    if (lstat(native.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        return false;

    if ((st.st_mode & 0777) != 0700 && chmod(native.constData(), 0700) != 0)
        return false;

    return true;
}

QString privateRuntimeRoot() {
    const QString rootName = QStringLiteral("hyprcapture-%1").arg(QString::number(static_cast<qulonglong>(geteuid())));
    QStringList   bases{QStringLiteral("/dev/shm"), QDir::tempPath()};
    bases.removeDuplicates();
    for (const QString& base : bases) {
        const QFileInfo baseInfo(base);
        if (!baseInfo.exists() || !baseInfo.isDir())
            continue;
        const QString path = QDir(base).filePath(rootName);
        if (ensurePrivateDirectory(path))
            return path;
    }
    return {};
}

bool pathIsInPrivateRuntimeRoot(const QString& path) {
    const QString root = privateRuntimeRoot();
    if (root.isEmpty())
        return false;

    const QString rootCanonical = QFileInfo(root).canonicalFilePath();
    const QString pathCanonical = QFileInfo(path).canonicalFilePath();
    if (rootCanonical.isEmpty() || pathCanonical.isEmpty())
        return false;

    return pathCanonical == rootCanonical || pathCanonical.startsWith(rootCanonical + QLatin1Char('/'));
}

bool privateRuntimeFileExists(const QString& path, qint64 maxSize) {
    if (path.isEmpty() || !pathIsInPrivateRuntimeRoot(path))
        return false;
    const QFileInfo info(path);
    return info.exists() && info.isFile() && info.isReadable() && info.size() >= 0 && info.size() <= maxSize;
}

bool allowEnvironmentName(const QString& name) {
    static const QSet<QString> allowed{
        QStringLiteral("HOME"),
        QStringLiteral("USER"),
        QStringLiteral("LOGNAME"),
        QStringLiteral("LANG"),
        QStringLiteral("XDG_RUNTIME_DIR"),
        QStringLiteral("XDG_CURRENT_DESKTOP"),
        QStringLiteral("XDG_SESSION_TYPE"),
        QStringLiteral("XDG_DATA_HOME"),
        QStringLiteral("XDG_CONFIG_HOME"),
        QStringLiteral("XDG_DATA_DIRS"),
        QStringLiteral("DESKTOP_SESSION"),
        QStringLiteral("WAYLAND_DISPLAY"),
        QStringLiteral("DISPLAY"),
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("QT_SCALE_FACTOR"),
        QStringLiteral("QT_AUTO_SCREEN_SCALE_FACTOR"),
        QStringLiteral("QT_ENABLE_HIGHDPI_SCALING"),
        QStringLiteral("HYPRCAPTURE_TIMING"),
        QStringLiteral("HYPRCAPTURE_TIMING_FILE"),
    };
    return allowed.contains(name) || name.startsWith(QStringLiteral("LC_"));
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
    process.setProcessEnvironment(trustedProcessEnvironment());
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

    const QString shell = trustedSystemProgramImpl(QStringLiteral("sh"));
    if (shell.isEmpty())
        return false;

    const QString script = QStringLiteral(R"("$1" --type "$2" < "$3"; status=$?; if [ "$4" = "1" ]; then rm -f "$3"; fi; exit $status)");
    QProcess process;
    process.setProgram(shell);
    process.setArguments({QStringLiteral("-c"),
                          script,
                          QStringLiteral("hyprcapture-wl-copy"),
                          program,
                          mimeType,
                          path,
                          removeAfterCopy ? QStringLiteral("1") : QStringLiteral("0")});
    process.setProcessEnvironment(trustedProcessEnvironment());
    return process.startDetached();
}

bool clearWithWlCopy() {
    const QString program = wlCopyProgram();
    if (program.isEmpty())
        return false;

    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("--clear")});
    process.setProcessEnvironment(trustedProcessEnvironment());
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
    QImageWriter writer(&buffer, "PNG");
    writer.setQuality(75);
    if (!writer.write(image))
        return {};
    return bytes;
}

bool writePrivateFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size()) {
        file.remove();
        return false;
    }
    return true;
}

bool savePng(const QImage& image, const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    QImageWriter writer(&file, "PNG");
    writer.setQuality(75);
    if (!writer.write(image)) {
        file.remove();
        return false;
    }
    return true;
}

bool copyImageToQtClipboard(const QImage& image) {
    auto* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return false;
    clipboard->setImage(image);
    return true;
}

void removeSnapshotFiles(const QJsonObject& obj, const QString& path) {
    if (obj.contains("image")) {
        const QString imagePath = obj.value("image").toString();
        if (pathIsInPrivateRuntimeRoot(imagePath))
            QFile::remove(imagePath);
    }
    if (pathIsInPrivateRuntimeRoot(path))
        QFile::remove(path);
}

} // namespace

QString runtimeFile(const QString& prefix, const QString& suffix) {
    static std::atomic_uint64_t counter{0};
    const QString root = privateRuntimeRoot();
    if (root.isEmpty())
        return {};
    return QDir(root).filePath(QStringLiteral("%1-%2-%3-%4%5")
                                   .arg(prefix)
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QDateTime::currentMSecsSinceEpoch())
                                   .arg(counter.fetch_add(1, std::memory_order_relaxed))
                                   .arg(suffix));
}

QString trustedSystemProgram(const QString& name) {
    return trustedSystemProgramImpl(name);
}

QProcessEnvironment trustedProcessEnvironment() {
    const auto source = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/local/bin:/usr/bin:/bin"));
    for (const QString& name : source.keys()) {
        if (allowEnvironmentName(name))
            env.insert(name, source.value(name));
    }
    return env;
}

bool isPrivateRuntimePath(const QString& path) {
    return pathIsInPrivateRuntimeRoot(path);
}

bool isPrivateRuntimeFile(const QString& path, qint64 maxSize) {
    return privateRuntimeFileExists(path, maxSize);
}

bool savePrivatePng(const QImage& image, const QString& path) {
    if (image.isNull() || path.isEmpty())
        return false;
    return savePng(image, path);
}

ClipboardSnapshotData captureClipboardSnapshotData() {
    ClipboardSnapshotData snapshot;
    const auto* clipboard = QGuiApplication::clipboard();
    const auto* mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime)
        return snapshot;

    snapshot.valid = true;
    if (mime->hasText()) {
        snapshot.text = mime->text();
        snapshot.empty = false;
    }
    if (mime->hasHtml()) {
        snapshot.html = mime->html();
        snapshot.empty = false;
    }
    if (mime->hasUrls()) {
        for (const auto& url : mime->urls())
            snapshot.urls.append(url);
        snapshot.empty = false;
    }
    if (mime->hasColor()) {
        const QColor color = qvariant_cast<QColor>(mime->colorData());
        if (color.isValid()) {
            snapshot.color = color;
            snapshot.empty = false;
        }
    }
    if (mime->hasImage()) {
        QImage image = qvariant_cast<QImage>(mime->imageData());
        if (image.isNull()) {
            const QPixmap pixmap = qvariant_cast<QPixmap>(mime->imageData());
            image = pixmap.toImage();
        }
        if (!image.isNull()) {
            snapshot.image = image;
            snapshot.empty = false;
        }
    }

    return snapshot;
}

QString saveClipboardSnapshotData(const ClipboardSnapshotData& snapshot) {
    if (!snapshot.valid)
        return {};

    QJsonObject root;
    root.insert("empty", snapshot.empty);
    if (!snapshot.text.isEmpty()) {
        root.insert("text", snapshot.text);
        root.insert("empty", false);
    }
    if (!snapshot.html.isEmpty()) {
        root.insert("html", snapshot.html);
        root.insert("empty", false);
    }
    if (!snapshot.urls.isEmpty()) {
        QJsonArray urls;
        for (const auto& url : snapshot.urls)
            urls.append(url.toString());
        root.insert("urls", urls);
        root.insert("empty", false);
    }
    if (snapshot.color.isValid()) {
        root.insert("color", snapshot.color.name(QColor::HexArgb));
        root.insert("empty", false);
    }
    if (!snapshot.image.isNull()) {
        const QString imagePath = runtimeFile("clipboard-image", ".png");
        if (savePng(snapshot.image, imagePath)) {
            root.insert("image", imagePath);
            root.insert("empty", false);
        }
    }

    const QString path = runtimeFile("clipboard", ".json");
    if (!writePrivateFile(path, QJsonDocument(root).toJson(QJsonDocument::Compact)))
        return {};
    return path;
}

QString saveClipboardSnapshot() {
    return saveClipboardSnapshotData(captureClipboardSnapshotData());
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
    if (savePng(image, path)) {
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
    if (!privateRuntimeFileExists(path, kMaxClipboardSnapshotBytes) || !file.open(QIODevice::ReadOnly))
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
        const QString imagePath = obj.value("image").toString();
        QFile         imageFile(imagePath);
        if (privateRuntimeFileExists(imagePath, kMaxClipboardImageBytes) && imageFile.open(QIODevice::ReadOnly) &&
            copyBytesWithWlCopy(imageFile.readAll(), QStringLiteral("image/png"))) {
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
        const QString imagePath = obj.value("image").toString();
        QImage        image(privateRuntimeFileExists(imagePath, kMaxClipboardImageBytes) ? imagePath : QString{});
        if (!image.isNull())
            mime->setImageData(image);
    }
    clipboard->setMimeData(mime);
    removeSnapshotFiles(obj, path);
}

} // namespace hyprcapture::ui
