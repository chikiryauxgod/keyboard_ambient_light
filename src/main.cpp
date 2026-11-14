#include "ambient/core.hpp"
#include "backends.hpp"
#include "capture.hpp"
#include <QApplication>
#include <QColorSpace>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <memory>

struct OutputWorker : QObject {
    std::unique_ptr<ambient::LightingOutput> output;
};

class Window final : public QMainWindow {
    QSettings settings_;
    QThread io_;
    OutputWorker *worker_ = new OutputWorker;
    ScreenSource capture_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    ambient::Smoother smoother_;
    QList<Device> devices_;
    QComboBox *device_, *screen_, *source_, *sampling_;
    QLineEdit *host_;
    QSpinBox *port_, *fps_, *smoothing_, *x_, *y_, *width_, *height_;
    QDoubleSpinBox *brightness_, *saturation_, *gamma_, *hue_, *black_;
    std::array<QDoubleSpinBox *, 3> gains_;
    QPushButton *start_, *stop_, *refresh_;
    QLabel *status_, *raw_, *output_, *thumbnail_, *deviceInfo_, *captureInfo_;
    bool active_{}, busy_{}, scanning_{}, loading_{};
    unsigned generation_{};

    QSpinBox *spin(int lo, int hi, int value, const QString &key, const QString &suffix = {}) {
        auto *s = new QSpinBox;
        s->setRange(lo, hi);
        s->setValue(settings_.value(key, value).toInt());
        s->setSuffix(suffix);
        connect(s, &QSpinBox::valueChanged, this, [this, key](int v) { settings_.setValue(key, v); });
        return s;
    }
    QDoubleSpinBox *number(double lo, double hi, double step) {
        auto *s = new QDoubleSpinBox;
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(2);
        return s;
    }
    QString colorKey() const {
        auto key = devices_.value(device_->currentIndex()).key;
        return "devices/" +
               QString::fromLatin1(
                   QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) +
               "/";
    }
    void loadCalibration() {
        loading_ = true;
        const auto prefix = colorKey();
        brightness_->setValue(settings_.value(prefix + "brightness", .5).toDouble());
        saturation_->setValue(settings_.value(prefix + "saturation", 1).toDouble());
        gamma_->setValue(settings_.value(prefix + "gamma", 1).toDouble());
        hue_->setValue(settings_.value(prefix + "hue", 0).toDouble());
        for (int i = 0; i < 3; ++i)
            gains_[i]->setValue(settings_.value(prefix + "gain" + QString::number(i), 1).toDouble());
        loading_ = false;
        auto d = devices_.value(device_->currentIndex());
        deviceInfo_->setText(d.details);
        if (!scanning_ && !active_)
            start_->setEnabled(d.available);
    }
    void persistCalibration() {
        if (loading_)
            return;
        const auto prefix = colorKey();
        settings_.setValue(prefix + "brightness", brightness_->value());
        settings_.setValue(prefix + "saturation", saturation_->value());
        settings_.setValue(prefix + "gamma", gamma_->value());
        settings_.setValue(prefix + "hue", hue_->value());
        for (int i = 0; i < 3; ++i)
            settings_.setValue(prefix + "gain" + QString::number(i), gains_[i]->value());
    }
    void exportAccessRule() {
        const auto d = devices_.value(device_->currentIndex());
        if (d.kind != DeviceKind::Via || d.interfaceNumber < 0) {
            QMessageBox::information(
                this, "USB access",
                "Select a directly connected VIA device with a known USB interface first.");
            return;
        }
        const auto path = QFileDialog::getSaveFileName(this, "Export device access rule",
                                                       "70-keyboard-ambient.rules", "udev rules (*.rules)");
        if (path.isEmpty())
            return;
        const auto match = QString("SUBSYSTEM==\"hidraw\", KERNEL==\"hidraw*\", ATTRS{idVendor}==\"%1\", "
                                   "ATTRS{idProduct}==\"%2\"")
                               .arg(d.vendorId, 4, 16, QChar('0'))
                               .arg(d.productId, 4, 16, QChar('0'));
        const auto rule = "# Keyboard Ambient: active desktop user, selected USB interface only\n" + match +
                          ", IMPORT{builtin}=\"usb_id\"\n" + match +
                          QString(", ENV{ID_USB_INTERFACE_NUM}==\"%1\", TAG+=\"uaccess\"\n")
                              .arg(d.interfaceNumber, 2, 16, QChar('0'));
        QSaveFile file(path);
        const auto data = rule.toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            QMessageBox::warning(this, "USB access", file.errorString());
            return;
        }
        QMessageBox::information(this, "USB access",
                                 "Rule exported. Install it in /etc/udev/rules.d/, reload udev rules, then "
                                 "reconnect this device. Run the application as your desktop user.");
    }
    void setActive(bool value) {
        active_ = value;
        device_->setEnabled(!value && !scanning_);
        refresh_->setEnabled(!value && !scanning_);
        screen_->setEnabled(!value);
        source_->setEnabled(!value);
        host_->setEnabled(!value && !scanning_);
        port_->setEnabled(!value && !scanning_);
        start_->setEnabled(!value && !scanning_ && devices_.value(device_->currentIndex()).available);
        stop_->setEnabled(value);
    }
    void refresh() {
        if (active_ || scanning_)
            return;
        scanning_ = true;
        setActive(false);
        status_->setText("Discovering USB/HID and OpenRGB devices…");
        const auto host = host_->text();
        const auto port = static_cast<quint16>(port_->value());
        const auto preferred = settings_.value("selectedDevice", "preview").toString();
        QMetaObject::invokeMethod(
            worker_,
            [this, host, port, preferred] {
                QList<Device> found;
                Device preview;
                preview.label = "Preview only";
                preview.kind = DeviceKind::Preview;
                preview.key = "preview";
                preview.details = "Preview · no device writes";
                found.push_back(preview);
                QStringList notes;
                try {
                    found.append(viaDevices());
                } catch (const std::exception &e) {
                    notes << QString::fromUtf8(e.what());
                }
                try {
                    found.append(openRgbDevices(host, port));
                } catch (const std::exception &e) {
                    notes << "OpenRGB: " + QString::fromUtf8(e.what());
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, found, notes, preferred] {
                        devices_ = found;
                        loading_ = true;
                        device_->blockSignals(true);
                        device_->clear();
                        int selected = 0;
                        for (int i = 0; i < devices_.size(); ++i) {
                            const auto &d = devices_[i];
                            device_->addItem(d.label);
                            device_->setItemData(i, d.details, Qt::ToolTipRole);
                            if (!d.available) {
                                if (auto *model = qobject_cast<QStandardItemModel *>(device_->model()))
                                    model->item(i)->setEnabled(false);
                            }
                            if (d.key == preferred && d.available)
                                selected = i;
                        }
                        device_->setCurrentIndex(selected);
                        device_->blockSignals(false);
                        loading_ = false;
                        scanning_ = false;
                        loadCalibration();
                        setActive(false);
                        status_->setText(notes.isEmpty() ? "Devices ready"
                                                         : "Devices ready. " + notes.join(" · "));
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }
    void start() {
        if (scanning_ || active_)
            return;
        const auto device = devices_.value(device_->currentIndex());
        if (!device.available)
            return;
        try {
            if (source_->currentIndex() == 0)
                capture_.start(QApplication::screens().value(screen_->currentIndex(), nullptr));
        } catch (const std::exception &e) {
            capture_.stop();
            status_->setText(QString::fromUtf8(e.what()));
            return;
        }
        const auto host = host_->text();
        const auto port = static_cast<quint16>(port_->value());
        const auto session = ++generation_;
        setActive(true);
        busy_ = true;
        status_->setText("Connecting…");
        QMetaObject::invokeMethod(
            worker_,
            [this, device, host, port, session] {
                QString error;
                try {
                    if (device.kind == DeviceKind::Via)
                        worker_->output = openVia(device);
                    else if (device.kind == DeviceKind::OpenRgb)
                        worker_->output = openRgb(host, port, device);
                } catch (const std::exception &e) {
                    error = QString::fromUtf8(e.what());
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, error, session] {
                        if (session != generation_)
                            return;
                        busy_ = false;
                        if (!error.isEmpty()) {
                            stop();
                            status_->setText(error);
                            return;
                        }
                        smoother_.reset();
                        elapsed_.start();
                        timer_.start(1000 / fps_->value());
                        status_->setText(source_->currentIndex() == 0 ? "Running · " + capture_.description()
                                                                      : "Running · calibration test color");
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }
    void stop() {
        timer_.stop();
        active_ = false;
        ++generation_;
        capture_.stop();
        QString error;
        QMetaObject::invokeMethod(
            worker_,
            [this, &error] {
                if (worker_->output) {
                    try {
                        worker_->output->restore();
                    } catch (const std::exception &e) {
                        error = QString::fromUtf8(e.what());
                    }
                    worker_->output.reset();
                }
            },
            Qt::BlockingQueuedConnection);
        busy_ = false;
        setActive(false);
        status_->setText(error.isEmpty() ? "Stopped · previous lighting restored where applicable"
                                         : "Restore failed: " + error);
    }
    void swatch(QLabel *label, ambient::Rgb color) {
        const auto q = QColor::fromRgbF(std::clamp(color.r, 0.f, 1.f), std::clamp(color.g, 0.f, 1.f),
                                        std::clamp(color.b, 0.f, 1.f));
        label->setStyleSheet("background-color:" + q.name() +
                             "; border:1px solid #808080; border-radius:6px;");
        label->setToolTip(q.name());
    }
    void frame() {
        if (!active_ || busy_)
            return;
        try {
            std::vector<ambient::Rgb> colors;
            if (source_->currentIndex() == 0) {
                auto image = capture_.grab({x_->value(), y_->value(), width_->value(), height_->value()});
                if (image.isNull()) {
                    status_->setText("Waiting for a screen frame / screen sharing permission…");
                    return;
                }
                // Convert tagged screenshots to sRGB before interpreting their bytes.
                if (image.colorSpace().isValid())
                    image = image.convertedToColorSpace(QColorSpace::SRgb);
                thumbnail_->setPixmap(
                    QPixmap::fromImage(image.scaled(520, 80, Qt::KeepAspectRatio, Qt::FastTransformation)));
                // Nearest-neighbor subsampling preserves actual source colors for dominant extraction.
                image = image.scaled(240, 80, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                            .convertToFormat(QImage::Format_RGBA8888);
                std::vector<ambient::Pixel> pixels;
                pixels.reserve(static_cast<std::size_t>(image.width() * image.height()));
                for (int y = 0; y < image.height(); ++y) {
                    const auto *row = image.constScanLine(y);
                    for (int x = 0; x < image.width(); ++x)
                        pixels.push_back({row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]});
                }
                ambient::SamplingOptions options;
                options.method =
                    sampling_->currentIndex() == 0 ? ambient::Sampling::Dominant : ambient::Sampling::Average;
                options.blackThreshold = static_cast<float>(black_->value());
                colors = ambient::sample(pixels, static_cast<unsigned>(image.width()),
                                         static_cast<unsigned>(image.height()), 1, options, 1);
            } else {
                static const std::array<ambient::Rgb, 8> tests = {{{1, 0, 0},
                                                                   {0, 1, 0},
                                                                   {0, 0, 1},
                                                                   {1, 1, 1},
                                                                   {0, 1, 1},
                                                                   {1, 0, 1},
                                                                   {1, 1, 0},
                                                                   {.5f, .5f, .5f}}};
                colors.push_back(tests.at(static_cast<std::size_t>(source_->currentIndex() - 1)));
                thumbnail_->setText("Calibration color · screen capture is inactive");
            }
            swatch(raw_, colors.front());
            const float dt = static_cast<float>(elapsed_.restart()) / 1000.f;
            colors = smoother_.update(colors, dt, static_cast<float>(smoothing_->value()) / 1000.f);
            ambient::Calibration k;
            k.red = static_cast<float>(gains_[0]->value());
            k.green = static_cast<float>(gains_[1]->value());
            k.blue = static_cast<float>(gains_[2]->value());
            k.saturation = static_cast<float>(saturation_->value());
            k.gamma = static_cast<float>(gamma_->value());
            k.hueDegrees = static_cast<float>(hue_->value());
            for (auto &c : colors)
                c = ambient::calibrate(c, k);
            const float brightness = static_cast<float>(brightness_->value());
            auto display = colors.front();
            display.r *= brightness;
            display.g *= brightness;
            display.b *= brightness;
            swatch(output_, display);
            busy_ = true;
            const auto session = generation_;
            QMetaObject::invokeMethod(
                worker_,
                [this, colors = std::move(colors), brightness, session] {
                    QString error;
                    try {
                        if (worker_->output)
                            worker_->output->write(colors, brightness);
                    } catch (const std::exception &e) {
                        error = QString::fromUtf8(e.what());
                    }
                    QMetaObject::invokeMethod(
                        this,
                        [this, error, session] {
                            if (session != generation_)
                                return;
                            busy_ = false;
                            if (!error.isEmpty()) {
                                stop();
                                status_->setText("Lighting error: " + error);
                            }
                        },
                        Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
        } catch (const std::exception &e) {
            stop();
            status_->setText(QString::fromUtf8(e.what()));
        }
    }

  public:
    void exercisePreview() {
        // Used by the offscreen smoke test; never discovers or opens real devices.
        loading_ = true;
        brightness_->setValue(.5);
        gamma_->setValue(1);
        saturation_->setValue(1);
        hue_->setValue(0);
        for (auto *gain : gains_)
            gain->setValue(1);
        loading_ = false;
        source_->setCurrentIndex(1);
        start();
    }
    bool previewHealthy() const {
        return active_ && raw_->toolTip() == "#ff0000" && output_->toolTip() == "#800000";
    }
    Window() {
        setWindowTitle("Keyboard Ambient");
        resize(680, 780);
        worker_->moveToThread(&io_);
        connect(&io_, &QThread::finished, worker_, &QObject::deleteLater);
        io_.start();
        auto *root = new QWidget;
        auto *layout = new QVBoxLayout(root);
        setCentralWidget(root);
        auto *title = new QLabel("Keyboard Ambient");
        auto font = title->font();
        font.setPointSize(22);
        title->setFont(font);
        layout->addWidget(title);
        layout->addWidget(new QLabel("Screen colors, across keyboards and desktops"));
        auto *deviceRow = new QHBoxLayout;
        device_ = new QComboBox;
        device_->setMinimumContentsLength(20);
        device_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        refresh_ = new QPushButton("Discover devices");
        deviceRow->addWidget(device_, 1);
        deviceRow->addWidget(refresh_);
        layout->addLayout(deviceRow);
        deviceInfo_ = new QLabel;
        deviceInfo_->setWordWrap(true);
        layout->addWidget(deviceInfo_);
        auto *tabs = new QTabWidget;
        layout->addWidget(tabs);
        auto *captureTab = new QWidget;
        auto *form = new QFormLayout(captureTab);
        tabs->addTab(captureTab, "Screen & effect");
        source_ = new QComboBox;
        source_->addItems({"Screen region", "Test: red", "Test: green", "Test: blue", "Test: white",
                           "Test: cyan", "Test: magenta", "Test: yellow", "Test: gray"});
        form->addRow("Color source", source_);
        screen_ = new QComboBox;
        for (auto *s : QApplication::screens())
            screen_->addItem(
                QString("%1 — %2 × %3").arg(s->name()).arg(s->size().width()).arg(s->size().height()));
        form->addRow("Monitor", screen_);
        captureInfo_ = new QLabel(
            ScreenSource::portalAvailable()
                ? "Desktop capture + Qt Multimedia available. Wayland uses the system screen picker."
                : "Desktop capture available. Wayland needs Qt 6.5+ Multimedia.");
        captureInfo_->setWordWrap(true);
        form->addRow(captureInfo_);
        x_ = spin(0, 99, 0, "region/x", " %");
        y_ = spin(0, 99, 80, "region/y", " %");
        width_ = spin(1, 100, 100, "region/width", " %");
        height_ = spin(1, 100, 17, "region/height", " %");
        auto *position = new QHBoxLayout;
        position->addWidget(new QLabel("X"));
        position->addWidget(x_);
        position->addWidget(new QLabel("Y"));
        position->addWidget(y_);
        form->addRow("Region origin", position);
        auto *size = new QHBoxLayout;
        size->addWidget(new QLabel("W"));
        size->addWidget(width_);
        size->addWidget(new QLabel("H"));
        size->addWidget(height_);
        form->addRow("Region size", size);
        sampling_ = new QComboBox;
        sampling_->addItems({"Dominant color — preserve scene tones", "Average — blend the region"});
        sampling_->setCurrentIndex(settings_.value("sampling", 0).toInt());
        form->addRow("Color extraction", sampling_);
        connect(sampling_, &QComboBox::currentIndexChanged, this,
                [this](int i) { settings_.setValue("sampling", i); });
        black_ = number(0, .3, .01);
        black_->setValue(settings_.value("blackThreshold", .04).toDouble());
        form->addRow("Ignore pixels darker than", black_);
        connect(black_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) { settings_.setValue("blackThreshold", v); });
        fps_ = spin(1, 30, 20, "fps", " fps");
        smoothing_ = spin(0, 2000, 200, "smoothing", " ms");
        form->addRow("Update rate", fps_);
        form->addRow("Transition time", smoothing_);
        connect(fps_, &QSpinBox::valueChanged, this, [this](int fps) {
            if (timer_.isActive())
                timer_.setInterval(1000 / fps);
        });
        auto *colorTab = new QWidget;
        auto *colorForm = new QFormLayout(colorTab);
        tabs->addTab(colorTab, "Device color");
        auto *colorHelp =
            new QLabel("Calibration is saved per device. Start with test white to adjust RGB balance, then "
                       "test red/green/blue. Screen previews show intended RGB; LEDs can differ.");
        colorHelp->setWordWrap(true);
        colorForm->addRow(colorHelp);
        brightness_ = number(0, 1, .05);
        saturation_ = number(0, 2, .05);
        gamma_ = number(.2, 3, .05);
        hue_ = number(-180, 180, 1);
        colorForm->addRow("Brightness", brightness_);
        colorForm->addRow("Saturation", saturation_);
        colorForm->addRow("Output gamma", gamma_);
        colorForm->addRow("Hue correction (degrees)", hue_);
        const QStringList names{"Red gain", "Green gain", "Blue gain"};
        for (int i = 0; i < 3; ++i) {
            gains_[i] = number(0, 2, .05);
            colorForm->addRow(names[i], gains_[i]);
        }
        for (auto *field : {brightness_, saturation_, gamma_, hue_, gains_[0], gains_[1], gains_[2]})
            connect(field, &QDoubleSpinBox::valueChanged, this, [this](double) { persistCalibration(); });
        auto *reset = new QPushButton("Reset device calibration");
        colorForm->addRow(reset);
        connect(reset, &QPushButton::clicked, this, [this] {
            loading_ = true;
            brightness_->setValue(.5);
            saturation_->setValue(1);
            gamma_->setValue(1);
            hue_->setValue(0);
            for (auto *g : gains_)
                g->setValue(1);
            loading_ = false;
            persistCalibration();
        });
        auto *connectionsTab = new QWidget;
        auto *connections = new QFormLayout(connectionsTab);
        tabs->addTab(connectionsTab, "Connections");
        host_ = new QLineEdit(settings_.value("host", "127.0.0.1").toString());
        port_ = spin(1, 65535, 6742, "port");
        connections->addRow("OpenRGB server", host_);
        connections->addRow("Port", port_);
        connect(host_, &QLineEdit::textChanged, this,
                [this](const QString &v) { settings_.setValue("host", v); });
        auto *support = new QLabel("OpenRGB discovers devices by name and uses Direct mode for live updates. "
                                   "Standard VIA RGB Matrix/RGB Light devices connect directly over HID. "
                                   "Wireless control depends on the receiver's firmware. Devices without "
                                   "software RGB control require an additional driver or firmware support.");
        support->setWordWrap(true);
        connections->addRow(support);
#ifdef Q_OS_LINUX
        auto *access = new QPushButton("Export USB access rule for selected device…");
        connections->addRow(access);
        connect(access, &QPushButton::clicked, this, [this] { exportAccessRule(); });
#endif
        thumbnail_ = new QLabel("Captured screen region");
        thumbnail_->setAlignment(Qt::AlignCenter);
        thumbnail_->setMinimumHeight(80);
        thumbnail_->setMaximumHeight(90);
        layout->addWidget(thumbnail_);
        auto *swatches = new QHBoxLayout;
        auto addSwatch = [&](const QString &name) {
            auto *box = new QVBoxLayout;
            box->addWidget(new QLabel(name));
            auto *label = new QLabel;
            label->setMinimumHeight(44);
            box->addWidget(label);
            swatches->addLayout(box);
            return label;
        };
        raw_ = addSwatch("Extracted screen color");
        output_ = addSwatch("Calibrated output");
        swatch(raw_, {});
        swatch(output_, {});
        layout->addLayout(swatches);
        auto *buttons = new QHBoxLayout;
        start_ = new QPushButton("Start");
        stop_ = new QPushButton("Stop & restore");
        buttons->addWidget(start_);
        buttons->addWidget(stop_);
        layout->addLayout(buttons);
        status_ = new QLabel("Ready");
        status_->setWordWrap(true);
        status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(status_);
        connect(device_, &QComboBox::currentIndexChanged, this, [this](int) {
            if (!loading_) {
                settings_.setValue("selectedDevice", devices_.value(device_->currentIndex()).key);
                loadCalibration();
            }
        });
        connect(refresh_, &QPushButton::clicked, this, [this] { refresh(); });
        connect(start_, &QPushButton::clicked, this, [this] { start(); });
        connect(stop_, &QPushButton::clicked, this, [this] { stop(); });
        connect(&timer_, &QTimer::timeout, this, [this] { frame(); });
        connect(&capture_, &ScreenSource::failed, this, [this](const QString &message) {
            if (active_)
                stop();
            status_->setText("Capture error: " + message);
        });
        // A usable preview is present immediately; discovery is asynchronous.
        Device preview;
        preview.label = "Preview only";
        preview.kind = DeviceKind::Preview;
        preview.key = "preview";
        preview.details = "Preview · no device writes";
        devices_.push_back(preview);
        device_->blockSignals(true);
        device_->addItem(preview.label);
        device_->blockSignals(false);
        loadCalibration();
        setActive(false);
        if (!qApp->arguments().contains("--smoke-test"))
            QTimer::singleShot(0, this, [this] { refresh(); });
    }
    ~Window() override {
        stop();
        io_.quit();
        io_.wait();
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("KeyboardAmbient");
    QCoreApplication::setApplicationName("KeyboardAmbient");
    QCoreApplication::setApplicationVersion(QStringLiteral(AMBIENT_RELEASE_TAG));
    const bool smoke = app.arguments().contains("--smoke-test");
    if (smoke)
        QCoreApplication::setApplicationName("KeyboardAmbientSmokeTest");
    Window window;
    window.show();
    if (smoke) {
        QTimer::singleShot(0, &window, [&window] { window.exercisePreview(); });
        auto *probe = new QTimer(&app);
        QObject::connect(probe, &QTimer::timeout, &app, [&app, &window] {
            if (!window.previewHealthy())
                return;
            const auto args = app.arguments();
            const int snapshot = args.indexOf("--snapshot");
            if (snapshot >= 0 && snapshot + 1 < args.size())
                window.grab().save(args[snapshot + 1]);
            app.exit(0);
        });
        probe->start(25);
        QTimer::singleShot(5000, &app, [&app] { app.exit(1); });
    }
    return app.exec();
}
