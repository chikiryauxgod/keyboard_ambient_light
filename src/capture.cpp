#include "capture.hpp"
#include <QGuiApplication>
#include <QPixmap>
#include <algorithm>
#include <stdexcept>
#ifdef AMBIENT_MULTIMEDIA
#include <QMediaCaptureSession>
#include <QScreenCapture>
#include <QVideoFrame>
#include <QVideoSink>
#endif

QRect regionRect(QSize size, CaptureRegion r) {
    if (size.isEmpty())
        return {};
    const int x = size.width() * std::clamp(r.x, 0, 99) / 100;
    const int y = size.height() * std::clamp(r.y, 0, 99) / 100;
    const int w = std::clamp(size.width() * std::clamp(r.width, 1, 100) / 100, 1, size.width() - x);
    const int h = std::clamp(size.height() * std::clamp(r.height, 1, 100) / 100, 1, size.height() - y);
    return {x, y, w, h};
}
struct ScreenSource::Impl {
    QPointer<QScreen> screen;
    QImage latest;
    bool streaming{};
#ifdef AMBIENT_MULTIMEDIA
    QMediaCaptureSession session;
    QScreenCapture capture;
    QVideoSink sink;
#endif
};
ScreenSource::ScreenSource(QObject *parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
#ifdef AMBIENT_MULTIMEDIA
    impl_->session.setScreenCapture(&impl_->capture);
    impl_->session.setVideoSink(&impl_->sink);
    connect(&impl_->sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) { impl_->latest = frame.toImage(); });
    connect(&impl_->capture, &QScreenCapture::errorOccurred, this,
            [this](QScreenCapture::Error, const QString &message) { emit failed(message); });
#endif
}
ScreenSource::~ScreenSource() {
    stop();
}
bool ScreenSource::portalAvailable() {
#ifdef AMBIENT_MULTIMEDIA
    return true;
#else
    return false;
#endif
}
void ScreenSource::start(QScreen *screen) {
    stop();
    impl_->screen = screen;
    if (!screen)
        throw std::runtime_error("Select an available monitor");
    const bool wayland = QGuiApplication::platformName().contains("wayland", Qt::CaseInsensitive);
    bool preferStream = wayland;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    preferStream = true;
#endif
#ifdef AMBIENT_MULTIMEDIA
    if (preferStream) {
        impl_->streaming = true;
        if (!wayland)
            impl_->capture.setScreen(screen);
        impl_->capture.start(); // Wayland requests a screen via the OS portal picker.
        if (impl_->capture.error() != QScreenCapture::NoError)
            throw std::runtime_error(impl_->capture.errorString().toStdString());
        return;
    }
#else
    Q_UNUSED(preferStream);
#endif
    if (wayland)
        throw std::runtime_error(
            "Wayland requires a build with Qt 6.5+ Multimedia, FFmpeg, PipeWire and a ScreenCast portal");
    if (screen->grabWindow(0, 0, 0, 8, 8).isNull())
        throw std::runtime_error(
            "Screen capture unavailable. On macOS grant Screen Recording permission and restart.");
}
void ScreenSource::stop() {
#ifdef AMBIENT_MULTIMEDIA
    impl_->capture.stop();
#endif
    impl_->latest = {};
    impl_->screen = nullptr;
    impl_->streaming = false;
}
QImage ScreenSource::grab(CaptureRegion region) {
    if (!impl_->screen)
        throw std::runtime_error("Selected monitor disconnected");
    if (impl_->streaming) {
        if (impl_->latest.isNull())
            return {};
        return impl_->latest.copy(regionRect(impl_->latest.size(), region));
    }
    const auto rect = regionRect(impl_->screen->size(), region);
    return impl_->screen->grabWindow(0, rect.x(), rect.y(), rect.width(), rect.height()).toImage();
}
QString ScreenSource::description() const {
    if (impl_->streaming)
        return "Qt Multimedia screen stream (OS permission/picker may be required)";
    return "Qt desktop capture";
}
