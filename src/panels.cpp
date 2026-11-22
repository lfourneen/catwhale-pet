#include "controller.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace catwhale {
namespace {

QDialog *makeDialog(PetWindow &window, const QString &title)
{
    auto *dialog = new QDialog;
    dialog->setWindowTitle(title);
    dialog->setWindowFlag(Qt::Tool);
    dialog->winId();
    dialog->windowHandle()->setTransientParent(&window);
    return dialog;
}

QLabel *plainLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

QString stateLabel(const QString &state)
{
    static const QMap<QString, QString> labels{
        {"working", QObject::tr("Working")}, {"speaking", QObject::tr("Speaking")},
        {"waiting_user", QObject::tr("Waiting for you")}, {"done", QObject::tr("Finished")},
        {"error", QObject::tr("Error")}, {"idle", QObject::tr("Idle")}, {"sleeping", QObject::tr("Resting")}
    };
    return labels.value(state, state);
}

QString metric(const QJsonValue &value, const QString &unit, int decimals = 0)
{
    return value.isDouble() ? QObject::tr("%1 %2").arg(QLocale().toString(value.toDouble(), 'f', decimals), unit)
                            : QStringLiteral("—");
}

} // namespace

void Controller::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    tray_ = new QSystemTrayIcon(QIcon(config_.assets() + "/tray.png"), this);
    tray_->setToolTip(tr("Catwhale Pet"));
    trayMenu_ = buildMenu(true);
    tray_->setContextMenu(trayMenu_);
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger)
            window_.show();
        else if (reason == QSystemTrayIcon::DoubleClick)
            showMenu();
    });
    tray_->show();
}

QMenu *Controller::buildMenu(bool trayMenu)
{
    auto *menu = new QMenu;
    if (trayMenu) {
        menu->addAction(tr("Show Catwhale"), &window_, &QWindow::show);
        menu->addAction(tr("Hide Catwhale"), &window_, &QWindow::hide);
        menu->addSeparator();
    }

    auto *interaction = menu->addMenu(tr("Interact"));
    interaction->addAction(tr("Say hello"), this, [this] { react("tail_open"); });
    interaction->addAction(tr("Wag tail"), this, [this] { react("tail_closed"); });
    interaction->addAction(tr("Random action"), this, [this] { react(); });
    interaction->addAction(tr("Eat some rice"), this, [this] { react("eat"); });
    interaction->addAction(tr("Keep it busy for 10 seconds"), this, [this] {
        touch();
        taskUntil_ = clock_.elapsed() + 10000;
        updatePlayback();
    });
    interaction->addAction(tr("Back to idle"), this, &Controller::idle);

    auto *appearance = menu->addMenu(tr("Appearance"));
    auto *sizes = appearance->addMenu(tr("Pet size"));
    for (const int height : {240, 340, 460, 620})
        sizes->addAction(tr("%1 px").arg(height), this, [this, height] { setPetSize(height); });
    appearance->addAction(tr("Open skin folder"), this, &Controller::showSkinFolder);
    appearance->addAction(tr("Reload skin"), this, &Controller::reloadSkin);

    auto *media = menu->addAction(tr("React to media"));
    media->setCheckable(true);
    connect(menu, &QMenu::aboutToShow, media, [this, media] { media->setChecked(mediaEnabled_); });
    connect(media, &QAction::triggered, this, &Controller::setMediaShow);
    menu->addAction(tr("Agent dock…"), this, &Controller::showAgentDock);
    menu->addAction(tr("Hardware status…"), this, &Controller::showHardware);
    menu->addAction(tr("Settings…"), this, &Controller::showSettings);
    menu->addSeparator();
    menu->addAction(tr("Quit Catwhale"), qApp, &QApplication::quit);
    return menu;
}

void Controller::showMenu(QPoint position)
{
    if (!window_.isVisible() || menuOpen_)
        return;
    if (!menu_) {
        menu_ = buildMenu(false);
        menu_->winId();
        menu_->windowHandle()->setTransientParent(&window_);
        connect(menu_, &QMenu::aboutToHide, this, [this] {
            menuOpen_ = false;
            updatePlayback();
        });
    }
    menuOpen_ = true;
    touch();
    updatePlayback();
    if (position.isNull())
        position = window_.mapToGlobal(QPoint(window_.width() / 2, window_.height() / 2));
    menu_->popup(position);
}

void Controller::closeMenu()
{
    if (menu_)
        menu_->hide();
}

void Controller::showSettings()
{
    closeMenu();
    if (settingsDialog_) {
        settingsDialog_->raise();
        settingsDialog_->activateWindow();
        return;
    }

    auto *dialog = makeDialog(window_, tr("Catwhale Pet Settings"));
    settingsDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumWidth(430);
    auto *body = new QVBoxLayout(dialog);

    auto *appearance = new QGroupBox(tr("Pet"), dialog);
    auto *petForm = new QFormLayout(appearance);
    auto *size = new QSpinBox(appearance);
    size->setRange(200, 620);
    size->setSuffix(tr(" px"));
    size->setValue(size_);
    auto *speed = new QDoubleSpinBox(appearance);
    speed->setRange(0.5, 2);
    speed->setSingleStep(0.1);
    speed->setSuffix(tr("×"));
    speed->setValue(speed_);
    auto *shadow = new QCheckBox(tr("Ground shadow"), appearance);
    shadow->setChecked(shadow_);
    petForm->addRow(tr("Size"), size);
    petForm->addRow(tr("Animation speed"), speed);
    petForm->addRow(shadow);
    body->addWidget(appearance);
    connect(size, &QSpinBox::valueChanged, this, &Controller::setPetSize);
    connect(speed, &QDoubleSpinBox::valueChanged, this, &Controller::setSpeed);
    connect(shadow, &QCheckBox::toggled, this, &Controller::setShadow);

    auto *behavior = new QGroupBox(tr("Behavior"), dialog);
    auto *behaviorForm = new QVBoxLayout(behavior);
    auto *media = new QCheckBox(tr("React to media playback"), behavior);
    auto *probe = new QPushButton(tr("Check media playback now"), behavior);
    mediaMessage_ = plainLabel(behavior);
    auto *dock = new QCheckBox(tr("Monitor local agents"), behavior);
    auto *follow = new QCheckBox(tr("Follow the active workspace (niri)"), behavior);
    auto *autostart = new QCheckBox(tr("Start with the session"), behavior);
    media->setChecked(mediaEnabled_);
    dock->setChecked(dockEnabled_);
    follow->setChecked(follow_);
    follow->setEnabled(NiriFollow::available());
    autostart->setChecked(config_.autostart());
    behaviorForm->addWidget(media);
    behaviorForm->addWidget(probe);
    behaviorForm->addWidget(mediaMessage_);
    behaviorForm->addWidget(dock);
    behaviorForm->addWidget(follow);
    behaviorForm->addWidget(autostart);
    body->addWidget(behavior);
    connect(media, &QCheckBox::toggled, this, &Controller::setMediaShow);
    connect(probe, &QPushButton::clicked, &media_, &MediaMonitor::probeOnce);
    connect(dock, &QCheckBox::toggled, this, &Controller::setDockEnabled);
    connect(follow, &QCheckBox::toggled, this, &Controller::setWorkspaceFollow);
    connect(autostart, &QCheckBox::toggled, this, &Controller::setAutostart);
    media_.onProbed = [this](const MediaStatus &status) {
        if (mediaMessage_) {
            mediaMessage_->setText(!status.error.isEmpty() ? status.error : status.playing()
                ? tr("Playing %1 via %2").arg(status.title, status.app) : tr("No active MPRIS media"));
        }
    };

    auto *clips = new QGroupBox(tr("Animation clip loop"), dialog);
    auto *clipForm = new QFormLayout(clips);
    auto *action = new QComboBox(clips);
    for (auto it = config_.clips().cbegin(); it != config_.clips().cend(); ++it)
        action->addItem(it->name, it.key());
    action->setCurrentIndex(action->findData(savedLoop_.action));
    auto *start = new QDoubleSpinBox(clips);
    auto *end = new QDoubleSpinBox(clips);
    for (auto *time : {start, end}) {
        time->setDecimals(2);
        time->setSuffix(tr(" s"));
        time->setSingleStep(0.05);
    }
    auto updateRange = [this, action, start, end] {
        const auto duration = config_.clips().value(action->currentData().toString()).duration;
        start->setMaximum(duration);
        end->setMaximum(duration);
    };
    connect(action, &QComboBox::currentIndexChanged, this, [updateRange, end] {
        updateRange();
        end->setValue(end->maximum());
    });
    updateRange();
    start->setValue(savedLoop_.start);
    end->setValue(savedLoop_.end);
    clipForm->addRow(tr("Action"), action);
    clipForm->addRow(tr("Start"), start);
    clipForm->addRow(tr("End"), end);

    auto *loopButtons = new QDialogButtonBox(clips);
    auto *preview = loopButtons->addButton(tr("Preview"), QDialogButtonBox::ActionRole);
    auto *apply = loopButtons->addButton(tr("Save loop"), QDialogButtonBox::ActionRole);
    auto *stop = loopButtons->addButton(tr("Stop loop"), QDialogButtonBox::ActionRole);
    clipForm->addRow(loopButtons);
    body->addWidget(clips);

    settingsMessage_ = plainLabel(dialog);
    settingsMessage_->setText(savedLoop_.enabled ? tr("Clip loop enabled") : tr("Clip loop off"));
    body->addWidget(settingsMessage_);
    const auto updateClip = [this, action, start, end](bool enabled, bool temporary) {
        const ClipLoop loop{enabled, action->currentData().toString(), start->value(), end->value()};
        if (setClipLoop(loop, temporary)) {
            settingsMessage_->setText(temporary ? tr("Previewing the selected range")
                : enabled ? tr("Clip loop saved") : tr("Clip loop stopped"));
        } else {
            settingsMessage_->setText(tr("Check the clip range and configuration directory permissions."));
        }
    };
    connect(preview, &QPushButton::clicked, dialog, [updateClip] { updateClip(true, true); });
    connect(apply, &QPushButton::clicked, dialog, [updateClip] { updateClip(true, false); });
    connect(stop, &QPushButton::clicked, dialog, [updateClip] { updateClip(false, false); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto *skin = buttons->addButton(tr("Open skin folder"), QDialogButtonBox::ActionRole);
    connect(skin, &QPushButton::clicked, this, &Controller::showSkinFolder);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    body->addWidget(buttons);
    auto *about = plainLabel(dialog);
    about->setText(tr("Catwhale Pet %1 · %2").arg(QStringLiteral(CATWHALE_VERSION),
        config_.skinName().isEmpty() ? tr("Built-in skin") : config_.skinName()));
    body->addWidget(about);
    connect(dialog, &QDialog::finished, this, [this] {
        settingsOpen_ = false;
        settingsDialog_ = nullptr;
        settingsMessage_ = mediaMessage_ = nullptr;
        media_.onProbed = {};
        if (previewing_) {
            activeLoop_ = savedLoop_;
            previewing_ = false;
        }
        updatePlayback();
    });

    settingsOpen_ = true;
    touch();
    updatePlayback();
    dialog->show();
}

void Controller::showAgentDock()
{
    closeMenu();
    if (dockDialog_) {
        dockDialog_->raise();
        dockDialog_->activateWindow();
        return;
    }
    auto *dialog = makeDialog(window_, tr("Catwhale Agent Dock"));
    dockDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(560, 300);
    auto *body = new QVBoxLayout(dialog);
    auto *mute = new QCheckBox(tr("Mute pet reactions"), dialog);
    mute->setChecked(dockMuted_);
    connect(mute, &QCheckBox::toggled, dialog, [this](bool enabled) {
        dockMuted_ = enabled;
        dockConfig_.insert("muted", dockMuted_);
        if (!writeJson(config_.file("dock.json"), dockConfig_))
            reportSettingsError();
        updatePlayback();
    });
    body->addWidget(mute);
    dockList_ = new QTreeWidget(dialog);
    dockList_->setHeaderLabels({tr("Agent"), tr("State"), tr("Session")});
    dockList_->setRootIsDecorated(false);
    body->addWidget(dockList_);
    dockDetails_ = plainLabel(dialog);
    dockDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(dockDetails_);
    connect(dockList_, &QTreeWidget::currentItemChanged, dialog, [this](QTreeWidgetItem *current) {
        if (current)
            dockDetails_->setText(current->data(0, Qt::UserRole + 1).toString());
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto *clear = buttons->addButton(tr("Clear ended sessions"), QDialogButtonBox::ActionRole);
    connect(clear, &QPushButton::clicked, &dock_, &AgentDock::clearEnded);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    body->addWidget(buttons);
    connect(dialog, &QDialog::finished, this, [this] {
        dockDialog_ = nullptr;
        dockList_ = nullptr;
        dockDetails_ = nullptr;
    });
    updateAgentDock();
    dialog->show();
}

void Controller::updateAgentDock()
{
    if (!dockList_)
        return;
    const QString selected = dockList_->currentItem()
        ? dockList_->currentItem()->data(0, Qt::UserRole).toString() : QString{};
    dockList_->clear();
    for (const auto &value : dock_.sessions()) {
        const auto session = value.toObject();
        const auto name = session.value("agentName").toString(session.value("agent").toString());
        auto *row = new QTreeWidgetItem(dockList_, {name, stateLabel(session.value("state").toString()),
            session.value("title").toString()});
        const auto key = session.value("key").toString();
        row->setData(0, Qt::UserRole, key);
        QStringList detail;
        for (const auto &[label, field] : {std::pair{tr("Tool"), QString("tool")},
            {tr("Folder"), QString("cwd")}, {tr("Last user message"), QString("lastUser")},
            {tr("Last agent message"), QString("lastAgent")}}) {
            const auto fieldValue = session.value(field).toString();
            if (!fieldValue.isEmpty())
                detail.append(tr("%1: %2").arg(label, fieldValue));
        }
        row->setData(0, Qt::UserRole + 1, detail.join(u'\n'));
        if (key == selected)
            dockList_->setCurrentItem(row);
    }
    if (dockList_->topLevelItemCount() == 0)
        dockDetails_->setText(dock_.error().isEmpty() ? tr("No active sessions") : dock_.error());
    else if (!dockList_->currentItem())
        dockList_->setCurrentItem(dockList_->topLevelItem(0));
}

void Controller::showHardware()
{
    closeMenu();
    if (hardwareDialog_) {
        hardwareDialog_->raise();
        hardwareDialog_->activateWindow();
        return;
    }
    auto *dialog = makeDialog(window_, tr("Catwhale Hardware Status"));
    hardwareDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumWidth(340);
    auto *body = new QVBoxLayout(dialog);
    hardwareMessage_ = plainLabel(dialog);
    body->addWidget(hardwareMessage_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    body->addWidget(buttons);
    connect(dialog, &QDialog::finished, this, [this] {
        hardwareTimer_.stop();
        hardwareMessage_ = nullptr;
        hardwareDialog_ = nullptr;
    });
    updateHardware();
    hardwareTimer_.start();
    dialog->show();
}

void Controller::updateHardware()
{
    if (!hardwareMessage_)
        return;
    const auto sample = hardware_.sample();
    const auto gpu = sample.value("gpu").toObject();
    QStringList summary{
        tr("CPU: %1 · %2").arg(metric(sample.value("cpuUsage"), "%"), metric(sample.value("cpuFreqMHz"), "MHz")),
        tr("GPU: %1 · %2 · %3").arg(gpu.value("name").toString(tr("Unknown")),
            metric(gpu.value("util"), "%"), metric(gpu.value("tempC"), "°C")),
        tr("Memory: %1 / %2").arg(metric(sample.value("memUsedGB"), "GB", 1),
            metric(sample.value("memTotalGB"), "GB", 1))
    };
    for (const auto &disk : sample.value("disks").toArray()) {
        const auto entry = disk.toObject();
        summary.append(tr("Disk %1: %2").arg(entry.value("drive").toString(), metric(entry.value("usedPct"), "%")));
    }
    hardwareMessage_->setText(summary.join(u'\n'));
}

void Controller::showSkinFolder()
{
    auto path = config_.skinPath();
    if (path.isEmpty())
        path = config_.file("skins");
    if (!QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        QMessageBox::warning(settingsDialog_, tr("Skin folder"), tr("Could not open the skin folder."));
    watchSkinFiles();
}

void Controller::reportSettingsError()
{
    if (settingsMessage_)
        settingsMessage_->setText(tr("Could not save the setting."));
    else
        qWarning("Could not save the setting.");
}

} // namespace catwhale
