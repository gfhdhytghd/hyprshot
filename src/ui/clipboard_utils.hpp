#pragma once

#include <QString>

class QImage;
class QPixmap;

namespace hyprcapture::ui {

QString runtimeFile(const QString& prefix, const QString& suffix);
QString saveClipboardSnapshot();
bool copyImageToClipboard(const QImage& image);
bool copyPixmapToClipboard(const QPixmap& pixmap);
void restoreClipboardSnapshot(const QString& path);

} // namespace hyprcapture::ui
