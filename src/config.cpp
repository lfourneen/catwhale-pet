#include "config.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace catwhale {
namespace {

QString dataDirectory()
{
    const QStringList candidates{
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(CATWHALE_RELATIVE_ASSETS),
        QStringLiteral(CATWHALE_INSTALL_ASSETS),
        QStringLiteral(CATWHALE_BUILD_ASSETS)
    };
    for (const auto &dir : candidates) {
        if (QFileInfo::exists(dir + "/states.json"))
            return QDir::cleanPath(dir);
    }
    return {};
}

QString autostartFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + "/autostart/catwhale-pet.desktop";
}

bool saveFile(const QString &path, const QByteArray &data)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

QString localAsset(const QString &root, QString relative)
{
    relative.replace(u'\\', u'/');
    while (relative.startsWith(u'/'))
        relative.remove(0, 1);
    const auto path = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
    return path.startsWith(QDir(root).canonicalPath() + '/') && QFileInfo(path).isFile()
        ? path : QString{};
}

double number(const QJsonObject &obj, const QString &key, double fallback, double min, double max)
{
    const double value = obj.value(key).toDouble(fallback);
    return std::isfinite(value) ? std::clamp(value, min, max) : fallback;
}

Clip clipFrom(const QJsonObject &entry, const QString &id, const QString &root)
{
    Clip clip;
    clip.path = localAsset(root, entry.value("file").toString());
    auto poster = entry.value("poster").toString();
    if (poster.isEmpty()) {
        const auto file = entry.value("file").toString();
        const auto dot = file.lastIndexOf(u'.');
        if (dot > 0)
            poster = file.left(dot) + "_poster.png";
    }
    if (!poster.isEmpty())
        clip.poster = localAsset(root, poster);
    clip.name = entry.value("name").toString(id);
    clip.role = entry.value("role").toString();
    clip.loop = entry.value("mode").toString() == "loop";
    clip.duration = number(entry, "duration", 4, 0.05, 3600);
    clip.rate = number(entry, "rate", 1, 0.1, 8);
    clip.weight = number(entry, "weight", 20, 0, 10000);
    clip.reaction = number(entry, "react", 0, 0, 10000);
    clip.scale = number(entry, "scale", 1, 0.05, 4);
    clip.scaleX = number(entry, "scaleX", 1, 0.05, 4);
    clip.scaleY = number(entry, "scaleY", 1, 0.05, 4);
    clip.offsetX = number(entry, "offX", 0, -200, 200);
    clip.offsetY = number(entry, "offY", 0, -200, 200);
    return clip;
}

} // namespace

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeJson(const QString &path, const QJsonObject &value)
{
    return saveFile(path, QJsonDocument(value).toJson());
}

QJsonObject mergeJson(QJsonObject defaults, const QJsonObject &overrides)
{
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        const auto existing = defaults.value(it.key());
        defaults.insert(it.key(), existing.isObject() && it->isObject()
            ? QJsonValue(mergeJson(existing.toObject(), it->toObject())) : *it);
    }
    return defaults;
}

Config::Config(QString root)
    : root_(root.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/catwhale-pet"
        : std::move(root)),
      assets_(dataDirectory())
{
    reloadClips();
}

QString Config::file(const QString &name) const { return QDir(root_).filePath(name); }

QString Config::icon() const
{
    const auto installed = assets_ + "/icon.png";
    return QFileInfo::exists(installed) ? installed : QStringLiteral(CATWHALE_BUILD_ICON);
}

QString Config::skinPath() const
{
    const auto slug = readJson(file("skins/active.json")).value("slug").toString();
    static const QRegularExpression valid("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$");
    if (!valid.match(slug).hasMatch())
        return {};
    const auto parent = QDir(file("skins")).canonicalPath();
    const auto path = QDir(file("skins/" + slug)).canonicalPath();
    return !parent.isEmpty() && path.startsWith(parent + '/') ? path : QString{};
}

void Config::reloadClips()
{
    builtInClips_.clear();
    const auto builtIn = readJson(assets_ + "/states.json");
    for (auto it = builtIn.begin(); it != builtIn.end(); ++it) {
        auto clip = clipFrom(it->toObject(), it.key(), assets_ + "/video");
        if (!clip.path.isEmpty())
            builtInClips_.insert(it.key(), std::move(clip));
    }
    clips_ = builtInClips_;
    const auto skin = skinPath();
    skinName_.clear();
    if (skin.isEmpty())
        return;
    const auto manifest = readJson(skin + "/skin.json");
    if (manifest.isEmpty())
        return;
    skinName_ = manifest.value("name").toString(QFileInfo(skin).fileName());
    const auto actions = manifest.value("actions").toObject();
    for (auto it = actions.begin(); it != actions.end(); ++it) {
        auto clip = clipFrom(it->toObject(), it.key(), skin);
        if (!clip.path.isEmpty())
            clips_.insert(it.key(), std::move(clip));
    }
}

bool Config::useBuiltInClip(const QString &id)
{
    const auto builtIn = builtInClips_.constFind(id);
    if (builtIn == builtInClips_.cend() || clips_.value(id).path == builtIn->path)
        return false;
    clips_.insert(id, *builtIn);
    return true;
}

QJsonObject Config::director() const
{
    auto values = readJson(assets_ + "/director-defaults.json");
    values = mergeJson(std::move(values), readJson(file("director.json")));
    const auto skin = skinPath();
    return skin.isEmpty() ? values : mergeJson(std::move(values), readJson(skin + "/director.json"));
}

bool Config::autostart() const
{
    QFile file(autostartFile());
    return file.open(QIODevice::ReadOnly) && file.readAll().contains("X-Catwhale-Pet=true");
}

bool Config::setAutostart(bool enabled) const
{
    const auto path = autostartFile();
    if (!enabled)
        return !QFileInfo::exists(path) || (autostart() && QFile::remove(path));
    if (QFileInfo::exists(path) && !autostart())
        return false;
    auto command = qEnvironmentVariable("CATWHALE_LAUNCHER", QCoreApplication::applicationFilePath());
    command.replace(u'%', "%%").replace(u'\\', "\\\\\\\\").replace(u'"', "\\\\\"")
        .replace(u'`', "\\\\`").replace(u'$', "\\\\$");
    return saveFile(path, QString("[Desktop Entry]\nType=Application\nName=Catwhale Pet\n"
        "Exec=\"%1\"\nIcon=catwhale-pet\nTerminal=false\nStartupNotify=false\n"
        "X-GNOME-Autostart-enabled=true\nX-Catwhale-Pet=true\n").arg(command).toUtf8());
}

} // namespace catwhale
