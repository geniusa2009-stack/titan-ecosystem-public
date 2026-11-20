// TITAN Studio — application entry point. Original TITAN code.
#include "AppSettings.h"
#include "EngineClient.h"
#include "SceneModel.h"
#include "SmokeTest.h"
#include "ViewportItem.h"

#include "titan/version.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

#include <clocale>

namespace {
// Locate the engine host: next to the executable / inside the app bundle first, then the
// development build tree (compile-time path), so both installed and dev runs work.
QString findHost() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    for (const QString& candidate : {appDir.filePath("titan-engine-host"), appDir.filePath("../Helpers/titan-engine-host"),
                                     appDir.filePath("../../../titan-engine-host")}) {
        if (QFileInfo(candidate).isExecutable()) return QFileInfo(candidate).canonicalFilePath();
    }
    return QStringLiteral(TITAN_DEV_HOST_PATH);
}

QStringList findExtensionDirs() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    QStringList dirs;
    for (const QString& candidate : {appDir.filePath("../Resources/extensions"), appDir.filePath("../extensions")})
        if (QFileInfo(candidate).isDir()) dirs << QFileInfo(candidate).canonicalFilePath();
    if (dirs.isEmpty()) dirs << QStringLiteral(TITAN_DEV_EXTENSIONS_DIR);
    return dirs;
}
}  // namespace

int main(int argc, char* argv[]) {
    // GPU timestamps for the Performance workspace (QRhi::EnableTimestamps via Qt Quick).
    if (!qEnvironmentVariableIsSet("QSG_RHI_PROFILE")) qputenv("QSG_RHI_PROFILE", "1");
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");  // protocol and project files are locale-independent
    QGuiApplication::setApplicationName(QStringLiteral("TITAN Studio"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("TITAN Studio"));
    QGuiApplication::setOrganizationName(QStringLiteral("TITAN"));
    QGuiApplication::setApplicationVersion(QString::fromLatin1(titan::kEngineVersion));
    QQuickStyle::setStyle(QStringLiteral("Basic"));  // fully restyled by TITAN components

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("TITAN Studio — native editor for TITAN Engine"));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption openOpt(QStringLiteral("open"), QStringLiteral("Open the project at <dir>."), QStringLiteral("dir"));
    QCommandLineOption smokeOpt(QStringLiteral("smoke-test"), QStringLiteral("Run the UI smoke test with project <dir>."),
                                QStringLiteral("dir"));
    QCommandLineOption shotOpt(QStringLiteral("screenshot-dir"), QStringLiteral("Where the smoke test writes screenshots."),
                               QStringLiteral("dir"));
    QCommandLineOption reportOpt(QStringLiteral("report"), QStringLiteral("Smoke test JSON report path."),
                                 QStringLiteral("file"));
    QCommandLineOption hostOpt(QStringLiteral("engine-host"), QStringLiteral("Override engine host executable."),
                               QStringLiteral("path"));
    QCommandLineOption engineOpt(QStringLiteral("engine"),
                                 QStringLiteral("Engine backend: reference (titan-engine-host) or unreal (UE 5.8.4)."),
                                 QStringLiteral("kind"));
    QCommandLineOption ueEditorOpt(QStringLiteral("unreal-editor"), QStringLiteral("UnrealEditor executable."),
                                   QStringLiteral("path"));
    QCommandLineOption ueProjectOpt(QStringLiteral("unreal-project"), QStringLiteral("TITAN host .uproject."),
                                    QStringLiteral("path"));
    cli.addOptions({openOpt, smokeOpt, shotOpt, reportOpt, hostOpt, engineOpt, ueEditorOpt, ueProjectOpt});
    cli.process(app);

    auto* settings = new AppSettings(&app);
    auto* scene = new SceneModel(&app);
    auto* client = new EngineClient(scene, cli.isSet(hostOpt) ? cli.value(hostOpt) : findHost(), findExtensionDirs(), &app);
    client->setUnrealPaths(cli.isSet(ueEditorOpt) ? cli.value(ueEditorOpt) : QStringLiteral(TITAN_DEV_UNREAL_EDITOR),
                           cli.isSet(ueProjectOpt) ? cli.value(ueProjectOpt) : QStringLiteral(TITAN_DEV_UNREAL_PROJECT));
    // The smoke test pins the reference host unless --engine is given; otherwise the saved preference wins.
    client->setEngineKind(cli.isSet(engineOpt) ? cli.value(engineOpt)
                          : cli.isSet(smokeOpt) ? QStringLiteral("reference")
                                                : settings->engineKind());

    QQmlApplicationEngine engine;
    client->setJsEngine(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("engineClient"), client);
    engine.rootContext()->setContextProperty(QStringLiteral("sceneModel"), scene);
    engine.rootContext()->setContextProperty(QStringLiteral("prefs"), settings);
    engine.rootContext()->setContextProperty(QStringLiteral("titanVersion"), QString::fromLatin1(titan::kEngineVersion));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(3); },
                     Qt::QueuedConnection);
    engine.loadFromModule("TitanStudio", "Main");
    if (engine.rootObjects().isEmpty()) return 3;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());

    client->start();
    if (cli.isSet(openOpt)) {
        const QString path = QFileInfo(cli.value(openOpt)).absoluteFilePath();
        QObject::connect(client, &EngineClient::connectionChanged, client, [client, path] {
            static bool done = false;
            if (client->connected() && !done) {
                done = true;
                client->openProject(path);
            }
        });
    }

    SmokeTest* smoke = nullptr;
    if (cli.isSet(smokeOpt)) {
        smoke = new SmokeTest(client, scene, settings, QFileInfo(cli.value(smokeOpt)).absoluteFilePath(),
                              cli.value(shotOpt), cli.value(reportOpt), &app);
        QTimer::singleShot(0, smoke, [smoke, window] { smoke->start(window); });
    }
    return app.exec();
}
