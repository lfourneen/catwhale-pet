#pragma once

#include <QJsonObject>
#include <QMap>
#include <QSize>
#include <QString>

namespace catwhale {

QJsonObject readJson(const QString &path);
bool writeJson(const QString &path, const QJsonObject &value);
QJsonObject mergeJson(QJsonObject defaults, const QJsonObject &overrides);

struct Clip {
    QString name;
    QString path;
    QString poster;
    QString role;
    bool loop = true;
    double duration = 4;
    double rate = 1;
    double weight = 20;
    double reaction = 0;
    double scale = 1;
    double scaleX = 1;
    double scaleY = 1;
    double offsetX = 0;
    double offsetY = 0;
};

class Config {
public:
    explicit Config(QString root = {});
    void reloadClips();
    bool useBuiltInClip(const QString &id);
    QString file(const QString &name) const;
    const QMap<QString, Clip> &clips() const { return clips_; }
    QString assets() const { return assets_; }
    QString icon() const;
    QString skinPath() const;
    QString skinName() const { return skinName_; }
    QJsonObject director() const;
    bool autostart() const;
    bool setAutostart(bool enabled) const;

private:
    QString root_;
    QString assets_;
    QString skinName_;
    QMap<QString, Clip> clips_;
    QMap<QString, Clip> builtInClips_;
};

} // namespace catwhale
