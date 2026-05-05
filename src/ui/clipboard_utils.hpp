#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QString>
#include <QUrl>

class QPixmap;

namespace hyprcapture::ui {

struct ClipboardSnapshotData {
    bool        valid = false;
    bool        empty = true;
    QString     text;
    QString     html;
    QList<QUrl> urls;
    QColor      color;
    QImage      image;
};

QString runtimeFile(const QString& prefix, const QString& suffix);
ClipboardSnapshotData captureClipboardSnapshotData();
QString saveClipboardSnapshotData(const ClipboardSnapshotData& snapshot);
QString saveClipboardSnapshot();
bool copyImageToClipboard(const QImage& image);
bool copyImageToClipboardDetached(const QImage& image);
bool copyImageFileToClipboardDetached(const QString& path);
bool copyPixmapToClipboard(const QPixmap& pixmap);
void restoreClipboardSnapshot(const QString& path);

} // namespace hyprcapture::ui
