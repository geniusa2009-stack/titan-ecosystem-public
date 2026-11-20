// TITAN Studio — GPU viewport. A QQuickRhiItem (QRhi: Metal on macOS, D3D on Windows,
// Vulkan/GL on Linux) that renders the engine-owned scene mirrored in SceneModel.
// Picking is delegated to the engine (scene.pick) so selection semantics live in one place.
// Original TITAN code.
#pragma once

#include "titan/math.h"
#include "titan/scene.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>
#include <QVariantList>
#include <QVector>

#include <atomic>
#include <memory>

#include "EngineClient.h"
#include "RemoteFrameSource.h"
#include "SceneModel.h"

struct DrawItem {
    titan::Shape shape = titan::Shape::Cube;
    titan::Mat4 model;
    float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    bool lit = true;
    bool selected = false;
    bool ghost = false;
};

// Written by the render thread after every frame, read by the GUI thread (lock-free).
struct ViewportStats {
    std::atomic<double> frameMs{0}, cpuMs{0}, gpuMs{0};
    std::atomic<int> drawCalls{0};
    std::atomic<quint64> frames{0};
    // Engine-rendered (Unreal) frames shown by this viewport.
    std::atomic<bool> showingRemote{false};
    std::atomic<quint64> remoteFrames{0};
    std::atomic<double> remoteLatencyMs{0};  // engine capture enqueued -> pixels uploaded for display
    std::atomic<double> remoteEngineGpuMs{0};
    // Interval between consecutive engine frames shown (moving average; resets after an idle gap), so the
    // UI can report the engine's real frame rate instead of Studio's 60 Hz re-presentation of the last frame.
    std::atomic<double> remoteIntervalMs{0};
    std::atomic<int> remoteWidth{0}, remoteHeight{0};
};

struct ViewportFrameState {
    QVector<DrawItem> items;
    QVector<titan::Mat4> selectionBoxes;
    titan::Vec3 eye{0, 5, 10}, target{0, 0, 0};
    titan::Vec3 lightDir{0.4f, 0.8f, 0.3f};
    float lightIntensity = 1.0f;
    float ambient = 0.28f;
    float sky[3] = {0.62f, 0.72f, 0.86f};
    float clear[3] = {0.1f, 0.11f, 0.13f};
    float grid[4] = {1, 1, 1, 0.08f};
    float accent[3] = {0.44f, 0.6f, 0.84f};
    bool continuous = false;
    std::shared_ptr<RemoteFrameSource> remote;  // set while the engine renders this viewport
};

class ViewportItem : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(EngineClient* engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(SceneModel* scene READ scene WRITE setScene NOTIFY sceneChanged)
    Q_PROPERTY(QColor canvasColor READ canvasColor WRITE setCanvasColor NOTIFY appearanceChanged)
    Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor NOTIFY appearanceChanged)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor NOTIFY appearanceChanged)
    Q_PROPERTY(bool continuous READ continuous WRITE setContinuous NOTIFY continuousChanged)
    Q_PROPERTY(int previewCount READ previewCount NOTIFY previewChanged)
    Q_PROPERTY(double frameMs READ frameMs NOTIFY statsChanged)
    Q_PROPERTY(double fps READ fps NOTIFY statsChanged)
    Q_PROPERTY(double cpuRenderMs READ cpuRenderMs NOTIFY statsChanged)
    Q_PROPERTY(double gpuMs READ gpuMs NOTIFY statsChanged)
    Q_PROPERTY(int drawCalls READ drawCalls NOTIFY statsChanged)
    Q_PROPERTY(double frameCount READ frameCount NOTIFY statsChanged)
    Q_PROPERTY(QString backendName READ backendName NOTIFY statsChanged)
    Q_PROPERTY(bool showingEngineFrames READ showingEngineFrames NOTIFY statsChanged)
    Q_PROPERTY(double engineFrameCount READ engineFrameCount NOTIFY statsChanged)
    Q_PROPERTY(double engineFrameLatencyMs READ engineFrameLatencyMs NOTIFY statsChanged)
    Q_PROPERTY(double engineGpuMs READ engineGpuMs NOTIFY statsChanged)
    Q_PROPERTY(double engineFps READ engineFps NOTIFY statsChanged)
    Q_PROPERTY(QString engineFrameSize READ engineFrameSize NOTIFY statsChanged)
    // Fraction of the viewport's physical pixels the engine renders (Retina: 0.5 = one engine pixel per point).
    Q_PROPERTY(double engineRenderScale READ engineRenderScale WRITE setEngineRenderScale NOTIFY appearanceChanged)
    Q_PROPERTY(double snapStep MEMBER m_snapStep NOTIFY appearanceChanged)  // move-drag grid in metres, 0 = off
    // Frame Selection and bookmarks fly the camera over ~0.25 s; off = jump (bound to reduced motion).
    Q_PROPERTY(bool smoothFocus MEMBER m_smoothFocus NOTIFY appearanceChanged)
    Q_PROPERTY(bool cameraAnimating READ cameraAnimating NOTIFY cameraAnimatingChanged)

public:
    ViewportItem();
    ~ViewportItem() override;

    EngineClient* engine() const { return m_engine; }
    void setEngine(EngineClient* e);
    SceneModel* scene() const { return m_scene; }
    void setScene(SceneModel* s);
    QColor canvasColor() const { return m_canvasColor; }
    QColor gridColor() const { return m_gridColor; }
    QColor accentColor() const { return m_accentColor; }
    void setCanvasColor(const QColor& c);
    void setGridColor(const QColor& c);
    void setAccentColor(const QColor& c);
    bool continuous() const { return m_continuous; }
    void setContinuous(bool c);
    int previewCount() const { return int(m_preview.size()); }

    double frameMs() const { return m_continuous ? m_stats->frameMs.load() : 0.0; }
    double fps() const { return frameMs() > 0 ? 1000.0 / frameMs() : 0.0; }
    double cpuRenderMs() const { return m_stats->cpuMs.load(); }
    double gpuMs() const { return m_stats->gpuMs.load(); }
    int drawCalls() const { return m_stats->drawCalls.load(); }
    double frameCount() const { return double(m_stats->frames.load()); }
    QString backendName() const { return m_backend; }
    bool showingEngineFrames() const { return m_stats->showingRemote.load(); }
    double engineFrameCount() const { return double(m_stats->remoteFrames.load()); }
    double engineFrameLatencyMs() const { return m_stats->remoteLatencyMs.load(); }
    double engineGpuMs() const { return m_stats->remoteEngineGpuMs.load(); }
    double engineFps() const { const double i = m_stats->remoteIntervalMs.load(); return i > 0 ? 1000.0 / i : 0.0; }
    QString engineFrameSize() const {
        return QStringLiteral("%1×%2").arg(m_stats->remoteWidth.load()).arg(m_stats->remoteHeight.load());
    }
    double engineRenderScale() const { return m_engineScale; }
    void setEngineRenderScale(double s);

    Q_INVOKABLE void setPreview(const QVariantList& objects);
    Q_INVOKABLE void clearPreview();
    // Frames the selected objects that still exist (and their descendants) by their world bounds; false when
    // there is nothing to frame (empty selection, or every selected object was deleted).
    Q_INVOKABLE bool frameSelection();
    Q_INVOKABLE void flyTo(const QVariantMap& camera);  // like applyCamera, animated when smoothFocus is on
    bool cameraAnimating() const;
    Q_INVOKABLE void resetCamera();
    Q_INVOKABLE void applyCamera(const QVariantMap& camera);
    Q_INVOKABLE QVariantMap cameraState() const;

    QQuickRhiItemRenderer* createRenderer() override;

    // Called by the renderer while the GUI thread is blocked (synchronize).
    ViewportFrameState snapshot() const;
    std::shared_ptr<ViewportStats> statsBlock() const { return m_stats; }
    void setBackendName(const QString& b) { m_backend = b; }

signals:
    void cameraAnimatingChanged();
    void engineChanged();
    void sceneChanged();
    void appearanceChanged();
    void continuousChanged();
    void previewChanged();
    void statsChanged();
    void cameraInteractionFinished();

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    titan::Vec3 eye() const;
    titan::Ray rayAt(QPointF p) const;
    void rebuildDrawList();
    void persistCamera();
    void sendDrag();
    void cameraMoved();
    void updateRemoteMode();
    void configureRemote();
    void sendRemoteCamera();

    QPointer<EngineClient> m_engine;
    QPointer<SceneModel> m_scene;
    QVector<DrawItem> m_items;
    QVector<titan::Mat4> m_boxes;
    QVector<DrawItem> m_preview;
    QColor m_canvasColor{20, 23, 32}, m_gridColor{255, 255, 255, 30}, m_accentColor{112, 152, 213};
    bool m_continuous = false;

    // Orbit camera
    titan::Vec3 m_target{0, 1, 0};
    float m_yaw = 35, m_pitch = 24, m_distance = 18;
    struct CameraPose {
        titan::Vec3 target;
        float yaw, pitch, distance;
    };
    void moveCamera(const CameraPose& to);  // jumps or animates, then persists
    void stopCameraAnimation();
    bool m_smoothFocus = true;
    class QTimer* m_animTimer = nullptr;
    QElapsedTimer m_animClock;
    CameraPose m_animFrom{}, m_animTo{};

    enum class Drag { None, Orbit, Pan, Move };
    Drag m_drag = Drag::None;
    QPointF m_pressPos, m_lastPos;
    bool m_moved = false;
    quint64 m_dragId = 0;
    float m_dragPlaneY = 0;
    titan::Vec3 m_dragStartHit, m_dragStartLocal;
    titan::Mat4 m_dragParentInv;
    titan::Vec3 m_dragPending;
    bool m_dragHasPending = false, m_dragInFlight = false;
    int m_dragCounter = 0;
    class QTimer* m_persistTimer = nullptr;

    std::shared_ptr<ViewportStats> m_stats = std::make_shared<ViewportStats>();
    QString m_backend;
    QMetaObject::Connection m_swapConnection;

    // Engine-rendered viewport (Unreal backend, capability "viewport.frames").
    bool m_remoteActive = false;
    QSize m_remoteSize;
    double m_engineScale = 0.5;
    double m_snapStep = 0.0;
    class QTimer* m_remoteConfigTimer = nullptr;
    bool m_cameraInFlight = false, m_cameraPending = false;
    quint32 m_cameraRevision = 0;
    std::shared_ptr<RemoteFrameSource> m_remoteSource;
};
