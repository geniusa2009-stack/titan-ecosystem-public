// TITAN Studio — client side of the TITAN editor protocol. Supervises the engine host
// process, speaks the framed JSON protocol over QLocalSocket, and mirrors engine-owned
// state (project, selection, undo, recipes) for QML. Studio never mutates scene state
// locally: every edit is a request, and the engine's events update the UI.
// Original TITAN code.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJSValue>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QThread>
#include <QVariant>

#include <functional>
#include <memory>

#include <QQmlEngine>
class SceneModel;
class EngineConnection;
class RemoteFrameSource;

class EngineClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(QString connectionStatus READ connectionStatus NOTIFY connectionChanged)
    Q_PROPERTY(QVariantMap engineInfo READ engineInfo NOTIFY connectionChanged)
    // "reference" (titan-engine-host) or "unreal" (UnrealEditor + TitanStudioBridge); chosen before start().
    Q_PROPERTY(QString engineKind READ engineKind WRITE setEngineKind NOTIFY engineKindChanged)
    Q_PROPERTY(bool unrealAvailable READ unrealAvailable NOTIFY engineKindChanged)
    Q_PROPERTY(bool remoteViewport READ remoteViewport NOTIFY connectionChanged)
    Q_PROPERTY(QStringList capabilities READ capabilities NOTIFY connectionChanged)
    Q_PROPERTY(bool projectOpen READ projectOpen NOTIFY stateChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY stateChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY stateChanged)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY stateChanged)
    Q_PROPERTY(double revision READ revision NOTIFY stateChanged)
    Q_PROPERTY(QVariantList selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList recipes READ recipes NOTIFY recipesChanged)
    Q_PROPERTY(QVariantMap editorCamera READ editorCamera NOTIFY projectLoaded)
    // Saved viewpoints of the open project ([{name, camera}], capability camera.bookmarks).
    Q_PROPERTY(QVariantList cameraBookmarks READ cameraBookmarks NOTIFY cameraBookmarksChanged)
    Q_PROPERTY(double lastRttMs READ lastRttMs NOTIFY metricsChanged)
    Q_PROPERTY(double avgRttMs READ avgRttMs NOTIFY metricsChanged)
    Q_PROPERTY(double lastDeliveryMs READ lastDeliveryMs NOTIFY metricsChanged)
    Q_PROPERTY(double avgDeliveryMs READ avgDeliveryMs NOTIFY metricsChanged)
    Q_PROPERTY(double lastOpenMs READ lastOpenMs NOTIFY metricsChanged)
    Q_PROPERTY(double lastSaveMs READ lastSaveMs NOTIFY metricsChanged)
    Q_PROPERTY(double connectMs READ connectMs NOTIFY metricsChanged)
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY messageChanged)
    Q_PROPERTY(QString messageLevel READ messageLevel NOTIFY messageChanged)
    Q_PROPERTY(QStringList logLines READ logLines NOTIFY logChanged)
    // {available, objects, savedAtMs} when the opened project has a crash-recovery point; empty otherwise.
    Q_PROPERTY(QVariantMap recovery MEMBER m_recovery NOTIFY recoveryChanged)

public:
    using Callback = std::function<void(const QJsonObject& result, const QJsonObject& error)>;

    EngineClient(SceneModel* model, QString hostPath, QStringList extensionDirs, QObject* parent = nullptr);
    void setUnrealPaths(const QString& editorExecutable, const QString& project) {
        m_unrealEditor = editorExecutable;
        m_unrealProject = project;
        emit engineKindChanged();
    }
    ~EngineClient() override;

    void setJsEngine(QQmlEngine* engine) { m_js = engine; }

    bool connected() const { return m_connected; }
    QString connectionStatus() const { return m_connectionStatus; }
    QVariantMap engineInfo() const { return m_engineInfo; }
    QString engineKind() const { return m_engineKind; }
    void setEngineKind(const QString& kind);
    bool unrealAvailable() const;
    QStringList capabilities() const { return m_capabilities; }
    // True when the engine advertises `capability` at `minVersion` or later (titan/capabilities.h).
    Q_INVOKABLE bool supports(const QString& capability, int minVersion = 1) const;
    bool remoteViewport() const { return m_connected && m_capabilities.contains(QStringLiteral("viewport.frames")); }
    std::shared_ptr<RemoteFrameSource> frameSource() const { return m_frames; }
    quint64 framesNotified() const;
    bool projectOpen() const { return m_state.value("projectOpen").toBool(); }
    QString projectName() const { return m_state.value("name").toString(); }
    QString projectPath() const { return m_state.value("path").toString(); }
    bool dirty() const { return m_state.value("dirty").toBool(); }
    bool canUndo() const { return m_state.value("canUndo").toBool(); }
    bool canRedo() const { return m_state.value("canRedo").toBool(); }
    QString undoLabel() const { return m_state.value("undoLabel").toString(); }
    QString redoLabel() const { return m_state.value("redoLabel").toString(); }
    double revision() const { return m_state.value("revision").toDouble(); }
    QVariantList selection() const { return m_selection; }
    QVariantList recipes() const { return m_recipes; }
    QVariantMap editorCamera() const { return m_editorCamera; }
    QVariantList cameraBookmarks() const { return m_cameraBookmarks; }
    // Replaces the project's bookmarks; reverts and reports if the engine refuses them.
    Q_INVOKABLE void setCameraBookmarks(const QVariantList& bookmarks);
    double lastRttMs() const { return m_lastRttMs; }
    double avgRttMs() const { return m_rttCount ? m_rttTotal / double(m_rttCount) : 0.0; }
    double lastDeliveryMs() const { return m_lastDeliveryMs; }
    double avgDeliveryMs() const { return m_rttCount ? m_deliveryTotal / double(m_rttCount) : 0.0; }
    double lastOpenMs() const { return m_lastOpenMs; }
    double lastSaveMs() const { return m_lastSaveMs; }
    double connectMs() const { return m_connectMs; }
    QString lastMessage() const { return m_lastMessage; }
    QString messageLevel() const { return m_messageLevel; }
    QStringList logLines() const { return m_log; }

    // Generic request API. The QML overload calls `callback(result, error)`.
    int call(const QString& method, const QJsonObject& params, Callback cb = {});
    Q_INVOKABLE int request(const QString& method, const QVariantMap& params = {}, const QJSValue& callback = {});

    Q_INVOKABLE void start();
    Q_INVOKABLE void restart();
    Q_INVOKABLE void openProject(const QString& path);
    Q_INVOKABLE void createProject(const QString& path, const QString& name, const QString& sceneTemplate = {});
    Q_INVOKABLE void saveProject();
    Q_INVOKABLE void closeProject(bool force = false);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void select(const QVariantList& ids);
    Q_INVOKABLE void updateObject(double id, const QVariantMap& set, const QString& coalesce = {});
    Q_INVOKABLE void createPrimitive(const QString& shape);
    Q_INVOKABLE void deleteSelection();
    // Clipboard (capability edit.clipboard): the engine serializes the selection; Studio keeps it on the system
    // clipboard as application/x-titan-objects, so it survives closing a project and works across projects.
    Q_INVOKABLE void copySelection(bool cut = false);
    Q_INVOKABLE void pasteClipboard(bool intoSelection = false);
    Q_INVOKABLE bool clipboardHasObjects() const;
    // Test seam: the UI smoke test keeps copies in-process so it never overwrites the user's system clipboard.
    void setPrivateClipboard(bool on) { m_privateClipboard = on; }
    Q_INVOKABLE void setEditorCamera(const QVariantMap& camera);
    Q_INVOKABLE void notify(const QString& message, const QString& level = QStringLiteral("info"));
    Q_INVOKABLE void shutdownEngine();
    Q_INVOKABLE void resolveRecovery(bool recover);
    QVariantMap recovery() const { return m_recovery; }  // project.recover or project.discardRecovery

signals:
    void connectionChanged();
    void engineKindChanged();
    void recoveryChanged();
    void stateChanged();
    void selectionChanged();
    void recipesChanged();
    void metricsChanged();
    void messageChanged();
    void logChanged();
    void projectLoaded();
    void cameraBookmarksChanged();
    void projectClosed();
    void eventReceived(const QString& name, const QVariantMap& data);

private:
    struct Pending {
        QString method;
        Callback cb;
        QElapsedTimer timer;
        qint64 sentNs = 0;
    };

    void onMessage(const QJsonObject& msg, qint64 arrivalNs);
    void onSocketConnected();
    void onSocketDisconnected();
    void onProcessOutput();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void handleMessage(const QJsonObject& msg, qint64 arrivalNs);
    void handleEvent(const QString& name, const QJsonObject& data);
    void applyState(const QJsonObject& state);
    void applySelection(const QJsonArray& ids);
    void applyRecipes(const QJsonArray& recipes);
    void loadSnapshot(const QJsonObject& openResult);
    void setConnection(bool connected, const QString& status);
    void appendLog(const QString& line);
    void failAllPending(const QString& why);

    SceneModel* m_model;
    QString m_hostPath;
    QStringList m_extensionDirs;
    QPointer<QQmlEngine> m_js;
    QProcess m_process;
    QThread m_ioThread;
    EngineConnection* m_conn = nullptr;  // lives on m_ioThread
    std::shared_ptr<RemoteFrameSource> m_frames;
    bool m_socketUp = false;
    QString m_socketPath;
    QString m_engineKind = QStringLiteral("reference");
    QString m_unrealEditor, m_unrealProject;
    QStringList m_capabilities;
    QHash<QString, int> m_capabilityVersions;
    QVariantMap m_recovery;
    QByteArray m_stdoutBuf;
    QHash<int, Pending> m_pending;
    int m_nextId = 1;
    bool m_connected = false;
    bool m_stopping = false;
    QString m_connectionStatus = QStringLiteral("Engine not started");
    QVariantMap m_engineInfo;
    QVariantMap m_state;
    QVariantList m_selection;
    QVariantList m_recipes;
    QVariantMap m_editorCamera;
    QVariantList m_cameraBookmarks;
    bool m_privateClipboard = false;
    QByteArray m_clipboardBytes;  // used only with m_privateClipboard
    QByteArray clipboardObjects() const;
    QString m_recoverPath;
    double m_lastDeliveryMs = 0, m_deliveryTotal = 0;
    double m_lastRttMs = 0, m_rttTotal = 0, m_lastOpenMs = 0, m_lastSaveMs = 0, m_connectMs = 0;
    quint64 m_rttCount = 0;
    QElapsedTimer m_startTimer;
    QString m_lastMessage, m_messageLevel = QStringLiteral("info");
    QStringList m_log;
};
