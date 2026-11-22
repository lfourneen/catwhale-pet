#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

namespace catwhale {

struct MediaStatus {
    QString kind;
    QString app;
    QString title;
    QString artist;
    QString error;

    bool playing() const { return !kind.isEmpty(); }
    bool operator==(const MediaStatus &) const = default;
};

class MediaMonitor final : public QObject {
public:
    explicit MediaMonitor(QObject *parent = nullptr);
    ~MediaMonitor() override;

    void configure(const QJsonObject &mediaShow);
    void probeOnce();
    MediaStatus status() const;

    std::function<void(const MediaStatus &)> onChanged;
    std::function<void(const MediaStatus &)> onProbed;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

class AgentDock final : public QObject {
public:
    explicit AgentDock(QObject *parent = nullptr);
    ~AgentDock() override;

    void configure(const QJsonObject &config);
    QJsonArray sessions() const;
    QString state() const;
    QString error() const;
    void clearEnded();

    std::function<void()> onChanged;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

class HardwareMonitor final : public QObject {
public:
    explicit HardwareMonitor(QObject *parent = nullptr);
    ~HardwareMonitor() override;

    QJsonObject sample(bool withDetails = true);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace catwhale
