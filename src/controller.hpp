#pragma once

#include "config.hpp"
#include "niri.hpp"
#include "pet_window.hpp"
#include "services.hpp"
#include "video.hpp"

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QPoint>
#include <QSet>
#include <QTimer>

class QDialog;
class QLabel;
class QMenu;
class QSystemTrayIcon;
class QTreeWidget;

namespace catwhale {

struct ClipLoop {
    bool enabled = false;
    QString action = "idle";
    double start = 0;
    double end = 2;
    bool operator==(const ClipLoop &) const = default;
};

class Controller final : public QObject {
    Q_OBJECT
public:
    Controller(Config &config, PetWindow &window, bool hardwareDecode, bool isolated);
    ~Controller() override;

    bool start();
    void command(const QStringList &arguments);
    QString clipBackend() const { return backend_; }

signals:
    void frameDisplayed(const QImage &frame);
    void playbackError(const QString &error);
    void decoderSelected(const QString &backend);

private:
    void updatePlayback(bool restart = false);
    void updateActivity();
    void touch();
    void react(QString action = {}, bool user = true);
    void idle();
    QString weighted(const QString &role, bool interaction) const;
    QString phase() const;
    double directorRate() const;
    void setPetSize(int size);
    void setSpeed(double speed);
    void setShadow(bool enabled);
    void setWorkspaceFollow(bool enabled);
    void setMediaShow(bool enabled);
    void setDockEnabled(bool enabled);
    void setAutostart(bool enabled);
    void saveSettings();
    void savePetState();
    void reloadSkin();
    void watchSkinFiles();

    void setupTray();
    QMenu *buildMenu(bool trayMenu);
    void showMenu(QPoint position = {});
    void closeMenu();
    void showSettings();
    void showAgentDock();
    void showHardware();
    void updateAgentDock();
    void updateHardware();
    void showSkinFolder();
    void reportSettingsError();
    bool setClipLoop(const ClipLoop &loop, bool preview);

    Config &config_;
    PetWindow &window_;
    pet::VideoPlayer player_;
    MediaMonitor media_;
    AgentDock dock_;
    HardwareMonitor hardware_;
    NiriFollow niri_;
    QElapsedTimer clock_;
    QTimer activity_;
    QTimer reloadSkinTimer_;
    QFileSystemWatcher skinFiles_;
    QTimer hardwareTimer_;
    QJsonObject settings_;
    QJsonObject petState_;
    QJsonObject dockConfig_;
    QJsonObject director_;
    QJsonObject hardwareSample_;
    ClipLoop savedLoop_;
    ClipLoop activeLoop_;
    ClipLoop playingLoop_;
    QString playing_;
    QString reaction_;
    QString lounge_ = "idle";
    QString backend_;
    QString previousAgent_;
    QString mediaKind_;
    QSet<QString> failedClips_;
    qint64 lastInput_ = 0;
    qint64 nextLounge_ = 12000;
    qint64 taskUntil_ = 0;
    qint64 reactionUntil_ = 0;
    qint64 nextMedia_ = 0;
    qint64 mediaRestUntil_ = 0;
    qint64 nextCpuSample_ = 0;
    int mediaCycles_ = 0;
    int agentCount_ = 0;
    int size_ = 340;
    double speed_ = 1;
    double playingRate_ = 1;
    bool previewing_ = false;
    bool playingOnce_ = false;
    bool userReaction_ = true;
    bool shadow_ = true;
    bool follow_ = true;
    bool mediaEnabled_ = false;
    bool dockEnabled_ = true;
    bool dockMuted_ = false;
    bool menuOpen_ = false;
    bool settingsOpen_ = false;
    bool isolated_ = false;
    bool started_ = false;

    QMenu *menu_ = nullptr;
    QMenu *trayMenu_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    QDialog *settingsDialog_ = nullptr;
    QDialog *dockDialog_ = nullptr;
    QDialog *hardwareDialog_ = nullptr;
    QLabel *settingsMessage_ = nullptr;
    QLabel *mediaMessage_ = nullptr;
    QLabel *hardwareMessage_ = nullptr;
    QLabel *dockDetails_ = nullptr;
    QTreeWidget *dockList_ = nullptr;
};

} // namespace catwhale
