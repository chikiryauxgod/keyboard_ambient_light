#pragma once
#include "backends.hpp"

// USB models with a documented native-effect protocol. Does not include receivers/Bluetooth.
QList<Device> logitechDevices();
std::unique_ptr<ambient::LightingOutput> openLogitech(const Device &device);
