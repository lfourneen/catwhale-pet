#include "niri.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace catwhale {
namespace {

constexpr qsizetype maxEventSize = 16 * 1024 * 1024;
constexpr qsizetype maxReplySize = 4096;

QString socketPath()
{
    return QFile::decodeName(qgetenv("NIRI_SOCKET"));
}

qint64 id(const QJsonObject &object, QLatin1StringView key)
{
    return object.value(key).toInteger(-1);
}

} // namespace

NiriFollow::NiriFollow(QObject *parent)
    : QObject(parent)
{
    events_.setReadBufferSize(maxEventSize);
    moves_.setReadBufferSize(maxReplySize);
    reconnect_.setInterval(4000);
    reconnect_.setSingleShot(true);
    moveDelay_.setInterval(40);
    moveDelay_.setSingleShot(true);
    moveTimeout_.setInterval(1500);
    moveTimeout_.setSingleShot(true);

    connect(&events_, &QLocalSocket::connected, this, [this] {
        events_.write("\"EventStream\"\n");
    });
    connect(&events_, &QLocalSocket::disconnected, this, &NiriFollow::reconnectEvents);
    connect(&events_, &QLocalSocket::errorOccurred, this, &NiriFollow::reconnectEvents);
    connect(&events_, &QLocalSocket::readyRead, this, &NiriFollow::readEvents);
    connect(&reconnect_, &QTimer::timeout, this, &NiriFollow::connectEvents);
    connect(&moveDelay_, &QTimer::timeout, this, &NiriFollow::sendMove);
    connect(&moveTimeout_, &QTimer::timeout, this, [this] { finishMove(false); });

    connect(&moves_, &QLocalSocket::connected, this, [this] {
        moves_.write(moveRequest_);
    });
    connect(&moves_, &QLocalSocket::readyRead, this, [this] {
        if (!moves_.canReadLine())
            return;
        const auto reply = QJsonDocument::fromJson(moves_.readLine()).object();
        finishMove(reply.contains(QLatin1StringView("Ok")));
    });
    connect(&moves_, &QLocalSocket::disconnected, this, [this] { finishMove(false); });
    connect(&moves_, &QLocalSocket::errorOccurred, this, [this] { finishMove(false); });
}

NiriFollow::~NiriFollow()
{
    setEnabled(false);
}

bool NiriFollow::available()
{
    return !socketPath().isEmpty();
}

void NiriFollow::setEnabled(bool enabled)
{
    enabled = enabled && available();
    if (enabled_ == enabled)
        return;
    enabled_ = enabled;
    if (enabled_) {
        connectEvents();
        return;
    }

    reconnect_.stop();
    events_.abort();
    reconnectEvents();
}

void NiriFollow::connectEvents()
{
    if (enabled_ && events_.state() == QLocalSocket::UnconnectedState)
        events_.connectToServer(socketPath());
}

void NiriFollow::reconnectEvents()
{
    focusedWorkspace_ = -1;
    window_ = -1;
    windowWorkspace_ = -1;
    pendingMove_ = false;
    moveDelay_.stop();
    finishMove(false);
    if (enabled_ && !reconnect_.isActive())
        reconnect_.start();
}

void NiriFollow::readEvents()
{
    while (events_.canReadLine()) {
        const auto event = QJsonDocument::fromJson(events_.readLine()).object();
        if (event.contains(QLatin1StringView("Err"))) {
            events_.abort();
            return;
        }
        handleEvent(event);
    }

    if (events_.bytesAvailable() >= maxEventSize)
        events_.abort();
}

void NiriFollow::handleEvent(const QJsonObject &event)
{
    using Qt::StringLiterals::operator""_L1;
    if (event.contains("WorkspacesChanged"_L1)) {
        const auto workspaces = event["WorkspacesChanged"_L1].toObject()["workspaces"_L1].toArray();
        for (const auto &value : workspaces) {
            const auto workspace = value.toObject();
            if (workspace["is_focused"_L1].toBool()) {
                updateFocus(id(workspace, "id"_L1));
                return;
            }
        }
        updateFocus(-1);
    } else if (event.contains("WorkspaceActivated"_L1)) {
        const auto workspace = event["WorkspaceActivated"_L1].toObject();
        if (workspace["focused"_L1].toBool())
            updateFocus(id(workspace, "id"_L1));
    } else if (event.contains("WindowsChanged"_L1)) {
        const auto windows = event["WindowsChanged"_L1].toObject()["windows"_L1].toArray();
        for (const auto &value : windows) {
            if (updateWindow(value.toObject()))
                return;
        }
        window_ = -1;
        windowWorkspace_ = -1;
    } else if (event.contains("WindowOpenedOrChanged"_L1)) {
        updateWindow(event["WindowOpenedOrChanged"_L1].toObject()["window"_L1].toObject());
    } else if (event.contains("WindowClosed"_L1)) {
        if (id(event["WindowClosed"_L1].toObject(), "id"_L1) == window_) {
            window_ = -1;
            windowWorkspace_ = -1;
        }
    }
}

void NiriFollow::updateFocus(qint64 workspace)
{
    if (focusedWorkspace_ == workspace)
        return;
    focusedWorkspace_ = workspace;
    pendingMove_ = workspace >= 0;
    queueMove();
}

bool NiriFollow::updateWindow(const QJsonObject &window)
{
    using Qt::StringLiterals::operator""_L1;
    const auto windowId = id(window, "id"_L1);
    if (windowId < 0 || window["app_id"_L1].toString() != "catwhale-pet"_L1
        || window["title"_L1].toString() != "Catwhale Pet"_L1)
        return false;

    const auto pid = id(window, "pid"_L1);
    if (pid >= 0 && pid != QCoreApplication::applicationPid())
        return false;

    pendingMove_ = pendingMove_ || window_ != windowId;
    window_ = windowId;
    windowWorkspace_ = id(window, "workspace_id"_L1);
    ++windowRevision_;
    queueMove();
    return true;
}

void NiriFollow::queueMove()
{
    if (enabled_ && pendingMove_ && focusedWorkspace_ >= 0 && window_ >= 0
        && windowWorkspace_ >= 0 && moveWindow_ < 0)
        moveDelay_.start();
}

void NiriFollow::sendMove()
{
    if (!enabled_ || !pendingMove_ || focusedWorkspace_ < 0 || window_ < 0
        || windowWorkspace_ < 0 || moveWindow_ >= 0)
        return;

    pendingMove_ = false;
    if (windowWorkspace_ == focusedWorkspace_)
        return;

    moveWindow_ = window_;
    moveWorkspace_ = focusedWorkspace_;
    moveRevision_ = windowRevision_;
    const QJsonObject action{
        {QStringLiteral("window_id"), moveWindow_},
        {QStringLiteral("reference"), QJsonObject{{QStringLiteral("Id"), moveWorkspace_}}},
        {QStringLiteral("focus"), false},
    };
    const QJsonObject request{
        {QStringLiteral("Action"), QJsonObject{{QStringLiteral("MoveWindowToWorkspace"), action}}},
    };
    moveRequest_ = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    moveTimeout_.start();
    moves_.connectToServer(socketPath());
}

void NiriFollow::finishMove(bool succeeded)
{
    if (moveWindow_ < 0)
        return;

    // A newer window event may have arrived before the reply on the action socket.
    if (succeeded && window_ == moveWindow_ && windowRevision_ == moveRevision_)
        windowWorkspace_ = moveWorkspace_;
    moveWindow_ = -1;
    moveTimeout_.stop();
    moves_.abort();
    queueMove();
}

} // namespace catwhale
