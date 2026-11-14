#pragma once
#include "backends.hpp"
QList<Device> razerDevices();
std::unique_ptr<ambient::LightingOutput> openRazer(const Device &device);
