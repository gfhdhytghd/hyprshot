#include "shared/config.hpp"
#include "ui/watermark.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QRect>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>

namespace {

bool imageDiffers(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.format() != b.format() || a.sizeInBytes() != b.sizeInBytes())
        return true;
    return std::memcmp(a.constBits(), b.constBits(), static_cast<std::size_t>(a.sizeInBytes())) != 0;
}

QRect diffBounds(const QImage& a, const QImage& b) {
    QRect bounds;
    for (int y = 0; y < std::min(a.height(), b.height()); ++y) {
        for (int x = 0; x < std::min(a.width(), b.width()); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y))
                bounds = bounds.united(QRect(x, y, 1, 1));
        }
    }
    return bounds;
}

} // namespace

int main(int argc, char** argv) {
    using namespace hyprcapture;

    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    QImage base(QSize(800, 600), QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::white);

    CaptureDefaults defaults;
    defaults.watermark = "activate-linux";
    defaults.watermarkPosition = WatermarkPosition::Central;
    defaults.watermarkWidth = "25%";
    defaults.watermarkOffset = "0 0";

    auto activate = base;
    ui::applyWatermark(activate, defaults);
    assert(imageDiffers(base, activate));

    defaults.watermark = "hypercam2";
    defaults.watermarkPosition = WatermarkPosition::DownRight;
    defaults.watermarkWidth = "20%";
    defaults.watermarkOffset = "-10px -10px";

    auto hypercam = base;
    ui::applyWatermark(hypercam, defaults);
    assert(imageDiffers(base, hypercam));
    assert(diffBounds(base, hypercam).height() >= 32);

    defaults.watermark = "/missing/unregistered hypercam 2.jpg";
    auto hypercamPathAlias = base;
    ui::applyWatermark(hypercamPathAlias, defaults);
    assert(imageDiffers(base, hypercamPathAlias));

    defaults.watermark.clear();
    auto disabled = base;
    ui::applyWatermark(disabled, defaults);
    assert(!imageDiffers(base, disabled));

    std::cout << "hyprcapture watermark tests passed\n";
    return 0;
}
