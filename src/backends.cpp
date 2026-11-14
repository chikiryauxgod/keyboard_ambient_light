#include "backends.hpp"
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#ifdef AMBIENT_HIDAPI
#include <hidapi.h>
#if defined(Q_OS_MACOS) && !defined(AMBIENT_FAKE_HIDAPI) && __has_include(<hidapi_darwin.h>)
#include <hidapi_darwin.h>
#define AMBIENT_HID_DARWIN
#endif
#endif

namespace {
[[noreturn]] void fail(const QString &text) {
    throw std::runtime_error(text.toStdString());
}
void put32(QByteArray &b, quint32 v) {
    const auto le = qToLittleEndian(v);
    b.append(reinterpret_cast<const char *>(&le), 4);
}
void put16(QByteArray &b, quint16 v) {
    const auto le = qToLittleEndian(v);
    b.append(reinterpret_cast<const char *>(&le), 2);
}
struct Reader {
    const QByteArray &data;
    qsizetype pos{};
    QByteArray take(qsizetype n) {
        if (n < 0 || n > data.size() - pos)
            fail("Malformed OpenRGB packet");
        auto value = data.mid(pos, n);
        pos += n;
        return value;
    }
    quint16 u16() {
        auto b = take(2);
        return qFromLittleEndian<quint16>(b.constData());
    }
    quint32 u32() {
        auto b = take(4);
        return qFromLittleEndian<quint32>(b.constData());
    }
    QString string() {
        auto b = take(u16());
        if (!b.isEmpty() && b.back() == '\0')
            b.chop(1);
        return QString::fromUtf8(b);
    }
};

class OpenRgb final : public ambient::LightingOutput {
    QTcpSocket socket_;
    unsigned id_;
    QByteArray colors_, mode_;
    quint32 active_{};
    unsigned count_{};
    bool changed_{};
    bool direct_{};
    QString name_, serial_, location_;
    void send(unsigned command, const QByteArray &payload = {}) {
        QByteArray packet("ORGB", 4);
        put32(packet, id_);
        put32(packet, command);
        put32(packet, static_cast<quint32>(payload.size()));
        packet += payload;
        if (socket_.write(packet) != packet.size())
            fail("OpenRGB write failed");
        while (socket_.bytesToWrite())
            if (!socket_.waitForBytesWritten(1000))
                fail(socket_.errorString());
    }
    QByteArray readBytes(qsizetype n, QElapsedTimer &deadline) {
        QByteArray data;
        while (data.size() < n) {
            data += socket_.read(n - data.size());
            if (data.size() == n)
                break;
            const auto left = 1500 - deadline.elapsed();
            if (left <= 0 || !socket_.waitForReadyRead(static_cast<int>(left)))
                fail("OpenRGB response timed out");
        }
        return data;
    }
    QByteArray request(unsigned command) {
        send(command);
        QElapsedTimer deadline;
        deadline.start();
        for (;;) {
            auto header = readBytes(16, deadline);
            if (header.left(4) != "ORGB")
                fail("Invalid OpenRGB header");
            Reader h{header, 4};
            const auto device = h.u32(), reply = h.u32(), size = h.u32();
            if (size > 8 * 1024 * 1024)
                fail("OpenRGB packet exceeds limit");
            auto payload = readBytes(size, deadline);
            if (reply == command && (command == 0 || device == id_))
                return payload;
        }
    }

  public:
    OpenRgb(const QString &host, quint16 port) : id_(0) {
        socket_.connectToHost(host, port);
        if (!socket_.waitForConnected(1500))
            fail(socket_.errorString());
    }
    unsigned controllerCount() {
        auto countData = request(0);
        Reader controllers{countData};
        return controllers.u32();
    }
    void inspect(unsigned id) {
        id_ = id;
        mode_.clear();
        colors_.clear();
        auto data = request(1);
        Reader r{data};
        if (r.u32() != static_cast<quint32>(data.size()))
            fail("Invalid controller size");
        r.u32();            // device type
        name_ = r.string(); // no vendor field in protocol v0
        r.string();
        r.string();
        serial_ = r.string();
        location_ = r.string();
        auto modes = r.u16();
        active_ = r.u32();
        bool direct = false;
        for (unsigned i = 0; i < modes; ++i) {
            const auto start = r.pos;
            const auto name = r.string();
            r.take(9 * 4); // v0 mode scalar fields
            r.take(static_cast<qsizetype>(r.u16()) * 4);
            if (i == active_)
                mode_ = data.mid(start, r.pos - start);
            direct |= name.compare("Direct", Qt::CaseInsensitive) == 0;
        }
        direct_ = direct && !mode_.isEmpty();
        const auto zones = r.u16();
        for (unsigned i = 0; i < zones; ++i) {
            r.string();
            r.take(4 * 4);
            r.take(r.u16()); // matrix map, including dimensions
        }
        const auto leds = r.u16();
        for (unsigned i = 0; i < leds; ++i) {
            r.string();
            r.u32();
        }
        count_ = r.u16();
        colors_ = r.take(static_cast<qsizetype>(count_) * 4);
    }
    Device descriptor() const {
        Device d;
        d.label = name_ + " — OpenRGB";
        d.kind = DeviceKind::OpenRgb;
        d.controller = id_;
        d.key = "openrgb:" + name_ + ":" + serial_ + ":" + location_;
        d.available = direct_ && count_ > 0;
        d.details = d.available ? QString("OpenRGB · %1 LEDs · whole-device color · Direct mode").arg(count_)
                                : "OpenRGB · live color updates unavailable (no Direct mode or LEDs)";
        return d;
    }
    OpenRgb(const QString &host, quint16 port, unsigned id, const QString &expectedKey = {})
        : OpenRgb(host, port) {
        if (id >= controllerCount())
            fail("OpenRGB controller no longer exists; refresh devices");
        inspect(id);
        if (!expectedKey.isEmpty() && descriptor().key != expectedKey)
            fail("OpenRGB device list changed; refresh and select the device again");
        if (!direct_ || !count_)
            fail("Controller must expose a Direct mode and addressable colors");
        changed_ = true;
        try {
            send(1100);
        } catch (...) {
            try {
                restore();
            } catch (...) {
            }
            throw;
        }
    }
    void write(std::span<const ambient::Rgb> zones, float brightness) override {
        if (zones.empty())
            return;
        // Whole-device color; LED order is not assumed to be keyboard geometry.
        ambient::Rgb c{};
        for (auto z : zones) {
            c.r += z.r;
            c.g += z.g;
            c.b += z.b;
        }
        auto byte = [&](float v) {
            return static_cast<char>(
                std::lround(std::clamp(v / static_cast<float>(zones.size()) * brightness, 0.f, 1.f) * 255));
        };
        QByteArray payload;
        put32(payload, 6 + 4 * count_);
        put16(payload, static_cast<quint16>(count_));
        for (unsigned i = 0; i < count_; ++i) {
            payload += byte(c.r);
            payload += byte(c.g);
            payload += byte(c.b);
            payload += '\0';
        }
        send(1050, payload);
    }
    void restore() override {
        if (!changed_)
            return;
        QByteArray colors;
        put32(colors, 6 + static_cast<quint32>(colors_.size()));
        put16(colors, static_cast<quint16>(count_));
        colors += colors_;
        send(1050, colors);
        QByteArray mode;
        put32(mode, 8 + static_cast<quint32>(mode_.size()));
        put32(mode, active_);
        mode += mode_;
        send(1101, mode);
        changed_ = false;
    }
    ~OpenRgb() override {
        try {
            restore();
        } catch (...) {
        }
    }
};

#ifdef AMBIENT_HIDAPI
// Protocol reference: vial-kb/vial-qmk quantum/vialrgb.{h,c}.
// Only volatile commands are used; never write firmware configuration to EEPROM.
class VialRgb final : public ambient::LightingOutput {
    hid_device *handle_{};
    std::array<unsigned char, 6> saved_{};
    unsigned char maximum_{};
    bool changed_{};
    std::array<unsigned char, 32> exchange(unsigned char command, unsigned char setting,
                                           std::span<const unsigned char> args = {}) {
        std::array<unsigned char, 33> packet{};
        packet[1] = command;
        packet[2] = setting;
        std::copy(args.begin(), args.end(), packet.begin() + 3);
        if (hid_write(handle_, packet.data(), packet.size()) < static_cast<int>(packet.size()))
            fail("VialRGB write failed");
        std::array<unsigned char, 32> reply{};
        if (hid_read_timeout(handle_, reply.data(), reply.size(), 250) != 32 || reply[0] != command ||
            reply[1] != setting)
            fail("Invalid VialRGB reply or timeout; close other lighting software");
        return reply;
    }

  public:
    explicit VialRgb(const Device &device) {
        if (hid_init() != 0)
            fail("HIDAPI initialization failed");
#ifdef AMBIENT_HID_DARWIN
        hid_darwin_set_open_exclusive(0);
#endif
        handle_ = hid_open_path(device.path.constData());
        if (!handle_)
            fail("Cannot open VialRGB HID device; check device permissions and connection");
        try {
            const auto info = exchange(8, 0x40);
            if (info[2] != 1 || info[3] != 0 || info[4] == 0)
                fail("Device does not expose VialRGB protocol 1");
            maximum_ = info[4];
            // Asking for IDs greater than 1 places Solid Color (ID 2) first if present.
            const std::array<unsigned char, 2> afterDirect{1, 0};
            const auto supported = exchange(8, 0x42, afterDirect);
            if (supported[2] != 2 || supported[3] != 0)
                fail("VialRGB firmware does not support Solid Color");
            const auto mode = exchange(8, 0x41);
            std::copy_n(mode.begin() + 2, saved_.size(), saved_.begin());
        } catch (...) {
            hid_close(handle_);
            handle_ = nullptr;
            throw;
        }
    }
    void write(std::span<const ambient::Rgb> zones, float brightness) override {
        if (zones.empty())
            return;
        ambient::Rgb color{};
        for (const auto &zone : zones) {
            color.r += zone.r;
            color.g += zone.g;
            color.b += zone.b;
        }
        const auto count = static_cast<float>(zones.size());
        color.r /= count;
        color.g /= count;
        color.b /= count;
        if (!std::isfinite(brightness))
            fail("VialRGB brightness must be finite");
        const auto hsv = ambient::toHsv(color);
        const std::array<unsigned char, 6> mode{
            2,
            0,
            saved_[2],
            hsv.h,
            hsv.s,
            static_cast<unsigned char>(std::lround(static_cast<float>(hsv.v) / 255.f * maximum_ *
                                                   std::clamp(brightness, 0.f, 1.f)))};
        changed_ = true; // A failed reply may still mean the device accepted the write.
        exchange(7, 0x41, mode);
    }
    void restore() override {
        if (!changed_)
            return;
        exchange(7, 0x41, saved_);
        changed_ = false;
    }
    ~VialRgb() override {
        if (handle_) {
            try {
                restore();
            } catch (...) {
            }
            hid_close(handle_);
        }
    }
};

class Via final : public ambient::LightingOutput {
    hid_device *handle_{};
    std::array<std::array<unsigned char, 2>, 3> saved_{};
    bool changed_{};
    unsigned char channel_{};
    std::array<unsigned char, 32> exchange(unsigned char command, unsigned char setting, unsigned char a = 0,
                                           unsigned char b = 0) {
        std::array<unsigned char, 33> packet{};
        packet[1] = command;
        if (command != 1) {
            packet[2] = channel_;
            packet[3] = setting;
            packet[4] = a;
            packet[5] = b;
        }
        if (hid_write(handle_, packet.data(), packet.size()) < static_cast<int>(packet.size()))
            fail("VIA write failed");
        std::array<unsigned char, 32> reply{};
        const auto n = hid_read_timeout(handle_, reply.data(), reply.size(), 250);
        if (n != 32 || reply[0] != command || (command != 1 && (reply[1] != channel_ || reply[2] != setting)))
            fail("Invalid VIA reply or timeout; close VIA and check keyboard connection");
        return reply;
    }

  public:
    explicit Via(const Device &device) : channel_(device.channel) {
        if (channel_ != 2 && channel_ != 3)
            fail("Unsupported VIA lighting channel");
        if (hid_init() != 0)
            fail("HIDAPI initialization failed");
#ifdef AMBIENT_HID_DARWIN
        // Keep normal keyboard input available while controlling its lighting.
        hid_darwin_set_open_exclusive(0);
#endif
        handle_ = hid_open_path(device.path.constData());
        if (!handle_) {
#ifdef Q_OS_LINUX
            fail("Cannot open HID device. Export/install its USB access rule in Connections and reconnect "
                 "it.");
#else
            fail("Cannot open HID device. Close other lighting software and check OS device access "
                 "permissions.");
#endif
        }
        try {
            auto protocol = exchange(1, 0);
            if (protocol[1] != 0 || (protocol[2] != 12 && protocol[2] != 13))
                fail("Standard lighting driver supports VIA protocol 12 and 13");
            for (unsigned i = 0; i < 3; ++i) {
                auto r = exchange(8, std::array<unsigned char, 3>{1, 2, 4}[i]);
                saved_[i] = {r[3], r[4]};
            }
            changed_ = true;
            exchange(7, 2, 1);
        } catch (...) {
            try {
                restore();
            } catch (...) {
            }
            hid_close(handle_);
            handle_ = nullptr;
            throw;
        }
    }
    void write(std::span<const ambient::Rgb> zones, float brightness) override {
        if (zones.empty())
            return;
        ambient::Rgb c{};
        for (auto z : zones) {
            c.r += z.r;
            c.g += z.g;
            c.b += z.b;
        }
        const auto count = static_cast<float>(zones.size());
        c.r /= count;
        c.g /= count;
        c.b /= count;
        auto hsv = ambient::toHsv(c);
        exchange(7, 4, hsv.h, hsv.s);
        exchange(7, 1, static_cast<unsigned char>(std::lround(hsv.v * std::clamp(brightness, 0.f, 1.f))));
    }
    void restore() override {
        if (!changed_)
            return;
        // Try every setting even if one command fails.
        QString errors;
        for (unsigned i : {2u, 0u, 1u}) {
            try {
                exchange(7, std::array<unsigned char, 3>{1, 2, 4}[i], saved_[i][0], saved_[i][1]);
            } catch (const std::exception &e) {
                errors += QString::fromUtf8(e.what()) + "\n";
            }
        }
        if (!errors.isEmpty())
            fail(errors);
        changed_ = false;
    }
    ~Via() override {
        if (handle_) {
            try {
                restore();
            } catch (...) {
            }
            hid_close(handle_);
        }
    }
};
#endif
} // namespace

QList<Device> viaDevices() {
    QList<Device> devices;
#ifdef AMBIENT_HIDAPI
    if (hid_init() != 0)
        fail("HIDAPI initialization failed");
    auto *list = hid_enumerate(0, 0);
    for (auto *d = list; d; d = d->next) {
        if (d->usage_page != 0xff60 || d->usage != 0x61)
            continue;
        const bool known = d->vendor_id == 0x342d && d->product_id == 0xe506;
        const auto label = QString("%1 [%2:%3]")
                               .arg(d->product_string ? QString::fromWCharArray(d->product_string) : "VIA")
                               .arg(d->vendor_id, 4, 16, QChar('0'))
                               .arg(d->product_id, 4, 16, QChar('0'));
        for (unsigned char channel : {static_cast<unsigned char>(3), static_cast<unsigned char>(2)}) {
            if (known && channel == 2)
                continue; // Prefer key backlight for the verified profile.
            Device device{label + (channel == 3 ? " — VIA Matrix" : " — VIA RGB Light"), QByteArray(d->path),
                          known};
            device.channel = channel;
            device.vendorId = d->vendor_id;
            device.productId = d->product_id;
            device.interfaceNumber = d->interface_number;
            const auto serial = d->serial_number ? QString::fromWCharArray(d->serial_number) : QString{};
            device.key =
                QString("via:%1:%2:%3:%4").arg(d->vendor_id).arg(d->product_id).arg(serial).arg(channel);
            device.details = known ? "VIA · whole-keyboard color · verified Luma100 2.4G"
                                   : "Standard VIA 12/13 · whole-keyboard color · RGB channel checked on "
                                     "connection; firmware compatibility varies";
            devices.push_back(device);
        }
        Device vial;
        vial.label = label + " — VialRGB";
        vial.path = QByteArray(d->path);
        vial.kind = DeviceKind::VialRgb;
        vial.vendorId = d->vendor_id;
        vial.productId = d->product_id;
        vial.interfaceNumber = d->interface_number;
        const auto serial = d->serial_number ? QString::fromWCharArray(d->serial_number) : QString{};
        vial.key = QString("vialrgb:%1:%2:%3").arg(d->vendor_id).arg(d->product_id).arg(serial);
        vial.details = "VialRGB candidate · requires protocol 1 and Solid Color · checked on connection · "
                       "whole-keyboard color · hardware not verified";
        devices.push_back(vial);
    }
    hid_free_enumeration(list);
#endif
    return devices;
}
QList<Device> openRgbDevices(const QString &host, quint16 port) {
    OpenRgb client(host, port);
    const auto count = client.controllerCount();
    if (count > 256)
        fail("OpenRGB device count exceeds discovery limit");
    QList<Device> result;
    QElapsedTimer deadline;
    deadline.start();
    for (unsigned i = 0; i < count; ++i) {
        if (deadline.elapsed() > 5000)
            fail("OpenRGB discovery timed out");
        client.inspect(i);
        result.push_back(client.descriptor());
    }
    return result;
}
std::unique_ptr<ambient::LightingOutput> openVia(const Device &device) {
#ifdef AMBIENT_HIDAPI
    return std::make_unique<Via>(device);
#else
    Q_UNUSED(device);
    fail("Built without HIDAPI; install HIDAPI and rebuild");
#endif
}
std::unique_ptr<ambient::LightingOutput> openRgb(const QString &host, quint16 port, unsigned controller) {
    return std::make_unique<OpenRgb>(host, port, controller);
}
std::unique_ptr<ambient::LightingOutput> openRgb(const QString &host, quint16 port, const Device &device) {
    return std::make_unique<OpenRgb>(host, port, device.controller, device.key);
}

std::unique_ptr<ambient::LightingOutput> openVialRgb(const Device &device) {
#ifdef AMBIENT_HIDAPI
    return std::make_unique<VialRgb>(device);
#else
    Q_UNUSED(device);
    fail("Built without HIDAPI; install HIDAPI and rebuild");
#endif
}
