#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

namespace pet {

class VideoPlayer final : public QObject
{
    Q_OBJECT

public:
    explicit VideoPlayer(bool allowHardware = true, QObject *parent = nullptr);
    ~VideoPlayer() override;

    void play(const QString &path, bool loop = true, double speed = 1.0,
              double start = 0.0, double end = 0.0);
    void stop();
    void setSpeed(double speed);
    void setTargetSize(QSize physicalPixels);

signals:
    void frameReady(const QImage &frame);
    void finished();
    void backendChanged(const QString &backend);
    void failed(const QString &message);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace pet
