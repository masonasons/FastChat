// Computer use where there is no desktop layer (a Linux build without X11 and
// AT-SPI's development files).

#include "core/desktop.h"

std::unique_ptr<Desktop> Desktop::Create() {
    return nullptr;
}

bool GrabStopKey(std::function<void()>) {
    return false;
}

void ReleaseStopKey() {}
