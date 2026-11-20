// TITAN Studio — automated UI smoke test (--smoke-test). Drives the real application:
// real engine process, real IPC, real GPU viewport, synthetic mouse input for picking.
// Original TITAN code.
#pragma once

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <functional>

class EngineClient;
class SceneModel;
class ViewportItem;
class QQuickWindow;
class AppSettings;

class SmokeTest : public QObject {
    Q_OBJECT
public:
    SmokeTest(EngineClient* engine, SceneModel* model, AppSettings* settings, QString projectDir, QString shotDir,
              QString reportPath, QObject* parent = nullptr);
    void start(QQuickWindow* window);

private:
    using Cond = std::function<bool()>;
    void step(const QString& name, Cond until, int timeoutMs, std::function<void()> action);
    void next();
    void pass(const QString& name, double ms, const QString& note = {});
    void skip(const QString& name, const QString& why);  // recorded as skipped, never as passed
    int t(int ms) const { return m_unreal ? ms * 8 : ms; }  // Unreal is slower than the reference host
    bool worldBuilder() const;
    void fail(const QString& name, const QString& why);
    void finish(bool ok);
    bool shoot(const QString& file);
    void clickViewportAt(double fx, double fy);
    void dragViewport(double fx0, double fy0, double fx1, double fy1);
    void dragScene(QPointF a, QPointF b);  // a real mouse drag between two window (scene) points

    struct Step {
        QString name;
        std::function<void()> action;
        Cond until;
        int timeoutMs;
    };
    QPointer<EngineClient> m_engine;
    QPointer<SceneModel> m_model;
    QPointer<AppSettings> m_settings;
    QPointer<QQuickWindow> m_window;
    QPointer<ViewportItem> m_viewport;
    QString m_projectDir, m_shotDir, m_reportPath;
    QList<Step> m_steps;
    int m_index = -1;
    QElapsedTimer m_stepTimer, m_total;
    QJsonArray m_results;
    QJsonObject m_metrics;
    double m_heroId = 0;
    double m_heroY = 1.0, m_heroZ = 0.0;  // the cube's starting position in the engine's scene
    double m_startFrames = 0;
    int m_liveCalls = 0;
    double m_liveRtt = 0, m_liveDelivery = 0, m_liveRttMax = 0, m_liveDeliveryMax = 0;
    void liveRoundTrip();
    QString m_savedTheme;
    bool m_unreal = false;
    bool m_skipCurrent = false;  // set by a step action when the engine lacks the capability it tests
    QString m_skipWhy;
    QStringList m_skipped;
    QVariantMap m_cameraBefore;
    double m_framesBefore = 0;
    int m_countBefore = 0;
    int m_prefabObjects = 0;
    int m_recoverPhase = 0;
    bool m_unsupportedChecked = false;
    int m_groupPhase = 0;
    int m_multiPhase = 0;
    double m_sphereId = 0;
    int m_reparentPhase = 0;
    int m_lockPhase = 0;
    bool m_searchDeferred = false;
    bool m_sawAnimating = false;
    int m_bookmarkPhase = 0, m_bookmarksBefore = 0;
    QVariantMap m_bookmarkCam;
    int m_pastePhase = 0, m_pasteBaseCount = 0;
    double m_propsId = 0;
    int m_cutPhase = 0;
    int m_historyPhase = 0, m_historyStart = 0;
    QString m_historyName;
};
