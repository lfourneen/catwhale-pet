#include "config.hpp"
#include "controller.hpp"
#include "pet_window.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QWidget>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>
#include <unistd.h>

namespace {

const QString version = QStringLiteral(CATWHALE_VERSION);
constexpr int retryOpenGl = 75;
constexpr int retrySoftware = 76;

struct Options {
    QString renderer;
    bool hardwareDecode = true;
    bool isolated = false;
    bool smoke = false;
    bool diag = false;
    bool valid = true;
};

Options parseOptions(const QStringList &arguments)
{
    Options options;
    for (const auto &arg : arguments) {
        if (arg.startsWith("--renderer="))
            options.renderer = arg.section(u'=', 1);
        if (arg.startsWith("--decode=")) {
            const auto decoder = arg.section(u'=', 1);
            if (decoder != "auto" && decoder != "cpu")
                options.valid = false;
            options.hardwareDecode = decoder == "auto";
        }
    }
    options.valid = options.valid && QStringList{"", "auto", "vulkan", "opengl", "software"}.contains(options.renderer);
    options.isolated = arguments.contains("--isolated");
    options.smoke = arguments.contains("--smoke-test");
    options.diag = arguments.contains("--diag");
    if (arguments.contains("--no-gpu")) {
        options.renderer = "software";
        options.hardwareDecode = false;
    }
    return options;
}

QSGRendererInterface::GraphicsApi selectGraphics(const QString &requested)
{
    const auto choice = qEnvironmentVariable("CATWHALE_RENDER_RETRY", requested);
    const auto platform = QGuiApplication::platformName();
    if (choice == "software" || platform == "offscreen" || platform == "minimal")
        return QSGRendererInterface::Software;
    if (choice != "opengl" && catwhale::hasVulkanDevice())
        return QSGRendererInterface::Vulkan;
    if (catwhale::hasOpenGlDevice())
        return QSGRendererInterface::OpenGL;
    return QSGRendererInterface::Software;
}

QString graphicsName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Vulkan: return QStringLiteral("Vulkan");
    case QSGRendererInterface::OpenGL: return QStringLiteral("OpenGL");
    case QSGRendererInterface::Software: return QStringLiteral("Software");
    default: return QStringLiteral("Unknown");
    }
}

QString runtimeName()
{
    const auto path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const auto config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const auto suffix = QCryptographicHash::hash(config.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    return QDir(path).filePath("catwhale-pet-" + QString::fromLatin1(suffix));
}

bool forward(const QString &name, const QStringList &arguments)
{
    QLocalSocket connection;
    connection.connectToServer(name, QIODevice::WriteOnly);
    if (!connection.waitForConnected(300))
        return false;
    auto data = QJsonDocument(QJsonArray::fromStringList(arguments)).toJson(QJsonDocument::Compact);
    connection.write(data + '\n');
    while (connection.bytesToWrite() && connection.waitForBytesWritten(300)) {}
    return connection.bytesToWrite() == 0;
}

void listen(QLocalServer &server, const QString &name, catwhale::Controller &controller)
{
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(name);
    if (!server.listen(name)) {
        qWarning().noquote() << "Single-instance command socket:" << server.errorString();
        return;
    }
    QObject::connect(&server, &QLocalServer::newConnection, &controller, [&server, &controller] {
        while (auto *client = server.nextPendingConnection()) {
            client->setReadBufferSize(16 * 1024);
            const auto receive = [client, &controller] {
                if (!client->canReadLine()) {
                    if (client->bytesAvailable() >= client->readBufferSize())
                        client->abort();
                    return;
                }
                const auto document = QJsonDocument::fromJson(client->readLine());
                QStringList arguments;
                for (const auto &arg : document.array()) {
                    if (arg.isString())
                        arguments.append(arg.toString());
                }
                controller.command(arguments);
                client->disconnectFromServer();
            };
            QObject::connect(client, &QLocalSocket::readyRead, &controller, receive);
            QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            if (client->bytesAvailable() > 0)
                receive();
            QTimer::singleShot(1500, client, [client] { client->abort(); });
        }
    });
}

bool hasAlpha(const QImage &source)
{
    const auto frame = source.convertToFormat(QImage::Format_ARGB32);
    bool transparent = false;
    bool visible = false;
    for (int y = 0; y < frame.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            transparent |= qAlpha(row[x]) < 32;
            visible |= qAlpha(row[x]) > 220;
            if (transparent && visible)
                return true;
        }
    }
    return false;
}

int run(int argc, char **argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")
        && (qEnvironmentVariableIsSet("WAYLAND_DISPLAY") || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland")) {
        qputenv("QT_QPA_PLATFORM", "wayland");
    }
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("catwhale-pet"));
    app.setApplicationDisplayName(QStringLiteral("Catwhale Pet"));
    app.setApplicationVersion(version);
    app.setDesktopFileName(QStringLiteral("catwhale-pet"));
    app.setQuitOnLastWindowClosed(false);
    const auto args = app.arguments().mid(1);
    const auto options = parseOptions(args);
    if (!options.valid) {
        qCritical("Invalid renderer or decoder option. See --help.");
        return 2;
    }

    QLocalServer server;
    const auto name = runtimeName();
    QLockFile lock(name + ".lock");
    if (!options.isolated) {
        if (forward(name, args))
            return 0;
        if (!lock.tryLock(1400)) {
            if (forward(name, args))
                return 0;
            qCritical("The running Catwhale Pet instance did not respond.");
            return 2;
        }
    }
    if (args.contains("--quit"))
        return 0;

    const auto temporary = options.isolated ? std::make_unique<QTemporaryDir>() : nullptr;
    if (temporary && !temporary->isValid())
        return 2;
    catwhale::Config config(temporary ? temporary->path() : QString{});
    const auto graphics = selectGraphics(options.renderer);
    QQuickWindow::setGraphicsApi(graphics);
    QQuickWindow::setDefaultAlphaBuffer(true);
    catwhale::PetWindow window;
    window.setIcon(QIcon(config.icon()));
    catwhale::Controller controller(config, window, options.hardwareDecode, options.isolated);
    if (!options.isolated)
        listen(server, name, controller);
    QObject::connect(&window, &QQuickWindow::sceneGraphInitialized, &window, [&window, diag = options.diag] {
        const auto backend = graphicsName(window.rendererInterface()->graphicsApi());
        if (diag)
            QTextStream(stderr) << "Renderer: " << backend << '\n';
        else
            qInfo().noquote() << "Renderer:" << backend;
    }, Qt::DirectConnection);
    QObject::connect(&window, &QQuickWindow::sceneGraphError, &app, [&app, graphics](auto, const QString &error) {
        qWarning().noquote() << graphicsName(graphics) << "initialization failed:" << error;
        app.exit(graphics == QSGRendererInterface::Vulkan ? retryOpenGl
            : graphics == QSGRendererInterface::OpenGL ? retrySoftware : 2);
    }, Qt::QueuedConnection);

    if (options.smoke) {
        const bool smokeSettings = args.contains("--open-settings");
        QObject::connect(&controller, &catwhale::Controller::frameDisplayed, &app,
                         [&app, &window, &controller, smokeSettings](const QImage &frame) {
            if (!hasAlpha(frame))
                return;
            QTimer::singleShot(160, &app, [&app, &window, &controller, smokeSettings] {
                const auto visible = window.grabWindow();
                if (!hasAlpha(visible)) {
                    qCritical("A transparent pet frame could not be rendered.");
                    app.exit(3);
                    return;
                }
                const auto bounds = QRegion(QRect(QPoint(), window.size()));
                if (window.mask().intersected(bounds).isEmpty() || bounds.subtracted(window.mask()).isEmpty()) {
                    qCritical("The input region must include the pet and exclude transparent space.");
                    app.exit(3);
                    return;
                }
                if (smokeSettings) {
                    const auto widgets = QApplication::topLevelWidgets();
                    const bool opened = std::any_of(widgets.cbegin(), widgets.cend(), [](const QWidget *widget) {
                        return widget->isVisible() && widget->windowTitle() == "Catwhale Pet Settings";
                    });
                    if (!opened) {
                        qCritical("The settings window did not open.");
                        app.exit(3);
                        return;
                    }
                }
                qInfo().noquote() << "Transparent animation and input region verified with" << controller.clipBackend();

                app.exit(0);
            });
        }, Qt::SingleShotConnection);
        QObject::connect(&controller, &catwhale::Controller::playbackError, &app, [&app] { app.exit(3); });
        QTimer::singleShot(10000, &app, [&app] { qCritical("Animation smoke test timed out."); app.exit(3); });
    }
    if (options.diag) {
        QTextStream(stderr) << "Qt platform: " << QGuiApplication::platformName()
                            << "\nRequested renderer: " << graphicsName(graphics)
                            << "\nNiri IPC: " << (catwhale::NiriFollow::available() ? "available" : "unavailable")
                            << "\nAssets: " << config.assets() << '\n';
        QObject::connect(&controller, &catwhale::Controller::decoderSelected, &app, [](const QString &backend) {
            QTextStream(stderr) << "Video decoder: " << backend << '\n';
        });
    }
    if (!controller.start())
        return 2;
    QTimer::singleShot(options.smoke ? 0 : 250, &controller, [&controller, args] { controller.command(args); });
    return app.exec();
}

} // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--version") {
            QTextStream(stdout) << "Catwhale Pet " << version << '\n';
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            QTextStream(stdout) << "Usage: catwhale-pet [options]\n"
                "  --renderer=auto|vulkan|opengl|software\n"
                "  --decode=auto|cpu   --no-gpu   --diag\n"
                "  --open-menu        --toggle-menu\n"
                "  --open-settings    --toggle-settings\n"
                "  --toggle-pet       --show      --quit\n"
                "  --autostart=on|off  --version\n";
            return 0;
        }
    }
    std::vector<char *> originalArguments(argv, argv + argc + 1);
    const int status = run(argc, argv);
    if (status == retryOpenGl || status == retrySoftware) {
        setenv("CATWHALE_RENDER_RETRY", status == retryOpenGl ? "opengl" : "software", 1);
        execv("/proc/self/exe", originalArguments.data());
        qCritical("Could not restart with the fallback renderer.");
        return 2;
    }
    return status;
}
