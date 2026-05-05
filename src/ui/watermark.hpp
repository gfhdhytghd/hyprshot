#pragma once

#include "shared/config.hpp"

#include <QImage>

namespace hyprcapture::ui {

void applyWatermark(QImage& image, const CaptureDefaults& defaults);

} // namespace hyprcapture::ui
