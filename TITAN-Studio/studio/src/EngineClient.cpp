// TITAN Studio — engine client implementation. Original TITAN code.
#include "EngineClient.h"

#include "titan/capabilities.h"
#include "titan/protocol.h"

#include "EngineConnection.h"
#include "RemoteFrameSource.h"
#include "SceneModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QtEndian>

namespace {
constexpr int kMaxLogLines = 500;
}  // namespace

EngineClient::EngineClient(SceneModel* model, QString hostPath, QStringList extensionDirs, QObject* parent)
    : QObject(parent),
      m_model(model),
      m_hostPath(std::move(hostPath)),
      m_extensionDirs(std::move(extensionDirs)),
      m_frames(std::make_shared<RemoteFrameSource>()) {
    // Protocol I/O runs on its own thread (see EngineConnection.h).
    m_ioThread.setObjectName(QStringLiteral("TITAN engine I/O"));
    m_conn = new EngineConnection(m_frames);
    m_conn->moveToThread(&m_ioThread);
    connect(&m_ioThread, &QThread::finished, m_conn, &QObject::deleteLater);
    connect(m_conn, &EngineConnection::message, this, &EngineClient::onMessage);
    connect(m_conn, &EngineConnection::connected, this, [this] {
        m_socketUp = true;
        onSocketConnected();
    });
    connect(m_conn, &EngineConnection::disconnected, this, [this] {
        m_socketUp = false;
        onSocketDisconnected();
    });
    connect(m_conn, &EngineConnection::socketError, this, [this](const QString& m) { appendLog(QStringLiteral("socket: ") + m); });
    connect(m_conn, &EngineConnection::protocolError, this, [this](const QString& m) { appendLog(QStringLiteral("protocol: ") + m); });
    m_ioThread.start();
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &EngineClient::onProcessOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        const auto lines = QString::fromUtf8(m_process.readAllStandardError()).split('\n', Qt::SkipEmptyParts);
        for (const auto& l : lines) appendLog(l);
    });
    connect(&m_process, &QProcess::finished, this, &EngineClient::onProcessFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            setConnection(false, tr("Engine failed to start: %1").arg(m_process.errorString()));
    });
}

EngineClient::~EngineClient() {
    m_stopping = true;
    if (m_process.state() != QProcess::NotRunning) {
        // Ask the engine to exit cleanly first (Unreal tears its bridge down in OnEnginePreExit).
        if (m_socketUp) {
            call(QStringLiteral("engine.shutdown"), {});
            m_process.waitForFinished(m_engineKind == QLatin1String("unreal") ? 20000 : 1000);
        }
    }
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        // Unreal needs longer than the reference host to shut down cleanly.
        if (!m_process.waitForFinished(m_engineKind == QLatin1String("unreal") ? 15000 : 2000)) m_process.kill();
    }
    m_frames->setRepaintHook({});
    m_ioThread.quit();
    m_ioThread.wait();
}

void EngineClient::setEngineKind(const QString& kind) {
    const QString k = kind == QLatin1String("unreal") ? kind : QStringLiteral("reference");
    if (k == m_engineKind) return;
    m_engineKind = k;
    emit engineKindChanged();
}

bool EngineClient::unrealAvailable() const {
    return QFileInfo(m_unrealEditor).isExecutable() && QFileInfo::exists(m_unrealProject);
}

quint64 EngineClient::framesNotified() const { return m_conn ? m_conn->framesNotified.load() : 0; }

void EngineClient::start() {
    if (m_process.state() != QProcess::NotRunning) return;
    const bool unreal = m_engineKind == QLatin1String("unreal");
    const QString exe = unreal ? m_unrealEditor : m_hostPath;
    if (!QFileInfo(exe).isExecutable()) {
        setConnection(false, unreal ? tr("Unreal Editor not found at %1 (build it with scripts/ue_build.sh)").arg(exe)
                                    : tr("Engine host not found at %1").arg(exe));
        return;
    }
    m_socketPath = QDir(QDir::tempPath()).filePath(
        QStringLiteral("titan-studio-%1.sock").arg(QCoreApplication::applicationPid()));
    const QString pid = QString::number(QCoreApplication::applicationPid());
    QStringList args;
    if (unreal) {
        if (!QFileInfo::exists(m_unrealProject)) {
            setConnection(false, tr("TITAN host project not found at %1").arg(m_unrealProject));
            return;
        }
        // The bridge plugin is dormant unless -TitanSocket is given; see TitanStudioBridgeModule.cpp.
        args << m_unrealProject << QStringLiteral("-TitanSocket=") + m_socketPath << QStringLiteral("-TitanParentPid=") + pid
             << QStringLiteral("-TitanMinimizeEditor") << QStringLiteral("-nosplash") << QStringLiteral("-unattended")
             << QStringLiteral("-NoSound");
    } else {
        args << QStringLiteral("--socket") << m_socketPath << QStringLiteral("--parent-pid") << pid;
        for (const auto& d : m_extensionDirs) args << QStringLiteral("--extensions") << d;
        // User scene templates (project.saveTemplate / templates.list) live with the user's app data.
        args << QStringLiteral("--templates")
             << QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("Templates"))
             << QStringLiteral("--extension-state")
             << QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("extensions.json"));
    }
    m_stopping = false;
    m_stdoutBuf.clear();
    m_capabilities.clear();
    m_startTimer.start();
    setConnection(false, unreal ? tr("Starting Unreal Engine… (the first launch compiles shaders and can take many minutes)")
                                : tr("Starting engine…"));
    appendLog(QStringLiteral("studio: launching %1").arg(exe));
    m_process.start(exe, args);
}

void EngineClient::restart() {
    m_recoverPath = projectPath().isEmpty() ? m_recoverPath : projectPath();
    m_stopping = true;
    QMetaObject::invokeMethod(m_conn, &EngineConnection::abort, Qt::QueuedConnection);
    m_socketUp = false;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        m_process.waitForFinished(m_engineKind == QLatin1String("unreal") ? 15000 : 2000);
    }
    m_stopping = false;
    start();
}

void EngineClient::onProcessOutput() {
    m_stdoutBuf += m_process.readAllStandardOutput();
    int nl;
    while ((nl = m_stdoutBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuf.left(nl).trimmed();
        m_stdoutBuf.remove(0, nl + 1);
        if (line.startsWith("TITAN_HOST_READY")) {
            setConnection(false, tr("Connecting…"));
            QMetaObject::invokeMethod(m_conn, [c = m_conn, p = m_socketPath] { c->open(p); }, Qt::QueuedConnection);
        } else if (!line.isEmpty()) {
            appendLog(QString::fromUtf8(line));
        }
    }
}

void EngineClient::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    QMetaObject::invokeMethod(m_conn, &EngineConnection::abort, Qt::QueuedConnection);
    m_socketUp = false;
    m_frames->detach();
    failAllPending(QStringLiteral("engine process exited"));
    if (m_stopping) return;
    // Unexpected exit: keep the project path so restart() can reopen the last saved state.
    if (!projectPath().isEmpty()) m_recoverPath = projectPath();
    m_state.clear();
    m_model->clear();
    emit stateChanged();
    setConnection(false, status == QProcess::CrashExit ? tr("Engine crashed") : tr("Engine exited (code %1)").arg(exitCode));
    notify(tr("The engine process stopped. Use “Restart Engine”: changes up to the last recovery point (taken every 20 s) can be restored when the project reopens."),
           QStringLiteral("error"));
}

void EngineClient::onSocketConnected() {
    QJsonObject proto{{"min", 1}, {"max", 1}};
    call(QStringLiteral("session.hello"), {{"client", "TITAN Studio"}, {"protocol", proto}},
         [this](const QJsonObject& r, const QJsonObject& err) {
             if (!err.isEmpty()) {
                 setConnection(false, tr("Protocol negotiation failed: %1").arg(err.value("message").toString()));
                 return;
             }
             m_connectMs = double(m_startTimer.elapsed());
             m_engineInfo = r.value("engine").toObject().toVariantMap();
             m_engineInfo.insert("protocol", r.value("protocol").toInt());
             m_capabilities.clear();
             for (const auto& c : r.value("capabilities").toArray()) m_capabilities << c.toString();
             // Versions per titan/capabilities.h; backends that predate them (e.g. the current Unreal bridge)
             // only list names, which count as version 1.
             m_capabilityVersions.clear();
             const QJsonObject versions = r.value("capabilityVersions").toObject();
             for (const auto& c : m_capabilities) m_capabilityVersions.insert(c, versions.value(c).toInt(1));
             setConnection(true, tr("Connected"));
             applyState(r.value("state").toObject());
             emit metricsChanged();
             if (!m_recoverPath.isEmpty()) {
                 const QString p = m_recoverPath;
                 m_recoverPath.clear();
                 openProject(p);
             }
         });
}

void EngineClient::onSocketDisconnected() {
    if (m_connected) setConnection(false, tr("Disconnected from engine"));
    failAllPending(QStringLiteral("disconnected"));
}

void EngineClient::setConnection(bool c, const QString& status) {
    m_connected = c;
    m_connectionStatus = status;
    emit connectionChanged();
}

void EngineClient::appendLog(const QString& line) {
    m_log.append(line);
    while (m_log.size() > kMaxLogLines) m_log.removeFirst();
    emit logChanged();
}

void EngineClient::notify(const QString& message, const QString& level) {
    m_lastMessage = message;
    m_messageLevel = level;
    appendLog(QStringLiteral("[%1] %2").arg(level, message));
    emit messageChanged();
}

void EngineClient::failAllPending(const QString& why) {
    auto pending = std::move(m_pending);
    m_pending.clear();
    QJsonObject err{{"code", -1}, {"message", why}};
    for (auto& p : pending)
        if (p.cb) p.cb({}, err);
}

bool EngineClient::supports(const QString& capability, int minVersion) const {
    return m_capabilityVersions.value(capability, 0) >= minVersion;
}

int EngineClient::call(const QString& method, const QJsonObject& params, Callback cb) {
    if (!m_socketUp) {
        if (cb) cb({}, QJsonObject{{"code", -1}, {"message", "not connected to engine"}});
        return 0;
    }
    // Capability contract: never send a method the connected engine has not declared support for.
    if (const titan::CapabilitySpec* cap = titan::capabilityForMethod(method.toStdString());
        cap && !supports(QString::fromLatin1(cap->name))) {
        const QJsonObject err{{"code", titan::protocol::kUnsupported},
                              {"message", tr("%1 is not supported by the connected engine (%2): it does not provide '%3'")
                                              .arg(method, m_engineInfo.value(QStringLiteral("backend")).toString(),
                                                   QString::fromLatin1(cap->name))}};
        if (cb) QMetaObject::invokeMethod(this, [cb, err] { cb({}, err); }, Qt::QueuedConnection);  // async, like a reply
        return 0;
    }
    const int id = m_nextId++;
    QJsonObject req{{"id", id}, {"method", method}, {"params", params}};
    const QByteArray payload = QJsonDocument(req).toJson(QJsonDocument::Compact);
    QByteArray frame(4, 0);
    qToBigEndian<quint32>(quint32(payload.size()), frame.data());
    frame += payload;
    Pending p{method, std::move(cb), {}, titan_studio::monotonicNs()};
    p.timer.start();
    m_pending.insert(id, std::move(p));
    QMetaObject::invokeMethod(m_conn, [c = m_conn, frame] { c->write(frame); }, Qt::QueuedConnection);
    return id;
}

int EngineClient::request(const QString& method, const QVariantMap& params, const QJSValue& callback) {
    QJSValue cb = callback;
    return call(method, QJsonObject::fromVariantMap(params), [this, cb](const QJsonObject& r, const QJsonObject& e) mutable {
        if (!cb.isCallable() || !m_js) return;
        QJSValue res = m_js->toScriptValue(r.toVariantMap());
        QJSValue err = e.isEmpty() ? QJSValue(QJSValue::NullValue) : m_js->toScriptValue(e.toVariantMap());
        QJSValue out = cb.call({res, err});
        if (out.isError()) appendLog(QStringLiteral("qml callback error: ") + out.toString());
    });
}

void EngineClient::onMessage(const QJsonObject& msg, qint64 arrivalNs) { handleMessage(msg, arrivalNs); }

void EngineClient::handleMessage(const QJsonObject& msg, qint64 arrivalNs) {
    if (msg.contains("event")) {
        handleEvent(msg.value("event").toString(), msg.value("data").toObject());
        return;
    }
    const int id = msg.value("id").toInt();
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    Pending p = std::move(it.value());
    m_pending.erase(it);
    // Round trip = request written -> response bytes read on the I/O thread (engine time + transport).
    // Delivery = until this GUI-thread handler runs (adds UI event-loop latency, e.g. vsync pacing).
    m_lastRttMs = double(arrivalNs - p.sentNs) / 1e6;
    m_lastDeliveryMs = double(p.timer.nsecsElapsed()) / 1e6;
    m_rttTotal += m_lastRttMs;
    m_deliveryTotal += m_lastDeliveryMs;
    ++m_rttCount;
    emit metricsChanged();
    const QJsonObject err = msg.value("error").toObject();
    if (!err.isEmpty()) appendLog(QStringLiteral("%1 failed: %2").arg(p.method, err.value("message").toString()));
    if (p.cb) p.cb(msg.value("result").toObject(), err);
}

void EngineClient::handleEvent(const QString& name, const QJsonObject& data) {
    if (name == QLatin1String("scene.changed")) {
        m_model->applyChange(data.value("upserted").toArray(), data.value("removed").toArray());
        applyRecipes(data.value("recipes").toArray());
        applyState(data.value("state").toObject());
    } else if (name == QLatin1String("selection.changed")) {
        applySelection(data.value("ids").toArray());
    } else if (name == QLatin1String("project.state")) {
        applyState(data);
    } else if (name == QLatin1String("project.closed")) {
        m_model->clear();
        applyState(data);
        applySelection({});
        applyRecipes({});
        emit projectClosed();
    }
    emit eventReceived(name, data.toVariantMap());
}

void EngineClient::applyState(const QJsonObject& state) {
    if (state.isEmpty()) return;
    m_state = state.toVariantMap();
    emit stateChanged();
    if (state.contains("selection")) applySelection(state.value("selection").toArray());
}

void EngineClient::applySelection(const QJsonArray& ids) {
    QVariantList sel;
    QSet<quint64> set;
    for (const auto& v : ids) {
        sel.append(v.toDouble());
        set.insert(quint64(v.toDouble()));
    }
    if (sel == m_selection) return;
    m_selection = sel;
    m_model->setSelection(set);
    emit selectionChanged();
}

void EngineClient::applyRecipes(const QJsonArray& recipes) {
    QVariantList list = recipes.toVariantList();
    if (list == m_recipes) return;
    m_recipes = list;
    emit recipesChanged();
}

void EngineClient::loadSnapshot(const QJsonObject& r) {
    const QJsonObject scene = r.value("scene").toObject();
    m_model->resetFromJson(scene.value("objects").toArray());
    applyRecipes(scene.value("recipes").toArray());
    m_editorCamera = r.value("editor").toObject().value("camera").toObject().toVariantMap();
    m_cameraBookmarks = r.value("editor").toObject().value("bookmarks").toArray().toVariantList();
    emit cameraBookmarksChanged();
    m_lastOpenMs = r.value("openMs").toDouble();
    applyState(r.value("state").toObject());
    emit metricsChanged();
    emit projectLoaded();
}

void EngineClient::openProject(const QString& path) {
    call(QStringLiteral("project.open"), {{"path", path}}, [this, path](const QJsonObject& r, const QJsonObject& e) {
        if (!e.isEmpty()) {
            notify(tr("Could not open %1: %2").arg(path, e.value("message").toString()), QStringLiteral("error"));
            return;
        }
        loadSnapshot(r);
        m_recovery = r.value("recovery").toObject().toVariantMap();  // unsaved work from an unclean session
        emit recoveryChanged();
        notify(tr("Opened %1 in %2 ms").arg(projectName()).arg(m_lastOpenMs, 0, 'f', 1));
    });
}

void EngineClient::resolveRecovery(bool recover) {
    const QString method = recover ? QStringLiteral("project.recover") : QStringLiteral("project.discardRecovery");
    call(method, {}, [this, recover](const QJsonObject&, const QJsonObject& e) {
        if (!e.isEmpty()) {
            // Nothing was resolved: keep the recovery offer (and the banner) so the user can retry.
            notify(tr("Recovery failed: %1").arg(e.value("message").toString()), QStringLiteral("error"));
            return;
        }
        m_recovery.clear();
        emit recoveryChanged();
        notify(recover ? tr("Recovered unsaved changes. Save to keep them (Undo reverts the recovery).")
                       : tr("Discarded the recovery point."));
    });
}

void EngineClient::createProject(const QString& path, const QString& name, const QString& sceneTemplate) {
    QJsonObject params{{"path", path}, {"name", name}};
    if (!sceneTemplate.isEmpty()) params.insert(QStringLiteral("template"), sceneTemplate);
    call(QStringLiteral("project.create"), params,
         [this, path](const QJsonObject& r, const QJsonObject& e) {
             if (!e.isEmpty()) {
                 notify(tr("Could not create project: %1").arg(e.value("message").toString()), QStringLiteral("error"));
                 return;
             }
             loadSnapshot(r);
             notify(tr("Created project %1").arg(projectName()));
         });
}

void EngineClient::saveProject() {
    call(QStringLiteral("project.save"), {}, [this](const QJsonObject& r, const QJsonObject& e) {
        if (!e.isEmpty()) {
            notify(tr("Save failed: %1").arg(e.value("message").toString()), QStringLiteral("error"));
            return;
        }
        m_lastSaveMs = r.value("saveMs").toDouble();
        emit metricsChanged();
        notify(tr("Saved in %1 ms").arg(m_lastSaveMs, 0, 'f', 1), QStringLiteral("success"));
    });
}

void EngineClient::closeProject(bool force) {
    call(QStringLiteral("project.close"), {{"force", force}}, [this](const QJsonObject&, const QJsonObject& e) {
        if (!e.isEmpty()) notify(e.value("message").toString(), QStringLiteral("warning"));
    });
}

void EngineClient::undo() {
    call(QStringLiteral("edit.undo"), {}, [this](const QJsonObject& r, const QJsonObject&) {
        if (r.value("applied").toBool()) notify(tr("Undo %1").arg(r.value("label").toString()));
    });
}

void EngineClient::redo() {
    call(QStringLiteral("edit.redo"), {}, [this](const QJsonObject& r, const QJsonObject&) {
        if (r.value("applied").toBool()) notify(tr("Redo %1").arg(r.value("label").toString()));
    });
}

void EngineClient::select(const QVariantList& ids) {
    QJsonArray arr;
    for (const auto& v : ids) arr.append(v.toDouble());
    call(QStringLiteral("selection.set"), {{"ids", arr}});
}

void EngineClient::updateObject(double id, const QVariantMap& set, const QString& coalesce) {
    QJsonObject p{{"id", id}, {"set", QJsonObject::fromVariantMap(set)}};
    if (!coalesce.isEmpty()) p.insert("coalesce", coalesce);
    call(QStringLiteral("object.update"), p, [this](const QJsonObject&, const QJsonObject& e) {
        if (!e.isEmpty()) notify(tr("Edit rejected: %1").arg(e.value("message").toString()), QStringLiteral("warning"));
    });
}

void EngineClient::createPrimitive(const QString& shape) {
    QJsonObject t{{"position", QJsonArray{0, shape == QLatin1String("plane") ? 0.01 : 0.5, 3}}};
    QJsonObject p{{"type", "mesh"}, {"shape", shape}, {"transform", t}, {"color", QJsonArray{0.75, 0.77, 0.8}}};
    call(QStringLiteral("object.create"), p, [this](const QJsonObject& r, const QJsonObject& e) {
        if (!e.isEmpty()) {
            notify(e.value("message").toString(), QStringLiteral("warning"));
            return;
        }
        select({r.value("id").toDouble()});
    });
}

void EngineClient::deleteSelection() {
    if (m_selection.isEmpty()) return;
    QJsonArray arr;
    for (const auto& v : m_selection) arr.append(v.toDouble());
    call(QStringLiteral("object.delete"), {{"ids", arr}});
}

namespace {
const QString kObjectsMime = QStringLiteral("application/x-titan-objects");
}

void EngineClient::copySelection(bool cut) {
    if (m_selection.isEmpty()) return;
    if (!supports(QStringLiteral("edit.clipboard"))) {
        notify(tr("The connected engine does not support copy and paste"), QStringLiteral("error"));
        return;
    }
    QJsonArray arr;
    for (const auto& v : m_selection) arr.append(v.toDouble());
    call(QStringLiteral("object.copy"), {{"ids", arr}}, [this, cut, arr](const QJsonObject& r, const QJsonObject& e) {
        if (!e.isEmpty()) {
            notify(e.value("message").toString(), QStringLiteral("error"));
            return;
        }
        const QByteArray bytes = QJsonDocument(r.value("clipboard").toObject()).toJson(QJsonDocument::Compact);
        if (m_privateClipboard) {
            m_clipboardBytes = bytes;
        } else {
            auto* data = new QMimeData;
            data->setData(kObjectsMime, bytes);
            QGuiApplication::clipboard()->setMimeData(data);
        }
        const int n = r.value("count").toInt();
        if (!cut) {
            notify(tr("Copied %n object(s)", nullptr, n));
            return;
        }
        // Cut = copy, then delete exactly what was copied (the selection may have changed meanwhile). Success is
        // reported only once the engine confirms the deletion; if it fails the clipboard keeps the copy.
        call(QStringLiteral("object.delete"), {{"ids", arr}}, [this, n](const QJsonObject&, const QJsonObject& de) {
            if (de.isEmpty())
                notify(tr("Cut %n object(s)", nullptr, n));
            else
                notify(tr("Copied %n object(s) but could not remove them: %1", nullptr, n).arg(de.value("message").toString()),
                       QStringLiteral("error"));
        });
    });
}

QByteArray EngineClient::clipboardObjects() const {
    if (m_privateClipboard) return m_clipboardBytes;
    const QMimeData* d = QGuiApplication::clipboard()->mimeData();
    return d && d->hasFormat(kObjectsMime) ? d->data(kObjectsMime) : QByteArray();
}

bool EngineClient::clipboardHasObjects() const { return !clipboardObjects().isEmpty(); }

void EngineClient::pasteClipboard(bool intoSelection) {
    if (!supports(QStringLiteral("edit.clipboard"))) {
        notify(tr("The connected engine does not support copy and paste"), QStringLiteral("error"));
        return;
    }
    const QByteArray bytes = clipboardObjects();
    if (bytes.isEmpty()) {
        notify(tr("The clipboard holds no TITAN objects"));
        return;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        notify(tr("The clipboard content is damaged"), QStringLiteral("error"));
        return;
    }
    QJsonObject p{{"clipboard", doc.object()}};
    if (intoSelection) {
        if (m_selection.size() != 1) {
            notify(tr("Select one object to paste into"));
            return;
        }
        p.insert("parent", m_selection.first().toDouble());
    }
    call(QStringLiteral("object.paste"), p, [this](const QJsonObject& r, const QJsonObject& e) {
        if (!e.isEmpty()) notify(e.value("message").toString(), QStringLiteral("error"));
        else notify(tr("Pasted %n object(s)", nullptr, int(r.value("ids").toArray().size())));
    });
}

void EngineClient::setEditorCamera(const QVariantMap& camera) {
    m_editorCamera = camera;
    call(QStringLiteral("editor.setState"), {{"camera", QJsonObject::fromVariantMap(camera)}});
}

void EngineClient::setCameraBookmarks(const QVariantList& bookmarks) {
    if (!supports(QStringLiteral("camera.bookmarks"))) {
        notify(tr("The connected engine does not store camera bookmarks"), QStringLiteral("error"));
        return;
    }
    const QVariantList before = m_cameraBookmarks;
    m_cameraBookmarks = bookmarks;
    emit cameraBookmarksChanged();
    call(QStringLiteral("editor.setState"), {{"bookmarks", QJsonArray::fromVariantList(bookmarks)}},
         [this, before](const QJsonObject&, const QJsonObject& e) {
             if (e.isEmpty()) return;
             m_cameraBookmarks = before;
             emit cameraBookmarksChanged();
             notify(e.value("message").toString(), QStringLiteral("error"));
         });
}

void EngineClient::shutdownEngine() {
    m_stopping = true;
    call(QStringLiteral("engine.shutdown"), {});
}
