#pragma once
#include "backends.hpp"

QList<Device> corsairDevices();
std::unique_ptr<ambient::LightingOutput> openCorsair(const Device &device);
