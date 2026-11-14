#pragma once
#include "ambient/core.hpp"
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <memory>

enum class DeviceKind { Preview, Via, OpenRgb, VialRgb, Logitech, Razer, Corsair };
struct Device {
    QString label{};
    QByteArray path{};
    bool verifiedProfile{};
    unsigned char channel{3};
    unsigned controller{};
    DeviceKind kind{DeviceKind::Via};
    QString key{};
    QString details{};
    bool available{true};
    quint16 vendorId{}, productId{};
    int interfaceNumber{-1};
};
QList<Device> viaDevices();
QList<Device> openRgbDevices(const QString &host, quint16 port);
std::unique_ptr<ambient::LightingOutput> openVia(const Device &device);
// OpenRGB v0 wire protocol: interoperates with current SDK servers.
// Select controller ID explicitly; a Direct mode is required.
std::unique_ptr<ambient::LightingOutput> openRgb(const QString &host, quint16 port, unsigned controller);
std::unique_ptr<ambient::LightingOutput> openRgb(const QString &host, quint16 port, const Device &device);

std::unique_ptr<ambient::LightingOutput> openVialRgb(const Device &device);

// Discovery is read-only; vendor drivers match explicit model/interface profiles.
QList<Device> directDevices(QStringList *errors = nullptr);
std::unique_ptr<ambient::LightingOutput> openDirect(const Device &device);
