#include "services.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace catwhale {
namespace {

using namespace Qt::StringLiterals;

constexpr qsizetype maxMediaOutput = 256 * 1024;
constexpr double bytesPerGiB = 1024.0 * 1024 * 1024;

QString clipped(QString text, qsizetype length)
{
    text = text.simplified();
    return text.size() > length ? text.left(length - 1) + u'…' : text;
}

QByteArray readFile(const QString &path, qsizetype limit = 128 * 1024)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(limit) : QByteArray{};
}

QString mediaKind(const QString &name, const QString &artist, const QString &url)
{
    static const QRegularExpression videoPlayers(
        u"chromium|chrome|firefox|brave|edge|mpv|vlc|celluloid|totem|haruna|bilibili|mpc-hc"_s,
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression musicPlayers(
        u"spotify|yesplaymusic|netease|cloudmusic|amberol|lollypop|rhythmbox|cmus|mpd|"
        "mopidy|strawberry|clementine|elisa|deezer"_s, QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression videoFile(u"\\.(mp4|mkv|webm|avi|mov|m3u8)(\\?|$)"_s,
                                              QRegularExpression::CaseInsensitiveOption);
    if (videoPlayers.match(name).hasMatch()) {
        return u"video"_s;
    }
    if (musicPlayers.match(name).hasMatch() || !artist.isEmpty()) {
        return u"music"_s;
    }
    return videoFile.match(url).hasMatch() ? u"video"_s : u"music"_s;
}

MediaStatus parseMedia(const QByteArray &output)
{
    for (const auto &line : output.split('\n')) {
        const auto fields = line.trimmed().split('\t');
        if (fields.size() < 2 || fields[0].isEmpty() || fields[1] != "Playing") {
            continue;
        }
        const auto app = clipped(QString::fromUtf8(fields[0]), 80);
        const auto title = clipped(QString::fromUtf8(fields.value(2)), 240);
        const auto artist = clipped(QString::fromUtf8(fields.value(3)), 160);
        return {mediaKind(app, artist, QString::fromUtf8(fields.value(4))), app, title, artist, {}};
    }
    return {};
}

} // namespace

struct MediaMonitor::Impl {
    explicit Impl(MediaMonitor &owner)
        : owner(owner)
    {
        clock.start();
        poll.setSingleShot(true);
        deadline.setSingleShot(true);
        QObject::connect(&poll, &QTimer::timeout, &owner, [this] { start(); });
        QObject::connect(&deadline, &QTimer::timeout, &owner, [this] {
            failure = MediaMonitor::tr("Media query timed out.");
            process.kill();
        });
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &owner, [this] {
            output += process.readAllStandardOutput();
            if (output.size() > maxMediaOutput) {
                output.clear();
                failure = MediaMonitor::tr("Media metadata exceeded the query limit.");
                process.kill();
            }
        });
        QObject::connect(&process, &QProcess::readyReadStandardError, &owner, [this] {
            process.readAllStandardError();
        });
        QObject::connect(&process, &QProcess::errorOccurred, &owner, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                failure = MediaMonitor::tr("Could not start playerctl.");
                finish();
            }
        });
        QObject::connect(&process, &QProcess::finished, &owner, [this](int, QProcess::ExitStatus status) {
            if (status == QProcess::CrashExit && failure.isEmpty()) {
                failure = MediaMonitor::tr("Media query ended unexpectedly.");
            }
            finish();
        });
    }

    void start()
    {
        if (process.state() != QProcess::NotRunning || (!enabled && !probeRequested)) {
            return;
        }
        output.clear();
        failure.clear();
        deadline.start(3000);
        process.start(u"playerctl"_s, {u"-a"_s, u"metadata"_s, u"--format"_s,
            u"{{playerName}}\t{{status}}\t{{xesam:title}}\t{{xesam:artist}}\t{{xesam:url}}"_s});
    }

    void publish(MediaStatus next)
    {
        if (status != next) {
            status = std::move(next);
            if (owner.onChanged) {
                owner.onChanged(status);
            }
        }
    }

    void finish()
    {
        deadline.stop();
        output += process.readAllStandardOutput();
        auto next = failure.isEmpty() && output.size() <= maxMediaOutput ? parseMedia(output) : MediaStatus{};
        next.error = failure;
        if (std::exchange(probeRequested, false) && owner.onProbed) {
            owner.onProbed(next);
        }
        if (!enabled) {
            return;
        }

        auto nextPoll = interval;
        if (next.kind == status.kind || !next.error.isEmpty()) {
            pending = false;
            publish(next);
        } else {
            const auto now = clock.elapsed();
            if (!pending || pendingKind != next.kind) {
                pending = true;
                pendingKind = next.kind;
                pendingSince = now;
            }
            const auto delay = next.playing() ? startDelay : stopDelay;
            const auto remaining = delay - (now - pendingSince);
            if (remaining <= 0) {
                pending = false;
                publish(next);
            } else {
                nextPoll = std::min(interval, static_cast<int>(remaining));
            }
        }
        if (enabled) {
            poll.start(nextPoll);
        }
    }

    MediaMonitor &owner;
    QProcess process;
    QTimer poll;
    QTimer deadline;
    QElapsedTimer clock;
    QByteArray output;
    QString failure;
    MediaStatus status;
    QString pendingKind;
    qint64 pendingSince = 0;
    int interval = 2000;
    int startDelay = 1200;
    int stopDelay = 2500;
    bool enabled = false;
    bool pending = false;
    bool probeRequested = false;
};

MediaMonitor::MediaMonitor(QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(*this))
{
}

MediaMonitor::~MediaMonitor()
{
    d->process.disconnect(this);
}

void MediaMonitor::configure(const QJsonObject &config)
{
    d->enabled = config.value("enabled"_L1).toBool();
    d->interval = std::clamp(config.value("pollMs"_L1).toInt(2000), 500, 60'000);
    d->startDelay = std::clamp(config.value("startMs"_L1).toInt(1200), 0, 60'000);
    d->stopDelay = std::clamp(config.value("stopMs"_L1).toInt(2500), 0, 60'000);
    d->poll.stop();
    if (d->enabled) {
        d->start();
    } else {
        d->pending = false;
        d->publish({});
    }
}

void MediaMonitor::probeOnce()
{
    d->probeRequested = true;
    d->poll.stop();
    d->start();
}

MediaStatus MediaMonitor::status() const
{
    return d->status;
}

namespace {

struct CpuTimes {
    quint64 total = 0;
    quint64 idle = 0;
};

CpuTimes cpuTimes()
{
    const auto fields = readFile(u"/proc/stat"_s, 4096).split('\n').value(0).simplified().split(' ');
    CpuTimes result;
    if (fields.value(0) != "cpu" || fields.size() < 5) {
        return result;
    }
    // Linux guest time is already included in user and nice time.
    for (qsizetype i = 1; i < std::min<qsizetype>(9, fields.size()); ++i) {
        result.total += fields[i].toULongLong();
    }
    result.idle = fields.value(4).toULongLong() + fields.value(5).toULongLong();
    return result;
}

QJsonValue numberOrNull(const QByteArray &value, double divisor = 1)
{
    bool ok = false;
    const auto number = value.trimmed().toDouble(&ok);
    return ok ? QJsonValue(number / divisor) : QJsonValue(QJsonValue::Null);
}

QJsonValue cpuFrequency()
{
    const QDir policies(u"/sys/devices/system/cpu/cpufreq"_s);
    const auto folders = policies.entryList({u"policy*"_s}, QDir::Dirs | QDir::NoDotAndDotDot);
    double sum = 0;
    int count = 0;
    for (const auto &folder : folders) {
        const auto value = numberOrNull(readFile(policies.filePath(folder + u"/scaling_cur_freq"_s), 64), 1000);
        if (value.isDouble()) {
            sum += value.toDouble();
            ++count;
        }
    }
    if (count == 0) {
        static const QRegularExpression pattern(u"^cpu MHz\\s*:\\s*([\\d.]+)"_s, QRegularExpression::MultilineOption);
        const auto contents = QString::fromUtf8(readFile(u"/proc/cpuinfo"_s));
        auto matches = pattern.globalMatch(contents);
        while (matches.hasNext()) {
            sum += matches.next().capturedView(1).toDouble();
            ++count;
        }
    }
    return count ? QJsonValue(sum / count) : QJsonValue(QJsonValue::Null);
}

QJsonObject memory()
{
    quint64 total = 0;
    quint64 available = 0;
    for (const auto &line : readFile(u"/proc/meminfo"_s, 4096).split('\n')) {
        const auto split = line.indexOf(':');
        if (split < 0) continue;
        const auto name = line.first(split);
        const auto size = line.sliced(split + 1).simplified().split(' ').value(0).toULongLong();
        if (name == "MemTotal") total = size;
        if (name == "MemAvailable") available = size;
    }
    return {{u"memTotalGB"_s, total * 1024.0 / bytesPerGiB},
            {u"memUsedGB"_s, (total - std::min(total, available)) * 1024.0 / bytesPerGiB},
            {u"memSpeedMHz"_s, QJsonValue::Null}};
}

QJsonArray disks()
{
    QJsonArray result;
    QSet<QByteArray> seen;
    for (const auto &path : {u"/"_s, QDir::homePath()}) {
        const QStorageInfo volume(path);
        const auto total = volume.bytesTotal();
        if (!volume.isReady() || total <= 0 || seen.contains(volume.device())) {
            continue;
        }
        seen.insert(volume.device());
        const auto available = std::clamp<qint64>(volume.bytesAvailable(), 0, total);
        result.append(QJsonObject{{u"drive"_s, path}, {u"freeGB"_s, available / bytesPerGiB},
            {u"totalGB"_s, total / bytesPerGiB}, {u"usedPct"_s, 100.0 * (total - available) / total}});
    }
    return result;
}

QJsonValue gpuTemperature(const QDir &hwmon)
{
    const auto folders = hwmon.entryList({u"hwmon*"_s}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &folder : folders) {
        const auto temp = numberOrNull(readFile(hwmon.filePath(folder + u"/temp1_input"_s), 64), 1000);
        if (temp.isDouble()) return temp;
    }
    return QJsonValue::Null;
}

QJsonObject sysfsGpu()
{
    static const QRegularExpression cardName(u"^card[0-9]+$"_s);
    const QDir drm(u"/sys/class/drm"_s);
    for (const auto &card : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!cardName.match(card).hasMatch()) continue;
        const auto path = drm.filePath(card);
        const auto device = path + u"/device"_s;
        const auto vendor = readFile(device + u"/vendor"_s, 16).trimmed();
        if (vendor == "0x1002") {
            static const QRegularExpression activeClock(u"([0-9]+)\\s*MHz\\s*\\*"_s,
                                                        QRegularExpression::CaseInsensitiveOption);
            const auto levels = QString::fromUtf8(readFile(device + u"/pp_dpm_sclk"_s, 4096));
            const auto clock = activeClock.match(levels).captured(1).toLatin1();
            return {{u"name"_s, u"AMD GPU"_s}, {u"clockMHz"_s, numberOrNull(clock)},
                {u"util"_s, numberOrNull(readFile(device + u"/gpu_busy_percent"_s, 64))},
                {u"tempC"_s, gpuTemperature(QDir(device + u"/hwmon"_s))}};
        }
        if (vendor == "0x8086") {
            return {{u"name"_s, u"Intel GPU"_s},
                {u"clockMHz"_s, numberOrNull(readFile(path + u"/gt_cur_freq_mhz"_s, 64))},
                {u"util"_s, numberOrNull(readFile(path + u"/gt_busy_percent"_s, 64))},
                {u"tempC"_s, gpuTemperature(QDir(device + u"/hwmon"_s))}};
        }
    }
    return {{u"name"_s, QString{}}, {u"clockMHz"_s, QJsonValue::Null},
            {u"util"_s, QJsonValue::Null}, {u"tempC"_s, QJsonValue::Null}};
}

} // namespace

struct HardwareMonitor::Impl {
    explicit Impl(HardwareMonitor &owner)
    {
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &owner, [this] { process.kill(); });
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &owner, [this] {
            output += process.readAllStandardOutput();
            if (output.size() > 64 * 1024) {
                output.clear();
                process.kill();
            }
        });
        QObject::connect(&process, &QProcess::readyReadStandardError, &owner, [this] {
            process.readAllStandardError();
        });
        QObject::connect(&process, &QProcess::errorOccurred, &owner, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                deadline.stop();
            }
        });
        QObject::connect(&process, &QProcess::finished, &owner, [this](int code, QProcess::ExitStatus status) {
            deadline.stop();
            if (code != 0 || status != QProcess::NormalExit) {
                gpu = sysfsGpu();
                pollDelay = 30'000;
                return;
            }
            output += process.readAllStandardOutput();
            const auto fields = output.split('\n').value(0).split(',');
            if (fields.size() >= 4) {
                gpu = {{u"name"_s, QString::fromUtf8(fields[0]).trimmed()}, {u"clockMHz"_s, numberOrNull(fields[1])},
                       {u"util"_s, numberOrNull(fields[2])}, {u"tempC"_s, numberOrNull(fields[3])}};
            }
        });
        previous = cpuTimes();
    }

    void probeGpu()
    {
        if (process.state() != QProcess::NotRunning || (lastPoll.isValid() && lastPoll.elapsed() < pollDelay)) {
            return;
        }
        lastPoll.start();
        if (binary.isEmpty()) {
            gpu = sysfsGpu();
            return;
        }
        output.clear();
        deadline.start(2000);
        process.start(binary, {u"--query-gpu=name,clocks.gr,utilization.gpu,temperature.gpu"_s,
                               u"--format=csv,noheader,nounits"_s});
    }

    CpuTimes previous;
    QString binary = QStandardPaths::findExecutable(u"nvidia-smi"_s);
    QProcess process;
    QTimer deadline;
    QElapsedTimer lastPoll;
    QByteArray output;
    QJsonObject gpu;
    int pollDelay = 2000;
};

HardwareMonitor::HardwareMonitor(QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(*this))
{
}

HardwareMonitor::~HardwareMonitor()
{
    d->process.disconnect(this);
}

QJsonObject HardwareMonitor::sample(bool withDetails)
{
    const auto previous = std::exchange(d->previous, cpuTimes());
    auto result = memory();
    QJsonValue usage(QJsonValue::Null);
    if (previous.total && d->previous.total > previous.total && d->previous.idle >= previous.idle) {
        const auto total = d->previous.total - previous.total;
        const auto idleTime = std::min(total, d->previous.idle - previous.idle);
        usage = 100.0 * (total - idleTime) / total;
    }
    result.insert(u"cpuUsage"_s, usage);
    if (withDetails) {
        d->probeGpu();
        result.insert(u"cpuFreqMHz"_s, cpuFrequency());
        result.insert(u"gpu"_s, d->gpu);
        result.insert(u"disks"_s, disks());
    }
    return result;
}

} // namespace catwhale
