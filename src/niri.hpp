#pragma once

#include <QLocalSocket>
#include <QObject>
#include <QTimer>

class QJsonObject;

namespace catwhale {

class NiriFollow final : public QObject {
public:
    explicit NiriFollow(QObject *parent = nullptr);
    ~NiriFollow() override;

    static bool available();
    void setEnabled(bool enabled);

private:
    void connectEvents();
    void reconnectEvents();
    void readEvents();
    void handleEvent(const QJsonObject &event);
    void updateFocus(qint64 workspace);
    bool updateWindow(const QJsonObject &window);
    void queueMove();
    void sendMove();
    void finishMove(bool succeeded);

    QLocalSocket events_;
    QLocalSocket moves_;
    QTimer reconnect_;
    QTimer moveDelay_;
    QTimer moveTimeout_;
    QByteArray moveRequest_;
    bool enabled_ = false;
    bool pendingMove_ = false;
    qint64 focusedWorkspace_ = -1;
    qint64 window_ = -1;
    qint64 windowWorkspace_ = -1;
    qint64 moveWindow_ = -1;
    qint64 moveWorkspace_ = -1;
    quint64 windowRevision_ = 0;
    quint64 moveRevision_ = 0;
};

} // namespace catwhale
