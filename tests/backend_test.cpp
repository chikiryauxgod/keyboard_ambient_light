#include "backends.hpp"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}
void u32(QByteArray &b, quint32 n) {
    auto le = qToLittleEndian(n);
    b.append(reinterpret_cast<const char *>(&le), 4);
}
void u16(QByteArray &b, quint16 n) {
    auto le = qToLittleEndian(n);
    b.append(reinterpret_cast<const char *>(&le), 2);
}
void str(QByteArray &b, const char *s) {
    QByteArray v(s);
    v += '\0';
    u16(b, static_cast<quint16>(v.size()));
    b += v;
}
QByteArray read(QTcpSocket &socket, int n) {
    QByteArray result;
    while (result.size() < n) {
        result += socket.read(n - result.size());
        if (result.size() < n && !socket.waitForReadyRead(2000))
            throw std::runtime_error("Mock server read timed out");
    }
    return result;
}
struct Packet {
    quint32 id;
    QByteArray body;
};
Packet receive(QTcpSocket &socket) {
    auto header = read(socket, 16);
    check(header.left(4) == "ORGB", "Bad magic");
    check(qFromLittleEndian<quint32>(header.constData() + 4) == 0, "Wrong controller");
    auto size = qFromLittleEndian<quint32>(header.constData() + 12);
    check(size < 4096, "Oversized test packet");
    return {qFromLittleEndian<quint32>(header.constData() + 8), read(socket, static_cast<int>(size))};
}
void reply(QTcpSocket &socket, quint32 id, const QByteArray &body) {
    QByteArray packet("ORGB", 4);
    u32(packet, 0);
    u32(packet, id);
    u32(packet, static_cast<quint32>(body.size()));
    packet += body;
    // Split the header to exercise TCP stream assembly.
    socket.write(packet.left(7));
    socket.waitForBytesWritten(1000);
    socket.write(packet.mid(7));
    socket.waitForBytesWritten(1000);
}
QByteArray mode(const char *name) {
    QByteArray b;
    str(b, name);
    for (int i = 0; i < 9; ++i)
        u32(b, 0);
    u16(b, 0);
    return b;
}
QByteArray description(const QByteArray &initialMode, bool direct) {
    QByteArray b;
    u32(b, 0);
    u32(b, 5);
    str(b, "Mock keyboard");
    str(b, "description");
    str(b, "version");
    str(b, "serial");
    str(b, "location");
    u16(b, direct ? 2 : 1);
    u32(b, 0);
    b += initialMode;
    if (direct)
        b += mode("Direct");
    u16(b, 1);
    str(b, "keys");
    u32(b, 1);
    u32(b, 2);
    u32(b, 2);
    u32(b, 2);
    u16(b, 0);
    u16(b, 2);
    str(b, "A");
    u32(b, 0);
    str(b, "B");
    u32(b, 1);
    u16(b, 2);
    b += QByteArray::fromHex("1122330044556600");
    qToLittleEndian<quint32>(static_cast<quint32>(b.size()), b.data());
    return b;
}
void scenario(bool direct, bool discovery = false, bool staleSelection = false) {
    std::promise<quint16> portPromise;
    auto port = portPromise.get_future();
    auto server = std::async(std::launch::async, [direct, discovery, staleSelection,
                                                  p = std::move(portPromise)]() mutable {
        QTcpServer listener;
        if (!listener.listen(QHostAddress::LocalHost, 0)) {
            p.set_exception(std::make_exception_ptr(std::runtime_error("Listen failed")));
            return;
        }
        p.set_value(listener.serverPort());
        check(listener.waitForNewConnection(2000), "Client did not connect");
        std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
        check(receive(*socket).id == 0, "Expected device count request");
        QByteArray count;
        u32(count, 1);
        reply(*socket, 0, count);
        auto request = receive(*socket);
        check(request.id == 1 && request.body.isEmpty(), "Expected v0 controller request");
        const auto initialMode = mode("Rainbow");
        reply(*socket, 1, description(initialMode, direct));
        if (!direct || discovery || staleSelection) {
            if (socket->state() != QAbstractSocket::UnconnectedState)
                socket->waitForDisconnected(2000);
            check(socket->bytesAvailable() == 0, "Discovery/rejected connection must not change hardware");
            return;
        }
        check(receive(*socket).id == 1100, "Expected transient Direct mode");
        auto update = receive(*socket);
        check(update.id == 1050, "Expected LED update");
        check(update.body == QByteArray::fromHex("0e00000002008000000080000000"),
              "Unexpected scaled red frame");
        auto restoreColors = receive(*socket);
        check(restoreColors.id == 1050 &&
                  restoreColors.body == QByteArray::fromHex("0e00000002001122330044556600"),
              "Colors not restored");
        auto restoreMode = receive(*socket);
        QByteArray expected;
        u32(expected, 8 + static_cast<quint32>(initialMode.size()));
        u32(expected, 0);
        expected += initialMode;
        check(restoreMode.id == 1101 && restoreMode.body == expected, "Active mode not restored");
        // No save-to-device command should be emitted anywhere in this sequence.
    });
    bool rejected = false;
    try {
        const auto serverPort = port.get();
        if (discovery) {
            const auto devices = openRgbDevices("127.0.0.1", serverPort);
            check(devices.size() == 1 && devices[0].label.contains("Mock keyboard"),
                  "Named device discovery");
            check(devices[0].available == direct, "Device capability must be exposed in discovery");
            check(devices[0].kind == DeviceKind::OpenRgb, "Generic device transport type");
        } else {
            std::unique_ptr<ambient::LightingOutput> output;
            if (staleSelection) {
                Device device;
                device.controller = 0;
                device.key = "different device";
                output = openRgb("127.0.0.1", serverPort, device);
            } else
                output = openRgb("127.0.0.1", serverPort, 0);
            const ambient::Rgb red[] = {{1, 0, 0}};
            output->write(red, .5f);
            output->restore();
        }
    } catch (const std::exception &) {
        rejected = true;
    }
    server.get();
    check(rejected == ((!direct && !discovery) || staleSelection), "Capability/selection validation failed");
}
} // namespace
TEST(OpenRgb, WritesScaledFrameAndRestoresColorsAndMode) {
    EXPECT_NO_THROW(scenario(true));
}
TEST(OpenRgb, RejectsControllerWithoutDirectModeWithoutWrites) {
    EXPECT_NO_THROW(scenario(false));
}
TEST(OpenRgb, DiscoversNamedControllersWithoutMutatingThem) {
    EXPECT_NO_THROW(scenario(true, true));
}
TEST(OpenRgb, ExposesUnavailableControllersWithoutMutatingThem) {
    EXPECT_NO_THROW(scenario(false, true));
}
TEST(OpenRgb, RefusesStaleDeviceSelectionWithoutMutatingIt) {
    EXPECT_NO_THROW(scenario(true, false, true));
}

class OpenRgbMalformed : public testing::TestWithParam<int> {};
TEST_P(OpenRgbMalformed, RejectsInvalidServerDataBeforeHardwareWrites) {
    std::promise<quint16> promise;
    auto port = promise.get_future();
    const int fault = GetParam();
    auto server = std::async(std::launch::async, [fault, p = std::move(promise)]() mutable {
        QTcpServer listener;
        if (!listener.listen(QHostAddress::LocalHost, 0)) {
            p.set_exception(std::make_exception_ptr(std::runtime_error("Listen failed")));
            return;
        }
        p.set_value(listener.serverPort());
        check(listener.waitForNewConnection(2000), "No client");
        std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
        check(receive(*socket).id == 0, "Expected count request");
        if (fault <= 1) {
            QByteArray header(fault == 0 ? "NOPE" : "ORGB", 4);
            u32(header, 0);
            u32(header, 0);
            u32(header, fault == 0 ? 0 : 8 * 1024 * 1024 + 1);
            socket->write(header);
            socket->waitForBytesWritten(1000);
        } else if (fault == 2)
            reply(*socket, 0, QByteArray(1, '\0'));
        else {
            QByteArray count;
            u32(count, fault == 3 ? 257 : 1);
            reply(*socket, 0, count);
            if (fault >= 4) {
                check(receive(*socket).id == 1, "Expected controller request");
                QByteArray data;
                if (fault == 4)
                    u32(data, 4); // Missing mandatory controller fields.
                else {
                    data = description(mode("Rainbow"), true);
                    qToLittleEndian<quint32>(static_cast<quint32>(data.size() + 1), data.data());
                }
                reply(*socket, 1, data);
            }
        }
        if (socket->state() != QAbstractSocket::UnconnectedState)
            socket->waitForDisconnected(2000);
        check(socket->bytesAvailable() == 0, "Malformed response must not trigger a hardware write");
    });
    EXPECT_THROW(openRgbDevices("127.0.0.1", port.get()), std::runtime_error);
    EXPECT_NO_THROW(server.get());
}
INSTANTIATE_TEST_SUITE_P(HeaderBoundsAndDescriptor, OpenRgbMalformed, testing::Values(0, 1, 2, 3, 4, 5));
