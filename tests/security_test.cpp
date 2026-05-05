#include "ui/clipboard_utils.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

#include <cstdlib>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

mode_t modeFor(const QString& path) {
    struct stat st {};
    const auto  native = QFile::encodeName(path);
    if (stat(native.constData(), &st) != 0) {
        std::cerr << "security test failed: stat failed for " << path.toStdString() << '\n';
        std::exit(1);
    }
    return st.st_mode & 0777;
}

void require(bool condition, const char* message) {
    if (condition)
        return;

    std::cerr << "security test failed: " << message << '\n';
    std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const QString path = hyprcapture::ui::runtimeFile(QStringLiteral("security"), QStringLiteral(".png"));
    require(!path.isEmpty(), "runtime path is not empty");
    require(path.contains(QStringLiteral("hyprcapture-%1").arg(QString::number(static_cast<qulonglong>(geteuid())))), "runtime path is per-user");

    const QFileInfo info(path);
    require(modeFor(info.absolutePath()) == 0700, "runtime directory is 0700");

    QImage image(QSize(1, 1), QImage::Format_ARGB32);
    image.fill(Qt::red);
    require(hyprcapture::ui::savePrivatePng(image, path), "private png save succeeds");
    require(modeFor(path) == 0600, "private png is 0600");
    require(hyprcapture::ui::isPrivateRuntimeFile(path, 1024 * 1024), "private png is accepted as private runtime file");

    QFile::remove(path);
    QDir(QFileInfo(info.absolutePath()).absolutePath()).rmdir(QFileInfo(info.absolutePath()).fileName());

    std::cout << "hyprcapture security tests passed\n";
    return 0;
}
