#include "video.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QTimer>

#include <functional>
#include <utility>

namespace {

constexpr auto timeBudgetMs = 20000;
const QSize targetPixels(180, 300);

bool hasAlphaCoverage(const QImage &frame)
{
    if (frame.format() != QImage::Format_RGBA8888_Premultiplied
        || frame.width() > targetPixels.width() || frame.height() > targetPixels.height())
        return false;

    bool transparent = false;
    bool visible = false;
    for (int y = 0; y < frame.height(); ++y) {
        const auto *row = frame.constScanLine(y);
        for (int x = 0; x < frame.width(); ++x) {
            transparent |= row[4 * x + 3] < 8;
            visible |= row[4 * x + 3] > 240;
        }
        if (transparent && visible)
            return true;
    }
    return false;
}

class ClipVerification
{
public:
    ClipVerification(QCoreApplication &app, QDir directory, const QStringList &clips, bool allowHardware)
        : app_(app), directory_(std::move(directory)), clips_(clips), allowHardware_(allowHardware), player_(allowHardware)
    {
        player_.setTargetSize(targetPixels);
        QObject::connect(&player_, &pet::VideoPlayer::backendChanged, &app_, [this](const QString &backend) {
            if (!allowHardware_ && backend != QStringLiteral("CPU"))
                fail(QStringLiteral("Unexpected decoder with hardware disabled: %1").arg(backend));
            backend_ = backend;
        });
        QObject::connect(&player_, &pet::VideoPlayer::frameReady, &app_, [this](const QImage &frame) {
            if (!hasAlphaCoverage(frame))
                fail(QStringLiteral("Decoded frame lost opacity, transparency or target dimensions."));
            gotFrame_ = true;
        });
        QObject::connect(&player_, &pet::VideoPlayer::failed, &app_, [this](const QString &reason) {
            fail(reason);
        });
        QObject::connect(&player_, &pet::VideoPlayer::finished, &app_, [this] {
            if (!gotFrame_ || backend_.isEmpty()) {
                fail(QStringLiteral("Playback completed without a frame or a decoder report."));
                return;
            }
            ++index_;
            next();
        });
    }

    void next()
    {
        if (index_ == clips_.size()) {
            qInfo() << "Verified alpha and clip seeking with" << backend_ << "on" << index_ << "clip(s).";
            if (done)
                done();
            return;
        }
        gotFrame_ = false;
        const auto file = directory_.absoluteFilePath(clips_.at(index_));
        player_.play(file, false, 8.0, 0.25, 0.45);
    }

    std::function<void()> done;

private:
    void fail(const QString &reason)
    {
        qCritical().noquote() << clips_.value(index_) << reason;
        player_.stop();
        app_.exit(1);
    }

    QCoreApplication &app_;
    QDir directory_;
    QStringList clips_;
    bool allowHardware_;
    pet::VideoPlayer player_;
    QString backend_;
    qsizetype index_ = 0;
    bool gotFrame_ = false;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        qCritical() << "Expected a directory of built-in WebM clips.";
        return 1;
    }
    const QDir directory(app.arguments().at(1));
    const auto clips = directory.entryList({QStringLiteral("*.webm")}, QDir::Files);
    if (clips.isEmpty() || !clips.contains(QStringLiteral("idle.webm"))) {
        qCritical() << "Missing built-in WebM clips:" << directory.absolutePath();
        return 1;
    }

    ClipVerification software(app, directory, clips, false);
    ClipVerification automatic(app, directory, {QStringLiteral("idle.webm")}, true);
    software.done = [&] { automatic.next(); };
    automatic.done = [&] { app.exit(0); };
    QTimer::singleShot(0, &app, [&] { software.next(); });
    QTimer::singleShot(timeBudgetMs, &app, [&] {
        qCritical() << "Playback verification timed out.";
        app.exit(1);
    });
    return app.exec();
}
