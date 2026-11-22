#include "controller.hpp"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QMenu>
#include <QRandomGenerator>
#include <QSystemTrayIcon>
#include <QTime>

#include <cmath>
#include <limits>

namespace catwhale {
namespace {

constexpr qint64 idleHold = 12000;

ClipLoop readLoop(const QJsonObject &object)
{
    return {object.value("enabled").toBool(), object.value("actionId").toString("idle"),
        object.value("start").toDouble(), object.value("end").toDouble(2)};
}

bool validLoop(const ClipLoop &loop, const QMap<QString, Clip> &clips)
{
    const auto clip = clips.constFind(loop.action);
    return clip != clips.cend() && std::isfinite(loop.start) && std::isfinite(loop.end)
        && loop.start >= 0 && loop.end - loop.start >= 0.049999 && loop.end <= clip->duration + 0.01;
}

} // namespace

Controller::Controller(Config &config, PetWindow &window, bool hardwareDecode, bool isolated)
    : config_(config), window_(window), player_(hardwareDecode), isolated_(isolated)
{
    settings_ = readJson(config_.file("settings.json"));
    petState_ = readJson(config_.file("pet-state.json"));
    dockConfig_ = readJson(config_.file("dock.json"));
    director_ = config_.director();
    const auto storedLoop = readLoop(readJson(config_.file("animation-loop.json")));
    if (validLoop(storedLoop, config_.clips()))
        savedLoop_ = activeLoop_ = storedLoop;

    size_ = std::clamp(petState_.value("size").toInt(340), 200, 620);
    speed_ = std::clamp(petState_.value("speed").toDouble(1), 0.5, 2.0);
    shadow_ = petState_.value("shadow").toBool(true);
    follow_ = settings_.value("niri").toObject().value("followWorkspace").toBool(true);
    mediaEnabled_ = settings_.value("media").toObject().value("show").toObject().value("enabled").toBool();
    dockEnabled_ = dockConfig_.value("enabled").toBool(true);
    dockMuted_ = dockConfig_.value("muted").toBool();

    connect(&window_, &PetWindow::clicked, this, [this] { react(); });
    connect(&window_, &PetWindow::dragStarted, this, [this] { touch(); updatePlayback(); });
    connect(&window_, &PetWindow::dragFinished, this, [this] { react("drop"); });
    connect(&window_, &PetWindow::menuRequested, this, &Controller::showMenu);
    connect(&window_, &PetWindow::agentDockRequested, this, &Controller::showAgentDock);
    connect(&window_, &PetWindow::panelToggled, this, [this] { command({"--toggle-menu"}); });
    connect(&window_, &PetWindow::targetSizeChanged, &player_, &pet::VideoPlayer::setTargetSize);
    connect(&window_, &QWindow::visibilityChanged, this, [this](QWindow::Visibility state) {
        if (!started_)
            return;
        if (state == QWindow::Hidden || state == QWindow::Minimized) {
            player_.stop();
            playing_.clear();
            activity_.stop();
        } else {
            activity_.start();
            updatePlayback();
        }
    });
    connect(&player_, &pet::VideoPlayer::frameReady, this, [this](const QImage &frame) {
        const auto clip = config_.clips().constFind(playing_);
        if (clip == config_.clips().cend())
            return;
        window_.present(frame, *clip);
        emit frameDisplayed(frame);
    });
    connect(&player_, &pet::VideoPlayer::finished, this, [this] {
        if (playing_ == reaction_)
            reaction_.clear();
        if (playing_ == lounge_)
            lounge_ = "idle";
        playing_.clear();
        updatePlayback();
    });
    connect(&player_, &pet::VideoPlayer::backendChanged, this, [this](const QString &backend) {
        backend_ = backend;
        qInfo().noquote() << "Video decoder:" << backend;
        emit decoderSelected(backend);
    });
    connect(&player_, &pet::VideoPlayer::failed, this, [this](const QString &message) {
        qWarning().noquote() << "Clip" << playing_ << message;
        if (config_.useBuiltInClip(playing_)) {
            playing_.clear();
            updatePlayback();
            return;
        }
        if (playing_ == "idle") {
            emit playbackError(message);
            return;
        }
        failedClips_.insert(playing_);
        if (playing_ == reaction_)
            reaction_.clear();
        playing_.clear();
        updatePlayback();
    });
    connect(&activity_, &QTimer::timeout, this, &Controller::updateActivity);
    activity_.setInterval(350);
    connect(&hardwareTimer_, &QTimer::timeout, this, &Controller::updateHardware);
    hardwareTimer_.setInterval(1500);
    reloadSkinTimer_.setSingleShot(true);
    reloadSkinTimer_.setInterval(160);
    connect(&reloadSkinTimer_, &QTimer::timeout, this, &Controller::reloadSkin);
    connect(&skinFiles_, &QFileSystemWatcher::fileChanged, this, [this] { reloadSkinTimer_.start(); });
    connect(&skinFiles_, &QFileSystemWatcher::directoryChanged, this, [this] { reloadSkinTimer_.start(); });

    media_.onChanged = [this](const MediaStatus &status) {
        if (mediaKind_ != status.kind) {
            mediaKind_ = status.kind;
            mediaCycles_ = 0;
            nextMedia_ = mediaRestUntil_ = 0;
        }
        updatePlayback();
    };
    dock_.onChanged = [this] {
        updateAgentDock();
        const auto state = dock_.state();
        const auto count = dock_.sessions().size();
        if (count != agentCount_ || state != previousAgent_) {
            agentCount_ = count;
            window_.setAgentBadge(agentCount_, state);
        }
        if (!dockMuted_ && state != previousAgent_) {
            if (state == "done")
                react("jump_high", false);
            else if (state == "error")
                react("dizzy", false);
        }
        previousAgent_ = state;
        updatePlayback();
    };
}

Controller::~Controller()
{
    media_.onChanged = {};
    media_.onProbed = {};
    dock_.onChanged = {};
    player_.stop();
    delete tray_;
    delete trayMenu_;
    delete menu_;
    delete settingsDialog_;
    delete dockDialog_;
    delete hardwareDialog_;
}

bool Controller::start()
{
    if (!config_.clips().contains("idle")) {
        qCritical() << "The built-in idle clip could not be found.";
        return false;
    }
    clock_.start();
    started_ = true;
    window_.setPetSize(size_);
    window_.setShadow(shadow_);
    window_.show();
    if (!isolated_) {
        setupTray();
        watchSkinFiles();
        media_.configure(settings_.value("media").toObject().value("show").toObject());
        dock_.configure(dockConfig_);
        niri_.setEnabled(follow_);
    }
    updatePlayback();
    return true;
}

void Controller::command(const QStringList &arguments)
{
    if (arguments.contains("--quit")) {
        QApplication::quit();
        return;
    }
    for (const auto &argument : arguments) {
        if (argument.startsWith("--autostart=")) {
            const auto option = argument.section(u'=', 1);
            setAutostart(option == "on" || option == "true" || option == "1");
        }
    }

    if (arguments.contains("--toggle-pet")) {
        window_.setVisible(!window_.isVisible());
        return;
    }
    if (arguments.contains("--show"))
        window_.show();
    if (arguments.contains("--toggle-menu") && menuOpen_) {
        closeMenu();
    } else if (arguments.contains("--toggle-menu") || arguments.contains("--open-menu")) {
        window_.show();
        showMenu();
    }
    if (arguments.contains("--toggle-settings") && settingsOpen_) {
        settingsDialog_->close();
    } else if (arguments.contains("--toggle-settings") || arguments.contains("--open-settings")) {
        showSettings();
    }
}

void Controller::updatePlayback(bool restart)
{
    if (!started_ || !window_.isVisible() || window_.visibility() == QWindow::Minimized)
        return;

    const auto now = clock_.elapsed();
    QString action;
    bool once = false;
    ClipLoop span;
    const auto agent = dockMuted_ ? QString{} : dock_.state();
    if (window_.dragInProgress()) {
        action = "grabbed";
    } else if (activeLoop_.enabled && (!userReaction_ || reaction_.isEmpty())) {
        action = activeLoop_.action;
        span = activeLoop_;
    } else if (activeLoop_.enabled && userReaction_ && !reaction_.isEmpty()) {
        action = reaction_;
        once = true;
    } else if (mediaEnabled_ && !mediaKind_.isEmpty() && now >= mediaRestUntil_) {
        action = mediaKind_ == "video" ? "watch_tv" : "sing_mic";
    } else if (agent == "working" || agent == "speaking") {
        action = "working";
    } else if (agent == "waiting_user" || menuOpen_ || settingsOpen_) {
        action = "stand_arms";
    } else if (now < taskUntil_) {
        action = "working";
    } else if (!reaction_.isEmpty()) {
        action = reaction_;
        once = true;
    } else {
        action = now - lastInput_ >= idleHold ? lounge_ : QString("idle");
    }

    auto clip = config_.clips().constFind(action);
    if (clip == config_.clips().cend() || failedClips_.contains(action)) {
        action = "idle";
        clip = config_.clips().constFind(action);
        once = false;
        span = {};
    }
    if (clip == config_.clips().cend())
        return;
    const bool loop = span.enabled || (!once && clip->loop);
    const double rate = speed_ * clip->rate * (span.enabled || once ? 1 : directorRate());
    if (!restart && action == playing_ && span == playingLoop_ && once == playingOnce_) {
        if (rate != playingRate_) {
            player_.setSpeed(rate);
            playingRate_ = rate;
        }
        return;
    }
    const bool showPoster = playing_.isEmpty();
    playing_ = action;
    playingLoop_ = span;
    playingOnce_ = once;
    playingRate_ = rate;
    window_.setShadow(shadow_ && action != "grabbed");
    if (showPoster && !clip->poster.isEmpty()) {
        QImageReader image(clip->poster);
        image.setScaledSize((QSizeF(window_.size()) * window_.devicePixelRatio()).toSize());
        window_.present(image.read(), *clip);
    }
    player_.play(clip->path, loop, rate, span.enabled ? span.start : 0, span.enabled ? span.end : 0);
}

QString Controller::phase() const
{
    const auto config = director_.value("clock").toObject();
    auto time = QTime::currentTime();
    if (config.value("mode").toString() == "fixed") {
        const auto fixed = QTime::fromString(config.value("fixed").toString(), "H:mm");
        if (fixed.isValid())
            time = fixed;
    }
    time = time.addSecs(std::clamp(config.value("offsetMin").toInt(), -1440, 1440) * 60);
    auto phases = director_.value("phases").toArray();
    if (phases.isEmpty()) {
        const auto presets = director_.value("phasesPresets").toObject();
        phases = presets.value(director_.value("preset").toString("standard")).toArray();
        if (phases.isEmpty())
            phases = presets.value("standard").toArray();
    }
    for (const auto &value : phases) {
        const auto phase = value.toObject();
        const auto from = QTime::fromString(phase.value("from").toString(), "H:mm");
        const auto to = QTime::fromString(phase.value("to").toString(), "H:mm");
        if (!from.isValid() || !to.isValid())
            continue;
        const bool current = from < to ? time >= from && time < to : time >= from || time < to;
        if (current)
            return phase.value("id").toString();
    }
    return {};
}

double Controller::directorRate() const
{
    return director_.value("enabled").toBool(true) && director_.value("deepNightFreeze").toBool(true)
        && phase() == "late" && hardwareSample_.value("cpuUsage").toDouble(100) < 25
        ? std::clamp(director_.value("freezeRate").toDouble(0.35), 0.1, 1.0) : 1.0;
}

QString Controller::weighted(const QString &role, bool interaction) const
{
    const bool dynamic = director_.value("enabled").toBool(true);
    const auto weights = dynamic ? director_.value("weights").toObject().value(phase()).toObject() : QJsonObject{};
    QJsonObject cpuWeights;
    const auto cpu = director_.value("cpu").toObject();
    if (dynamic && cpu.value("enabled").toBool(true)) {
        const auto usage = hardwareSample_.value("cpuUsage").toDouble(-1);
        for (const auto &value : cpu.value("bands").toArray()) {
            const auto band = value.toObject();
            if (usage >= band.value("min").toDouble(0) && usage < band.value("max").toDouble(101)) {
                cpuWeights = band.value("weights").toObject();
                break;
            }
        }
    }
    QList<std::pair<QString, double>> candidates;
    double total = 0;
    const auto &clips = config_.clips();
    for (auto it = clips.cbegin(); it != clips.cend(); ++it) {
        if (failedClips_.contains(it.key()) || (!interaction && it->role != role))
            continue;
        const auto weight = (interaction ? it->reaction : it->weight)
            * std::clamp(weights.value(it.key()).toDouble(1), 0.0, 100.0)
            * std::clamp(cpuWeights.value(it.key()).toDouble(1), 0.0, 100.0);
        if (weight <= 0)
            continue;
        total += weight;
        candidates.append({it.key(), total});
    }
    if (candidates.isEmpty())
        return interaction ? QString("tail_open") : QString("idle");
    const auto pick = QRandomGenerator::global()->generateDouble() * total;
    for (const auto &[action, weight] : candidates) {
        if (pick < weight)
            return action;
    }
    return candidates.back().first;
}

void Controller::updateActivity()
{
    const auto now = clock_.elapsed();
    if (!reaction_.isEmpty() && reaction_ != playing_ && now >= reactionUntil_)
        reaction_.clear();
    if (now >= nextCpuSample_ && director_.value("enabled").toBool(true)) {
        hardwareSample_ = hardware_.sample(false);
        nextCpuSample_ = now + 5000;
    }
    if (now >= nextLounge_) {
        const auto tempo = director_.value("enabled").toBool(true)
            ? std::clamp(director_.value("tempo").toObject().value(phase()).toDouble(1), 0.1, 8.0) : 1.0;
        const auto min = std::clamp(director_.value("loungeMinMs").toInt(7000), 1000, 60000);
        const auto max = std::clamp(director_.value("loungeMaxMs").toInt(14000), min, 120000);
        const auto chance = std::clamp(director_.value("idleActChance").toDouble(0.35), 0.0, 1.0);
        lounge_ = weighted(QRandomGenerator::global()->generateDouble() < chance ? "idle_act" : "lounge", false);
        nextLounge_ = now + qRound((min + QRandomGenerator::global()->bounded(max - min + 1)) / tempo);
    }
    if (mediaEnabled_ && !mediaKind_.isEmpty()) {
        const QString action = mediaKind_ == "video" ? "watch_tv" : "sing_mic";
        if (now >= mediaRestUntil_ && playing_ == action && now >= nextMedia_) {
            const auto duration = config_.clips().value(action).duration;
            nextMedia_ = now + qRound(duration * 1000 / playingRate_);
            if (++mediaCycles_ > 3) {
                mediaCycles_ = 0;
                mediaRestUntil_ = now + 24000 + QRandomGenerator::global()->bounded(6000);
            }
        } else if (playing_ != action) {
            nextMedia_ = now;
        }
    }
    updatePlayback();
}

void Controller::touch()
{
    lastInput_ = clock_.elapsed();
    nextLounge_ = lastInput_ + idleHold;
    lounge_ = "idle";
}

void Controller::react(QString action, bool user)
{
    touch();
    if (action.isEmpty())
        action = weighted({}, true);
    if (!config_.clips().contains(action) || failedClips_.contains(action))
        return;
    const auto &clip = config_.clips().value(action);
    reactionUntil_ = clock_.elapsed() + qRound(clip.duration * 1000 / (speed_ * clip.rate));
    reaction_ = std::move(action);
    userReaction_ = user;
    updatePlayback(playing_ == reaction_);
}

void Controller::idle()
{
    touch();
    reaction_.clear();
    taskUntil_ = 0;
    if (activeLoop_.enabled) {
        auto loop = savedLoop_;
        loop.enabled = false;
        setClipLoop(loop, false);
    }
    updatePlayback();
}

void Controller::setPetSize(int size)
{
    size_ = std::clamp(size, 200, 620);
    window_.setPetSize(size_);
    savePetState();
}

void Controller::setSpeed(double speed)
{
    speed_ = std::clamp(speed, 0.5, 2.0);
    updatePlayback();
    savePetState();
}

void Controller::setShadow(bool enabled)
{
    shadow_ = enabled;
    window_.setShadow(shadow_ && playing_ != "grabbed");
    savePetState();
}

void Controller::setWorkspaceFollow(bool enabled)
{
    follow_ = enabled;
    niri_.setEnabled(follow_ && !isolated_);
    saveSettings();
}

void Controller::setMediaShow(bool enabled)
{
    mediaEnabled_ = enabled;
    saveSettings();
    if (!isolated_)
        media_.configure(settings_.value("media").toObject().value("show").toObject());
    updatePlayback();
}

void Controller::setDockEnabled(bool enabled)
{
    dockEnabled_ = enabled;
    dockConfig_.insert("enabled", enabled);
    if (!writeJson(config_.file("dock.json"), dockConfig_))
        reportSettingsError();
    if (!isolated_)
        dock_.configure(dockConfig_);
    updatePlayback();
}

void Controller::setAutostart(bool enabled)
{
    if (!isolated_ && !config_.setAutostart(enabled))
        reportSettingsError();
}

void Controller::saveSettings()
{
    settings_ = mergeJson(settings_, {
        {"media", QJsonObject{{"show", QJsonObject{{"enabled", mediaEnabled_}}}}},
        {"niri", QJsonObject{{"followWorkspace", follow_}}}
    });
    if (!writeJson(config_.file("settings.json"), settings_))
        reportSettingsError();
}

void Controller::savePetState()
{
    petState_ = mergeJson(petState_, {{"size", size_}, {"speed", speed_}, {"shadow", shadow_}});
    if (!writeJson(config_.file("pet-state.json"), petState_))
        reportSettingsError();
}

void Controller::reloadSkin()
{
    config_.reloadClips();
    watchSkinFiles();
    director_ = config_.director();
    failedClips_.clear();
    if (activeLoop_.enabled && !validLoop(activeLoop_, config_.clips()))
        activeLoop_.enabled = false;
    updatePlayback(true);
}

void Controller::watchSkinFiles()
{
    if (isolated_)
        return;
    const auto existing = skinFiles_.files() + skinFiles_.directories();
    const auto active = config_.skinPath();
    auto desired = QStringList{config_.file("skins"), config_.file("skins/active.json"),
        config_.file("director.json")};
    if (!active.isEmpty())
        desired.append({active, active + "/skin.json", active + "/director.json"});
    for (const auto &path : existing) {
        if (!desired.contains(path))
            skinFiles_.removePath(path);
    }
    for (const auto &path : desired) {
        if (QFileInfo::exists(path) && !existing.contains(path))
            skinFiles_.addPath(path);
    }
}

bool Controller::setClipLoop(const ClipLoop &loop, bool preview)
{
    if (loop.enabled && !validLoop(loop, config_.clips()))
        return false;
    if (!preview) {
        auto values = readJson(config_.file("animation-loop.json"));
        values = mergeJson(std::move(values), {{"enabled", loop.enabled}, {"actionId", loop.action},
            {"start", loop.start}, {"end", loop.end}});
        if (!writeJson(config_.file("animation-loop.json"), values))
            return false;
        savedLoop_ = loop;
    }
    reaction_.clear();
    previewing_ = preview;
    activeLoop_ = loop;
    updatePlayback(true);
    return true;
}

} // namespace catwhale
