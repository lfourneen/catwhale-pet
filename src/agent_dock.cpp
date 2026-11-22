#include "services.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <utility>

namespace catwhale {
namespace {

using namespace Qt::StringLiterals;

constexpr qsizetype maxRequestHead = 8 * 1024;
constexpr qsizetype maxRequestBody = 64 * 1024;
constexpr qsizetype maxConnections = 32;
constexpr qsizetype maxSessions = 128;
constexpr qsizetype logTailBytes = 96 * 1024;
constexpr qint64 logActiveMs = 10'000;
constexpr qint64 logLiveMs = 120'000;
constexpr qint64 discoveryInterval = 10'000;
constexpr qsizetype maxLogDirectories = 192;
constexpr qsizetype maxRecentLogs = 3;

QString clipped(QString text, qsizetype length)
{
    text = text.simplified();
    return text.size() > length ? text.left(length - 1) + u'…' : text;
}

QString firstText(const QJsonObject &object, std::initializer_list<QLatin1StringView> keys,
                  QString fallback = {})
{
    for (const auto key : keys) {
        const auto value = object.value(key);
        if (value.isString() && !value.toString().isEmpty()) {
            return value.toString();
        }
    }
    return fallback;
}

struct Agent {
    QString id;
    QString name;
    QString kind;
    QString accent;
};

const auto agents = std::to_array<Agent>({
    {u"hermes"_s, u"Hermes"_s, u"push"_s, u"#5b9dff"_s},
    {u"codex"_s, u"ChatGPT Codex"_s, u"log"_s, u"#10a37f"_s},
    {u"claude-code"_s, u"Claude Code"_s, u"log"_s, u"#d97757"_s},
    {u"openclaw"_s, u"OpenClaw"_s, u"push"_s, u"#8b5cf6"_s},
    {u"mimocode"_s, u"MiMoCode"_s, u"cli"_s, u"#f59e0b"_s},
    {u"workbuddy"_s, u"WorkBuddy"_s, u"push"_s, u"#0ea5e9"_s},
    {u"generic"_s, u"Other Agent"_s, u"push"_s, u"#94a3b8"_s},
});

Agent agentInfo(const QString &id)
{
    const auto entry = std::ranges::find(agents, id, &Agent::id);
    return entry == agents.end() ? Agent{id, id, u"push"_s, agents.back().accent} : *entry;
}

constexpr auto idle = "idle"_L1;
constexpr auto working = "working"_L1;
constexpr auto speaking = "speaking"_L1;
constexpr auto waiting = "waiting_user"_L1;
constexpr auto done = "done"_L1;
constexpr auto errorState = "error"_L1;
constexpr auto sleeping = "sleeping"_L1;

QString normalizeAgent(QString value)
{
    static const QHash<QString, QString> aliases = {
        {u"claude"_s, u"claude-code"_s}, {u"claudecode"_s, u"claude-code"_s},
        {u"claude_code"_s, u"claude-code"_s}, {u"openai-codex"_s, u"codex"_s},
        {u"chatgpt"_s, u"codex"_s}, {u"codex-cli"_s, u"codex"_s},
        {u"clawd"_s, u"openclaw"_s}, {u"open-claw"_s, u"openclaw"_s},
        {u"mimo"_s, u"mimocode"_s}, {u"mimo-code"_s, u"mimocode"_s},
    };
    static const QRegularExpression valid(u"^[a-z0-9._-]{1,32}$"_s);
    value = value.trimmed().toLower();
    return valid.match(value).hasMatch() ? aliases.value(value, value) : u"generic"_s;
}

int rank(QStringView state)
{
    if (state == waiting) return 6;
    if (state == errorState) return 5;
    if (state == working) return 4;
    if (state == speaking) return 3;
    if (state == done) return 2;
    if (state == idle) return 1;
    if (state == sleeping) return 0;
    return -1;
}

bool ended(QStringView state)
{
    return state == done || state == errorState || state == sleeping;
}

QString normalizeState(QString state)
{
    static const QRegularExpression spacesOrHyphens(u"[\\s-]+"_s);
    state = state.trimmed().toLower().replace(spacesOrHyphens, u"_"_s);
    if (rank(state) >= 0) {
        return state;
    }
    static const QHash<QString, QLatin1StringView> aliases = {
        {u"attention"_s, done}, {u"finished"_s, done}, {u"failed"_s, errorState},
        {u"juggling"_s, working}, {u"sweeping"_s, working}, {u"carrying"_s, working},
        {u"busy"_s, working}, {u"tool_running"_s, working}, {u"thinking"_s, working},
        {u"typing"_s, speaking}, {u"talking"_s, speaking}, {u"notification"_s, waiting},
        {u"ready"_s, idle}, {u"offline"_s, sleeping},
    };
    return QString(aliases.value(state));
}

QString eventState(QString event)
{
    static const QRegularExpression separators(u"[\\s_-]+"_s);
    event = event.toLower().remove(separators);
    static const QHash<QString, QLatin1StringView> states = {
        {u"sessionstart"_s, idle}, {u"onsessionstart"_s, idle}, {u"onsessionreset"_s, idle},
        {u"sessionend"_s, sleeping}, {u"onsessionend"_s, done}, {u"onsessionfinalize"_s, done},
        {u"agentloopstopped"_s, done}, {u"stop"_s, done}, {u"agentstop"_s, done},
        {u"preapprovalrequest"_s, waiting}, {u"onhumaninputrequest"_s, waiting},
        {u"permissionrequest"_s, waiting}, {u"notification"_s, waiting}, {u"elicitation"_s, waiting},
        {u"onstreamstart"_s, speaking}, {u"onstreamdelta"_s, speaking},
        {u"assistantmessage"_s, speaking}, {u"assistantoutput"_s, speaking},
        {u"apierror"_s, errorState}, {u"apirequesterror"_s, errorState},
        {u"posttoolusefailure"_s, errorState}, {u"stopfailure"_s, errorState}, {u"erroroccurred"_s, errorState},
        {u"prellmcall"_s, working}, {u"postllmcall"_s, working},
        {u"preapirequest"_s, working}, {u"postapirequest"_s, working},
        {u"onstreamend"_s, working}, {u"pretoolcall"_s, working}, {u"posttoolcall"_s, working},
        {u"postapprovalresponse"_s, working}, {u"onhumaninputresolved"_s, working},
        {u"subagentstart"_s, working}, {u"subagentstop"_s, working},
        {u"userpromptsubmit"_s, working}, {u"userpromptsubmitted"_s, working},
        {u"pretooluse"_s, working}, {u"posttooluse"_s, working},
        {u"precompact"_s, working}, {u"postcompact"_s, working},
    };
    return QString(states.value(event));
}

QJsonObject makeSession(const QString &id, const QString &sessionId, qint64 timestamp)
{
    const auto agent = agentInfo(id);
    return {
        {u"key"_s, id + u"::"_s + sessionId}, {u"agent"_s, id}, {u"agentName"_s, agent.name},
        {u"accent"_s, agent.accent}, {u"sessionId"_s, sessionId}, {u"updatedAt"_s, timestamp},
    };
}

QString recordText(const QJsonValue &content)
{
    if (content.isString()) {
        return content.toString();
    }
    QStringList pieces;
    for (const auto &part : content.toArray()) {
        pieces << firstText(part.toObject(), {"text"_L1, "input_text"_L1, "output_text"_L1});
    }
    return pieces.join(u' ');
}

struct LogSummary {
    QString user;
    QString assistant;
    QString tool;
    bool failed = false;
};

LogSummary summarizeLog(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(std::max<qint64>(0, file.size() - logTailBytes))) {
        return {};
    }
    LogSummary result;
    for (const auto &line : file.read(logTailBytes).split('\n')) {
        if (line.isEmpty() || line.front() != '{') {
            continue;
        }
        const auto record = QJsonDocument::fromJson(line).object();
        const auto type = record.value("type"_L1).toString();
        const auto payload = record.value("payload"_L1).toObject();
        const auto message = record.value("message"_L1).toObject();
        const auto role = firstText(message, {"role"_L1},
            firstText(payload, {"role"_L1}, record.value("role"_L1).toString()));
        const auto subtype = payload.value("type"_L1).toString();
        auto content = message.value("content"_L1);
        if (content.isUndefined()) content = payload.value("content"_L1);
        if (content.isUndefined()) content = record.value("content"_L1);
        const auto parsedContent = recordText(content);
        const auto text = parsedContent.isEmpty() ? payload.value("message"_L1).toString() : parsedContent;
        const auto tool = firstText(payload, {"name"_L1}, record.value("tool_name"_L1).toString());
        if (role == "user"_L1 || (type == "event_msg"_L1 && subtype == "user_message"_L1)) {
            if (!text.isEmpty()) result.user = clipped(text, 300);
        } else if (role == "assistant"_L1 || role == "agent"_L1
                   || (type == "event_msg"_L1 && subtype == "agent_message"_L1)) {
            if (!text.isEmpty()) result.assistant = clipped(text, 400);
        }
        if (!tool.isEmpty()) {
            result.tool = clipped(tool, 40);
        }
        const auto recordError = record.value("error"_L1);
        result.failed |= type == errorState || recordError.isObject() || recordError.toBool()
            || !recordError.toString().isEmpty()
            || !payload.value("ok"_L1).toBool(true);
    }
    return result;
}

struct LogFile {
    QString path;
    QString agent;
    QString key;
    qint64 updated = 0;
    qint64 size = -1;
    LogSummary summary;
};

QList<LogFile> findRecentLogs(const QString &root, const QString &agent, qint64 now)
{
    QList<LogFile> files;
    QList<std::pair<QString, int>> pending = {{root, 0}};
    qsizetype visited = 0;
    while (!pending.isEmpty() && visited < maxLogDirectories) {
        const auto [directory, depth] = pending.takeLast();
        ++visited;
        const QDir dir(directory);
        const auto entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                               QDir::Time | QDir::Reversed);
        for (const auto &entry : entries) {
            if (entry.isDir()) {
                if (depth < 6) {
                    pending.append({entry.filePath(), depth + 1});
                }
                continue;
            }
            if (entry.suffix() != "jsonl"_L1) {
                continue;
            }
            const auto timestamp = entry.lastModified().toMSecsSinceEpoch();
            if (now - timestamp <= logLiveMs) {
                const auto key = agent + u"::"_s + entry.completeBaseName();
                files.append({entry.filePath(), agent, key, timestamp, -1, {}});
            }
        }
    }
    std::ranges::sort(files, std::greater<>(), &LogFile::updated);
    if (files.size() > maxRecentLogs) {
        files.resize(maxRecentLogs);
    }
    return files;
}

struct DockRequest {
    QByteArray data;
    QByteArray method;
    QByteArray path;
    QString agent;
    qsizetype bodyOffset = -1;
    qsizetype bodyLength = 0;
    bool expectContinue = false;
};

void sendJson(QTcpSocket &socket, QByteArrayView status, const QJsonObject &object)
{
    const auto body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    socket.write("HTTP/1.1 " + status.toByteArray()
                 + "\r\nContent-Type: application/json; charset=utf-8"
                   "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket.disconnectFromHost();
}

void sendError(QTcpSocket &socket, QByteArrayView status, QString reason)
{
    sendJson(socket, status, {{u"ok"_s, false}, {u"error"_s, std::move(reason)}});
}

bool localHost(const QByteArray &host, quint16 port)
{
    const auto suffix = ":" + QByteArray::number(port);
    return host == "localhost" + suffix || host == "127.0.0.1" + suffix
        || (port == 80 && (host == "localhost" || host == "127.0.0.1"));
}

struct HttpError {
    QByteArrayView status;
    QString reason;
};

std::optional<HttpError> parseRequestHeader(DockRequest &request, quint16 port)
{
    const auto headerEnd = request.data.indexOf("\r\n\r\n");
    if (headerEnd > maxRequestHead || (headerEnd < 0 && request.data.size() > maxRequestHead)) {
        return HttpError{"431 Request Header Fields Too Large", u"headers too large"_s};
    }
    if (headerEnd < 0) {
        return {};
    }

    auto lines = request.data.first(headerEnd).split('\n');
    const auto start = lines.takeFirst().trimmed().split(' ');
    if (start.size() != 3 || (start[2] != "HTTP/1.1" && start[2] != "HTTP/1.0")) {
        return HttpError{"400 Bad Request", u"invalid request line"_s};
    }
    QHash<QByteArray, QByteArray> headers;
    for (const auto &line : lines) {
        const auto colon = line.indexOf(':');
        if (colon <= 0) {
            return HttpError{"400 Bad Request", u"invalid header"_s};
        }
        const auto name = line.first(colon).trimmed().toLower();
        if (headers.contains(name)) {
            return HttpError{"400 Bad Request", u"duplicate header"_s};
        }
        headers.insert(name, line.sliced(colon + 1).trimmed());
    }

    const auto host = headers.value("host").toLower();
    if (headers.contains("origin") || headers.contains("sec-fetch-site")
        || ((!host.isEmpty() || start[2] == "HTTP/1.1") && !localHost(host, port))) {
        return HttpError{"403 Forbidden", u"local clients only"_s};
    }
    if (headers.contains("transfer-encoding")) {
        return HttpError{"400 Bad Request", u"Content-Length is required for request bodies"_s};
    }

    bool validLength = false;
    const auto length = headers.value("content-length", "0").toLongLong(&validLength);
    if (!validLength || length < 0) {
        return HttpError{"400 Bad Request", u"invalid content length"_s};
    }
    if (length > maxRequestBody) {
        return HttpError{"413 Content Too Large", u"body too large"_s};
    }

    const auto expectation = headers.value("expect").toLower();
    if (!expectation.isEmpty() && expectation != "100-continue") {
        return HttpError{"417 Expectation Failed", u"unsupported expectation"_s};
    }

    const auto query = start[1].indexOf('?');
    request.method = start[0];
    request.path = query < 0 ? start[1] : start[1].first(query);
    request.agent = QString::fromUtf8(headers.value("x-agent"));
    request.bodyOffset = headerEnd + 4;
    request.bodyLength = static_cast<qsizetype>(length);
    request.expectContinue = expectation == "100-continue";
    return {};
}

} // namespace

struct AgentDock::Impl {
    explicit Impl(AgentDock &owner)
        : owner(owner)
    {
        connect(&server, &QTcpServer::newConnection, &owner, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                if (sockets.size() >= maxConnections) {
                    socket->abort();
                    socket->deleteLater();
                    continue;
                }
                sockets.insert(socket);
                socket->setReadBufferSize(maxRequestHead + maxRequestBody + 1);
                QObject::connect(socket, &QTcpSocket::disconnected, &this->owner, [this, socket] {
                    sockets.remove(socket);
                    socket->deleteLater();
                });
                QTimer::singleShot(5000, socket, [socket] { socket->abort(); });
                const auto request = std::make_shared<DockRequest>();
                QObject::connect(socket, &QTcpSocket::readyRead, &this->owner, [this, socket, request] {
                    handle(*socket, *request);
                });
                if (socket->bytesAvailable()) {
                    handle(*socket, *request);
                }
            }
        });
        poll.setInterval(2000);
        connect(&poll, &QTimer::timeout, &owner, [this] { scan(); });
        notify.setSingleShot(true);
        notify.setInterval(100);
        connect(&notify, &QTimer::timeout, &owner, [this] {
            if (this->owner.onChanged) {
                this->owner.onChanged();
            }
        });
    }

    void changed()
    {
        if (!notify.isActive()) {
            notify.start();
        }
    }

    QJsonArray sessionList() const
    {
        auto sorted = entries.values();
        std::ranges::sort(sorted, [](const auto &left, const auto &right) {
            const auto lr = rank(left.value("state"_L1).toString());
            const auto rr = rank(right.value("state"_L1).toString());
            if (lr != rr) {
                return lr > rr;
            }
            const auto lt = left.value("updatedAt"_L1).toInteger();
            const auto rt = right.value("updatedAt"_L1).toInteger();
            return lt == rt ? left.value("key"_L1).toString() < right.value("key"_L1).toString() : lt > rt;
        });
        QJsonArray result;
        for (const auto &entry : sorted) {
            result.append(entry);
        }
        return result;
    }

    void stop()
    {
        server.close();
        for (auto *socket : std::as_const(sockets).values()) {
            socket->abort();
        }
        poll.stop();
        entries.clear();
        logs.clear();
        ignoredLogs.clear();
        nextDiscovery = 0;
        changed();
    }

    QJsonObject info() const
    {
        return {
            {u"ok"_s, server.isListening()}, {u"app"_s, u"catwhale-agent-dock"_s},
            {u"host"_s, u"127.0.0.1"_s}, {u"port"_s, server.serverPort()},
            {u"startedAt"_s, startedAt},
        };
    }

    QJsonArray channelList() const
    {
        const auto home = QDir::homePath();
        const auto config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        const auto data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        const QHash<QString, QStringList> paths = {
            {u"hermes"_s, {config + u"/hermes/config.yaml"_s, data + u"/hermes/config.yaml"_s}},
            {u"codex"_s, {codexRoot}}, {u"claude-code"_s, {claudeRoot}},
            {u"openclaw"_s, {qEnvironmentVariable("OPENCLAW_HOME"), home + u"/.openclaw"_s,
                            config + u"/openclaw"_s, data + u"/openclaw"_s}},
            {u"mimocode"_s, {config + u"/mimocode"_s, data + u"/mimocode"_s, home + u"/.mimocode"_s,
                            home + u"/.local/bin/mimo"_s}},
            {u"workbuddy"_s, {config + u"/workbuddy"_s, config + u"/WorkBuddy"_s,
                             data + u"/workbuddy"_s, data + u"/WorkBuddy"_s, home + u"/.workbuddy"_s}},
        };
        QJsonArray result;
        for (const auto &agent : agents) {
            const auto candidates = paths.value(agent.id);
            QJsonArray found;
            for (const auto &path : candidates) {
                if (!path.isEmpty() && QFileInfo::exists(path)) {
                    found.append(path);
                }
            }
            const auto count = std::ranges::count_if(entries, [&agent](const auto &entry) {
                return entry.value("agent"_L1).toString() == agent.id;
            });
            const bool available = agent.id == "generic"_L1 || !found.isEmpty();
            const auto detail = available ? (agent.kind == "log"_L1 ? u"Following session logs"_s : u"Local state push available"_s)
                                          : u"Installation not detected"_s;
            result.append(QJsonObject{{u"id"_s, agent.id}, {u"name"_s, agent.name},
                {u"kind"_s, agent.kind}, {u"accent"_s, agent.accent}, {u"available"_s, available},
                {u"wired"_s, false}, {u"detail"_s, detail}, {u"paths"_s, found},
                {u"checkedAt"_s, QDateTime::currentMSecsSinceEpoch()}, {u"sessions"_s, count},
                {u"disabled"_s, false}});
        }
        return result;
    }

    QJsonObject snapshot() const
    {
        auto result = info();
        const auto list = sessionList();
        const auto first = list.isEmpty() ? QJsonObject{} : list.first().toObject();
        result.insert(u"updatedAt"_s, QDateTime::currentMSecsSinceEpoch());
        result.insert(u"state"_s, first.value("state"_L1).toString(QString(idle)));
        result.insert(u"dominant"_s, first.value("key"_L1).toString().isEmpty()
                      ? QJsonValue(QJsonValue::Null) : first.value("key"_L1));
        result.insert(u"selected"_s, QJsonValue::Null);
        result.insert(u"pinned"_s, QJsonValue::Null);
        result.insert(u"sessions"_s, list);
        result.insert(u"channels"_s, channelList());
        result.insert(u"config"_s, QJsonObject{{u"enabled"_s, true}, {u"port"_s, server.serverPort()},
                                              {u"keepDoneSec"_s, keepDoneMs / 1000}});
        result.insert(u"counters"_s, QJsonObject{{u"received"_s, received}, {u"dropped"_s, dropped},
                                                {u"parseErrors"_s, parseErrors}});
        return result;
    }

    void handle(QTcpSocket &socket, DockRequest &request)
    {
        if (socket.state() != QAbstractSocket::ConnectedState) {
            return;
        }
        request.data += socket.readAll();
        if (request.bodyOffset < 0) {
            if (const auto problem = parseRequestHeader(request, server.serverPort())) {
                if (problem->status.startsWith("413")) ++dropped;
                sendError(socket, problem->status, problem->reason);
                return;
            }
            if (request.bodyOffset < 0) {
                return;
            }
            if (request.expectContinue && request.data.size() < request.bodyOffset + request.bodyLength) {
                socket.write("HTTP/1.1 100 Continue\r\n\r\n");
            }
        }
        if (request.data.size() < request.bodyOffset + request.bodyLength) {
            return;
        }

        const auto &method = request.method;
        const auto &path = request.path;
        if (method == "GET" && (path == "/health" || path == "/")) {
            sendJson(socket, "200 OK", info());
        } else if (method == "GET" && path == "/api") {
            sendJson(socket, "200 OK", snapshot());
        } else if (method == "POST" && path == "/state") {
            QJsonParseError parse;
            const auto body = request.bodyLength ? request.data.sliced(request.bodyOffset, request.bodyLength)
                                                 : QByteArray("{}");
            const auto payload = QJsonDocument::fromJson(body, &parse);
            if (parse.error != QJsonParseError::NoError) {
                ++parseErrors;
                sendError(socket, "400 Bad Request", u"invalid json"_s);
            } else if (!payload.isObject()) {
                ++dropped;
                sendError(socket, "400 Bad Request", u"payload must be an object"_s);
            } else {
                sendJson(socket, "200 OK", ingest(payload.object(), request.agent));
            }
        } else {
            sendError(socket, "404 Not Found", u"not found"_s);
        }
    }

    QJsonObject ingest(const QJsonObject &raw, const QString &header)
    {
        ++received;
        const auto event = clipped(firstText(raw, {"event"_L1, "hook_event_name"_L1, "hookName"_L1}), 80);
        auto id = firstText(raw, {"agent"_L1, "agent_id"_L1, "source"_L1}, header);
        if (id.isEmpty() && !event.isEmpty()
            && (raw.contains("profile"_L1) || raw.contains("hook_event_name"_L1))) {
            id = u"hermes"_s;
        }
        id = normalizeAgent(id);

        auto state = normalizeState(raw.value("state"_L1).toString());
        if (state.isEmpty()) {
            state = eventState(event);
        }
        if (state.isEmpty()) {
            state = normalizeState(raw.value("status"_L1).toString());
        }
        if (state.isEmpty()) {
            state = QString(working);
        }

        auto sessionId = clipped(firstText(raw, {"session_id"_L1, "sessionId"_L1, "session"_L1}), 120);
        if (sessionId.isEmpty()) {
            sessionId = u"default"_s;
        }
        const auto now = QDateTime::currentMSecsSinceEpoch();
        auto entry = makeSession(id, sessionId, now);
        const auto key = entry.value("key"_L1).toString();
        const auto previous = entries.value(key);
        const auto cwd = clipped(firstText(raw, {"cwd"_L1}, previous.value("cwd"_L1).toString()), 200);
        const auto defaultTitle = cwd.isEmpty() ? sessionId.left(12) : QFileInfo(cwd).fileName();
        const auto fallbackTitle = firstText(previous, {"title"_L1}, firstText(raw, {"name"_L1}, defaultTitle));

        entry.insert(u"state"_s, state);
        entry.insert(u"source"_s, u"push"_s);
        entry.insert(u"event"_s, event.isEmpty() ? previous.value("event"_L1).toString() : event);
        entry.insert(u"title"_s, clipped(firstText(raw, {"session_title"_L1, "title"_L1}, fallbackTitle), 48));
        entry.insert(u"cwd"_s, cwd);
        entry.insert(u"tool"_s, clipped(firstText(raw, {"tool_name"_L1, "tool"_L1}, previous.value("tool"_L1).toString()), 40));
        entry.insert(u"lastUser"_s, clipped(firstText(raw, {"prompt"_L1, "last_user_message"_L1}, previous.value("lastUser"_L1).toString()), 300));
        entry.insert(u"lastAgent"_s, clipped(firstText(raw, {"last_assistant_message"_L1, "message"_L1, "text"_L1},
                                                      previous.value("lastAgent"_L1).toString()), 400));
        entry.insert(u"firstSeenAt"_s, previous.value("firstSeenAt"_L1).toInteger(now));
        entry.insert(u"stateSince"_s, previous.value("state"_L1).toString() == state
                     ? previous.value("stateSince"_L1).toInteger(now) : now);

        if (previous.isEmpty() && entries.size() >= maxSessions) {
            const auto oldest = std::min_element(entries.keyValueBegin(), entries.keyValueEnd(),
                [](auto left, auto right) { return left.second.value("updatedAt"_L1).toInteger()
                                                 < right.second.value("updatedAt"_L1).toInteger(); });
            entries.remove((*oldest).first);
        }
        entries.insert(key, entry);
        changed();
        return {{u"ok"_s, true}, {u"port"_s, server.serverPort()}, {u"session"_s,
            QJsonObject{{u"key"_s, key}, {u"agent"_s, id}, {u"state"_s, state}, {u"sessionId"_s, sessionId}}}};
    }

    void discover(qint64 now)
    {
        auto found = findRecentLogs(codexRoot, u"codex"_s, now);
        found.append(findRecentLogs(claudeRoot, u"claude-code"_s, now));
        for (auto &file : found) {
            const auto saved = std::ranges::find(logs, file.path, &LogFile::path);
            if (saved != logs.end() && saved->updated == file.updated) {
                file.size = saved->size;
                file.summary = std::move(saved->summary);
            }
        }
        logs = std::move(found);
        nextDiscovery = now + discoveryInterval;
    }

    void scan()
    {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto countBefore = entries.size();
        entries.removeIf([this, now](const auto &item) {
            const auto state = item.value().value("state"_L1).toString();
            const bool fromLog = item.value().value("source"_L1) == "log"_L1;
            const auto updated = item.value().value("updatedAt"_L1).toInteger();
            const auto ttl = fromLog ? logLiveMs : state == done || state == errorState ? keepDoneMs :
                state == sleeping ? 20'000 : state == waiting ? 600'000 : state == working ? 150'000 :
                state == speaking ? 180'000 : 300'000;
            return now - updated > ttl;
        });
        ignoredLogs.removeIf([now](const auto &item) { return now - item.value() > logLiveMs; });
        bool modified = entries.size() != countBefore;
        if (now >= nextDiscovery) {
            discover(now);
        }
        for (auto &log : logs) {
            const QFileInfo info(log.path);
            if (!info.isFile() || info.isSymLink()) {
                if (entries.value(log.key).value("source"_L1) == "log"_L1) {
                    modified |= entries.remove(log.key);
                }
                continue;
            }
            const auto updated = info.lastModified().toMSecsSinceEpoch();
            const auto previous = entries.value(log.key);
            if (now - updated > logLiveMs || ignoredLogs.value(log.key) == updated
                || previous.value("source"_L1) == "push"_L1) {
                continue;
            }
            if (updated != log.updated || log.size != info.size()) {
                log.summary = summarizeLog(log.path);
                log.updated = updated;
                log.size = info.size();
            }
            const auto state = now - updated <= logActiveMs ? working : log.summary.failed ? errorState : done;
            if (previous.value("updatedAt"_L1).toInteger() == updated && previous.value("state"_L1) == state) {
                continue;
            }
            const auto sessionId = info.completeBaseName();
            auto session = makeSession(log.agent, sessionId, updated);
            session.insert(u"state"_s, QString(state));
            session.insert(u"source"_s, u"log"_s);
            session.insert(u"event"_s, u"log"_s);
            session.insert(u"title"_s, log.summary.user.isEmpty() ? sessionId.left(12) : clipped(log.summary.user, 48));
            session.insert(u"tool"_s, log.summary.tool);
            session.insert(u"lastUser"_s, log.summary.user);
            session.insert(u"lastAgent"_s, log.summary.assistant);
            session.insert(u"firstSeenAt"_s, previous.value("firstSeenAt"_L1).toInteger(updated));
            session.insert(u"stateSince"_s, previous.value("state"_L1) == state
                           ? previous.value("stateSince"_L1).toInteger(updated) : now);
            entries.insert(log.key, session);
            modified = true;
        }
        if (modified) {
            changed();
        }
    }

    AgentDock &owner;
    QTcpServer server;
    QTimer poll;
    QTimer notify;
    QSet<QTcpSocket *> sockets;
    QHash<QString, QJsonObject> entries;
    QHash<QString, qint64> ignoredLogs;
    QList<LogFile> logs;
    QString codexRoot;
    QString claudeRoot;
    QString lastError;
    qint64 nextDiscovery = 0;
    qint64 keepDoneMs = 60'000;
    qint64 startedAt = 0;
    qint64 received = 0;
    qint64 dropped = 0;
    qint64 parseErrors = 0;
};

AgentDock::AgentDock(QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(*this))
{
}

AgentDock::~AgentDock()
{
    d->stop();
}

void AgentDock::configure(const QJsonObject &config)
{
    d->keepDoneMs = 1000LL * std::clamp(config.value("keepDoneSec"_L1).toInt(60), 1, 3600);
    const auto port = static_cast<quint16>(std::clamp(config.value("port"_L1).toInt(8770), 0, 65535));
    if (!config.value("enabled"_L1).toBool(true)) {
        d->stop();
        d->lastError.clear();
        return;
    }

    const auto home = QDir::homePath();
    const auto codex = firstText(config, {"codexRoot"_L1}, home + u"/.codex/sessions"_s);
    const auto claude = firstText(config, {"claudeRoot"_L1}, home + u"/.claude/projects"_s);
    if (d->codexRoot != codex || d->claudeRoot != claude) {
        d->codexRoot = codex;
        d->claudeRoot = claude;
        d->nextDiscovery = 0;
        if (d->entries.removeIf([](const auto &item) { return item.value().value("source"_L1) == "log"_L1; })) {
            d->changed();
        }
    }
    if (d->server.isListening() && (port == 0 || d->server.serverPort() == port)) {
        return;
    }

    d->stop();
    d->lastError.clear();
    if (d->server.listen(QHostAddress::LocalHost, port)) {
        d->startedAt = QDateTime::currentMSecsSinceEpoch();
        d->poll.start();
        QTimer::singleShot(0, this, [this] {
            if (d->poll.isActive()) d->scan();
        });
    } else {
        d->lastError = d->server.errorString();
    }
}

QJsonArray AgentDock::sessions() const
{
    return d->sessionList();
}

QString AgentDock::state() const
{
    const auto list = d->sessionList();
    return list.isEmpty() ? QString(idle) : list.first().toObject().value("state"_L1).toString();
}

QString AgentDock::error() const
{
    return d->lastError;
}

void AgentDock::clearEnded()
{
    const auto cleared = d->entries.removeIf([this](const auto &item) {
        const auto entry = item.value();
        if (!ended(entry.value("state"_L1).toString())) {
            return false;
        }
        if (entry.value("source"_L1) == "log"_L1) {
            d->ignoredLogs.insert(item.key(), entry.value("updatedAt"_L1).toInteger());
        }
        return true;
    });
    if (cleared) {
        d->changed();
    }
}

} // namespace catwhale
