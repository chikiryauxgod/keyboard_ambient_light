#pragma once
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <memory>

struct CaptureRegion {
    int x{0}, y{80}, width{100}, height{17};
}; // percent of selected display
QRect regionRect(QSize size, CaptureRegion region);

class ScreenSource final : public QObject {
    Q_OBJECT
  public:
    explicit ScreenSource(QObject *parent = nullptr);
    ~ScreenSource() override;
    void start(QScreen *screen);
    void stop();
    QImage grab(CaptureRegion region);
    QString description() const;
    static bool portalAvailable();
  signals:
    void failed(const QString &message);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
