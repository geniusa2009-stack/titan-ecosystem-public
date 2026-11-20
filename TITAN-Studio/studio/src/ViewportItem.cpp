// TITAN Studio — GPU viewport implementation. Original TITAN code.
#include "ViewportItem.h"

#include "EngineClient.h"
#include "EngineConnection.h"
#include "SceneModel.h"

#include "titan/mesh.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QQuickWindow>
#include <QTimer>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace {
constexpr float kFovY = 50.0f, kNear = 0.05f, kFar = 600.0f;

// Must match the std140 block in shaders/mesh.{vert,frag}: 3 x mat4 + 4 x vec4 = 256 bytes.
struct Ubo {
    float mvp[16];
    float model[16];
    float normalMat[16];
    float color[4];
    float lightDir[4];  // xyz towards light, w = ambient
    float params[4];    // x = lit, y = selected, z = light intensity, w = ghost
    float sky[4];       // rgb sky, w = fog density
};
static_assert(sizeof(Ubo) == 256, "UBO layout must match shaders");

titan::Mat4 fromQt(const QMatrix4x4& q) {
    titan::Mat4 m;
    std::memcpy(m.m.data(), q.constData(), sizeof(float) * 16);  // both column-major
    return m;
}

titan::Mat4 transposeInverse(const titan::Mat4& m) {
    titan::Mat4 r;
    auto inv = m.inverse();
    if (!inv) return r;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) r.at(row, col) = inv->at(col, row);
    return r;
}

QShader loadShader(const QString& path) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) return QShader::fromSerialized(f.readAll());
    qWarning("TITAN viewport: shader %s missing", qPrintable(path));
    return {};
}

std::optional<titan::Vec3> hitPlaneY(const titan::Ray& r, float y) {
    if (std::fabs(r.direction.y) < 1e-6f) return std::nullopt;
    float t = (y - r.origin.y) / r.direction.y;
    if (t <= 0) return std::nullopt;
    return r.origin + r.direction * t;
}

titan::Vec3 vecFrom(const QVariant& v, titan::Vec3 def) {
    const QVariantList l = v.toList();
    if (l.size() != 3) return def;
    return {float(l[0].toDouble()), float(l[1].toDouble()), float(l[2].toDouble())};
}

class ViewportRenderer final : public QQuickRhiItemRenderer {
public:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    struct GpuMesh {
        std::unique_ptr<QRhiBuffer> vbuf, ibuf;
        quint32 indexCount = 0;
    };
    void createMesh(GpuMesh& out, const titan::MeshData& data, QRhiResourceUpdateBatch* u);
    void ensureUniformSlots(int needed);
    std::unique_ptr<QRhiGraphicsPipeline> makePipeline(QRhiGraphicsPipeline::Topology topo, bool depthTest,
                                                       bool depthWrite, bool blend, bool cullBack = false);

    QRhi* m_rhi = nullptr;
    int m_sampleCount = -1;
    GpuMesh m_meshes[5];  // indexed by titan::Shape
    GpuMesh m_gridMinor, m_gridMajor, m_box;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_solid, m_ghost, m_lines, m_overlay;
    int m_slots = 0;
    quint32 m_slotSize = 0;
    QRhiResourceUpdateBatch* m_initialUploads = nullptr;
    QShader m_vs, m_fs;
    QByteArray m_staging;

    ViewportFrameState m_state;
    std::shared_ptr<ViewportStats> m_stats;
    QElapsedTimer m_interval;
    double m_frameMs = 0, m_cpuMs = 0, m_gpuMs = 0;
    int m_drawCalls = 0;
    quint64 m_frames = 0;
    QString m_backend;

    // Engine-rendered frames (Unreal backend).
    void ensureFramePipeline();
    RemoteFrameSource::Frame m_scratch;
    quint64 m_remoteFrame = 0, m_remoteGeneration = 0;
    int64_t m_lastRemoteNs = 0;  // time the previous engine frame was shown
    bool m_haveRemote = false;
    titan::Mat4 m_frameView;     // view the displayed engine frame was rendered with
    bool m_frameViewValid = false;
    std::unique_ptr<QRhiTexture> m_frameTex;
    std::unique_ptr<QRhiSampler> m_frameSampler;
    std::unique_ptr<QRhiBuffer> m_frameUbuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_frameSrb;
    std::unique_ptr<QRhiGraphicsPipeline> m_framePs;
    QShader m_fvs, m_ffs;
};

void ViewportRenderer::ensureFramePipeline() {
    if (m_framePs && m_frameSrb) return;
    if (!m_frameUbuf) {
        m_frameUbuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
        m_frameUbuf->create();
    }
    if (!m_frameSampler) {
        m_frameSampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                               QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_frameSampler->create();
    }
    m_frameSrb.reset(m_rhi->newShaderResourceBindings());
    m_frameSrb->setBindings(
        {QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                  m_frameUbuf.get()),
         QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_frameTex.get(),
                                                   m_frameSampler.get())});
    m_frameSrb->create();
    m_framePs.reset(m_rhi->newGraphicsPipeline());
    m_framePs->setShaderStages({{QRhiShaderStage::Vertex, m_fvs}, {QRhiShaderStage::Fragment, m_ffs}});
    m_framePs->setVertexInputLayout({});
    m_framePs->setShaderResourceBindings(m_frameSrb.get());
    m_framePs->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_framePs->setSampleCount(renderTarget()->sampleCount());
    m_framePs->setTopology(QRhiGraphicsPipeline::Triangles);
    m_framePs->setDepthTest(false);
    m_framePs->setDepthWrite(false);
    if (!m_framePs->create()) qWarning("TITAN viewport: engine-frame pipeline creation failed");
}

void ViewportRenderer::createMesh(GpuMesh& out, const titan::MeshData& data, QRhiResourceUpdateBatch* u) {
    const auto vbytes = quint32(data.vertices.size() * sizeof(float));
    const auto ibytes = quint32(data.indices.size() * sizeof(uint32_t));
    out.vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, vbytes));
    out.vbuf->create();
    out.ibuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, ibytes));
    out.ibuf->create();
    u->uploadStaticBuffer(out.vbuf.get(), data.vertices.data());
    u->uploadStaticBuffer(out.ibuf.get(), data.indices.data());
    out.indexCount = quint32(data.indices.size());
}

void ViewportRenderer::ensureUniformSlots(int needed) {
    if (needed <= m_slots && m_ubuf) return;
    m_slots = std::max(needed, std::max(64, m_slots * 2));
    m_slotSize = quint32(m_rhi->ubufAligned(int(sizeof(Ubo))));
    if (!m_ubuf) m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 1));
    m_ubuf->setSize(m_slotSize * quint32(m_slots));
    m_ubuf->create();
    if (!m_srb) m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ubuf.get(),
        sizeof(Ubo))});
    m_srb->create();
}

std::unique_ptr<QRhiGraphicsPipeline> ViewportRenderer::makePipeline(QRhiGraphicsPipeline::Topology topo,
                                                                     bool depthTest, bool depthWrite, bool blend,
                                                                     bool cullBack) {
    std::unique_ptr<QRhiGraphicsPipeline> ps(m_rhi->newGraphicsPipeline());
    ps->setShaderStages({{QRhiShaderStage::Vertex, m_vs}, {QRhiShaderStage::Fragment, m_fs}});
    QRhiVertexInputLayout layout;
    layout.setBindings({{6 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                          {0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)}});
    ps->setVertexInputLayout(layout);
    ps->setShaderResourceBindings(m_srb.get());
    ps->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    ps->setSampleCount(renderTarget()->sampleCount());
    ps->setTopology(topo);
    // Primitives are wound counter-clockwise when seen from outside.
    ps->setFrontFace(QRhiGraphicsPipeline::CCW);
    ps->setCullMode(cullBack ? QRhiGraphicsPipeline::Back : QRhiGraphicsPipeline::None);
    ps->setDepthTest(depthTest);
    ps->setDepthWrite(depthWrite);
    ps->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    if (blend) {
        QRhiGraphicsPipeline::TargetBlend b;
        b.enable = true;
        b.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        b.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        b.srcAlpha = QRhiGraphicsPipeline::One;
        b.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        ps->setTargetBlends({b});
    }
    if (!ps->create()) qWarning("TITAN viewport: pipeline creation failed");
    return ps;
}

void ViewportRenderer::initialize(QRhiCommandBuffer*) {
    if (m_rhi != rhi()) {  // new or changed QRhi: drop everything
        m_solid.reset();
        m_ghost.reset();
        m_lines.reset();
        m_overlay.reset();
        m_srb.reset();
        m_ubuf.reset();
        for (auto& m : m_meshes) m = {};
        m_gridMinor = {};
        m_gridMajor = {};
        m_box = {};
        m_slots = 0;
        m_framePs.reset();
        m_frameSrb.reset();
        m_frameUbuf.reset();
        m_frameSampler.reset();
        m_frameTex.reset();
        m_haveRemote = false;
        m_rhi = rhi();
        m_backend = QString::fromLatin1(m_rhi->backendName());
        m_vs = loadShader(QStringLiteral(":/shaders/mesh.vert.qsb"));
        m_fs = loadShader(QStringLiteral(":/shaders/mesh.frag.qsb"));
        m_fvs = loadShader(QStringLiteral(":/shaders/frame.vert.qsb"));
        m_ffs = loadShader(QStringLiteral(":/shaders/frame.frag.qsb"));
    }
    if (!m_box.vbuf) {
        m_initialUploads = m_rhi->nextResourceUpdateBatch();
        for (titan::Shape s : {titan::Shape::Cube, titan::Shape::Plane, titan::Shape::Sphere, titan::Shape::Cylinder})
            createMesh(m_meshes[int(s)], titan::makeMesh(s, 40), m_initialUploads);
        createMesh(m_gridMinor, titan::makeGridLines(40, 1.0f), m_initialUploads);
        createMesh(m_gridMajor, titan::makeGridLines(8, 5.0f), m_initialUploads);
        createMesh(m_box, titan::makeUnitBoxLines(), m_initialUploads);
    }
    ensureUniformSlots(64);
    if (m_sampleCount != renderTarget()->sampleCount()) {
        m_sampleCount = renderTarget()->sampleCount();
        m_solid.reset();
        m_framePs.reset();
    }
    if (!m_solid) {
        m_solid = makePipeline(QRhiGraphicsPipeline::Triangles, true, true, false);
        m_ghost = makePipeline(QRhiGraphicsPipeline::Triangles, true, false, true, true);
        m_lines = makePipeline(QRhiGraphicsPipeline::Lines, true, false, true);
        m_overlay = makePipeline(QRhiGraphicsPipeline::Lines, false, false, true);
    }
}

void ViewportRenderer::synchronize(QQuickRhiItem* item) {
    auto* vp = static_cast<ViewportItem*>(item);
    m_state = vp->snapshot();
    if (!m_stats) m_stats = vp->statsBlock();
    vp->setBackendName(m_backend);
}

void ViewportRenderer::render(QRhiCommandBuffer* cb) {
    QElapsedTimer cpu;
    cpu.start();
    QRhiResourceUpdateBatch* u = m_initialUploads ? m_initialUploads : m_rhi->nextResourceUpdateBatch();
    m_initialUploads = nullptr;

    // Engine-rendered frame: copy the newest complete frame out of shared memory and upload it.
    bool drawRemote = false;
    if (m_state.remote) {
        const quint64 gen = m_state.remote->generation();
        if (gen != m_remoteGeneration) {  // engine re-created the channel: frame numbers restarted
            m_remoteGeneration = gen;
            m_remoteFrame = 0;
            m_haveRemote = false;
        }
        if (m_state.remote->copyLatest(m_remoteFrame, m_scratch)) {
            if (!m_frameTex || m_frameTex->pixelSize() != m_scratch.size) {
                m_frameTex.reset(m_rhi->newTexture(QRhiTexture::BGRA8, m_scratch.size));
                m_frameTex->create();
                m_frameSrb.reset();
                m_framePs.reset();
            }
            u->uploadTexture(m_frameTex.get(),
                             QRhiTextureUploadDescription({0, 0, QRhiTextureSubresourceUploadDescription(m_scratch.pixels)}));
            m_remoteFrame = m_scratch.number;
            m_haveRemote = true;
            // Overlays must line up with the pixels on screen, not with a camera the engine has not
            // rendered yet: keep the frame's own view matrix (sent with viewport.camera).
            std::memcpy(m_frameView.m.data(), m_scratch.view, sizeof(m_scratch.view));
            m_frameViewValid = m_scratch.view[15] != 0.0f;
            if (m_stats) {
                m_stats->remoteFrames.fetch_add(1);
                const int64_t now = titan_studio::monotonicNs();
                const double gap = m_lastRemoteNs ? double(now - m_lastRemoteNs) / 1e6 : 0.0;
                m_lastRemoteNs = now;
                if (gap > 0 && gap < 1000.0) {
                    const double prev = m_stats->remoteIntervalMs.load();
                    m_stats->remoteIntervalMs = prev > 0 ? prev * 0.9 + gap * 0.1 : gap;
                } else if (gap >= 1000.0) {
                    m_stats->remoteIntervalMs = 0;  // idle (on-demand rendering): no rate to report
                }
                m_stats->remoteLatencyMs = double(titan_studio::monotonicNs() - m_scratch.captureNs) / 1e6;
                m_stats->remoteEngineGpuMs = m_scratch.engineGpuMs;
                m_stats->remoteWidth = m_scratch.size.width();
                m_stats->remoteHeight = m_scratch.size.height();
            }
        }
        drawRemote = m_haveRemote && m_frameTex;
        if (drawRemote) {
            ensureFramePipeline();
            const float params[4] = {m_rhi->isYUpInNDC() ? 1.0f : -1.0f, 1.0f, 0, 0};
            u->updateDynamicBuffer(m_frameUbuf.get(), 0, 16, params);
        }
    } else {
        m_haveRemote = false;
    }
    if (m_stats) m_stats->showingRemote = drawRemote;

    const QSize px = renderTarget()->pixelSize();
    const float aspect = px.height() > 0 ? float(px.width()) / float(px.height()) : 1.0f;
    const titan::Mat4 view = drawRemote && m_frameViewValid ? m_frameView
                                                            : titan::Mat4::lookAt(m_state.eye, m_state.target, {0, 1, 0});
    const titan::Mat4 vp = fromQt(m_rhi->clipSpaceCorrMatrix()) * titan::Mat4::perspective(kFovY, aspect, kNear, kFar) * view;

    struct Draw {
        const GpuMesh* mesh;
        QRhiGraphicsPipeline* ps;
    };
    QVector<Draw> draws;
    QVector<Ubo> ubos;
    auto push = [&](const GpuMesh* mesh, QRhiGraphicsPipeline* ps, const titan::Mat4& model, const float* rgba,
                    bool lit, bool selected, bool ghost) {
        if (!mesh || !mesh->vbuf) return;
        Ubo u{};
        const titan::Mat4 mvp = vp * model;
        std::memcpy(u.mvp, mvp.m.data(), 64);
        std::memcpy(u.model, model.m.data(), 64);
        const titan::Mat4 nm = transposeInverse(model);
        std::memcpy(u.normalMat, nm.m.data(), 64);
        std::memcpy(u.color, rgba, 16);
        const titan::Vec3 l = titan::normalize(m_state.lightDir);
        u.lightDir[0] = l.x;
        u.lightDir[1] = l.y;
        u.lightDir[2] = l.z;
        u.lightDir[3] = m_state.ambient;
        u.params[0] = lit ? 1.0f : 0.0f;
        u.params[1] = selected ? 1.0f : 0.0f;
        u.params[2] = m_state.lightIntensity;
        u.params[3] = ghost ? 1.0f : 0.0f;
        u.sky[0] = m_state.sky[0];
        u.sky[1] = m_state.sky[1];
        u.sky[2] = m_state.sky[2];
        u.sky[3] = 0.0105f;
        ubos.append(u);
        draws.append({mesh, ps});
    };

    // With an engine frame the scene itself comes from the engine; Studio only draws its overlays.
    if (!drawRemote) {
        for (const DrawItem& d : m_state.items)
            if (!d.ghost) push(&m_meshes[int(d.shape)], m_solid.get(), d.model, d.color, d.lit, d.selected, false);
        const float minor[4] = {m_state.grid[0], m_state.grid[1], m_state.grid[2], m_state.grid[3]};
        const float major[4] = {m_state.grid[0], m_state.grid[1], m_state.grid[2], std::min(1.0f, m_state.grid[3] * 2.2f)};
        const titan::Mat4 lift = titan::Mat4::translation({0, 0.002f, 0});
        push(&m_gridMinor, m_lines.get(), lift, minor, false, false, false);
        push(&m_gridMajor, m_lines.get(), lift, major, false, false, false);
    }
    for (const DrawItem& d : m_state.items)
        if (d.ghost) push(&m_meshes[int(d.shape)], m_ghost.get(), d.model, d.color, true, false, true);
    const float accent[4] = {m_state.accent[0], m_state.accent[1], m_state.accent[2], 1.0f};
    for (const titan::Mat4& box : m_state.selectionBoxes) push(&m_box, m_overlay.get(), box, accent, false, false, false);

    ensureUniformSlots(int(ubos.size()));
    m_staging.resize(qsizetype(m_slotSize) * ubos.size());
    for (int i = 0; i < ubos.size(); ++i)
        std::memcpy(m_staging.data() + qsizetype(m_slotSize) * i, &ubos[i], sizeof(Ubo));

    if (!ubos.isEmpty()) u->updateDynamicBuffer(m_ubuf.get(), 0, quint32(m_staging.size()), m_staging.constData());

    const QColor clear = QColor::fromRgbF(m_state.clear[0], m_state.clear[1], m_state.clear[2], 1.0f);
    cb->beginPass(renderTarget(), clear, {1.0f, 0}, u);
    cb->setViewport({0, 0, float(px.width()), float(px.height())});
    if (drawRemote && m_framePs) {
        cb->setGraphicsPipeline(m_framePs.get());
        cb->setShaderResources(m_frameSrb.get());
        cb->draw(3);
    }
    QRhiGraphicsPipeline* bound = nullptr;
    for (int i = 0; i < draws.size(); ++i) {
        const Draw& d = draws[i];
        if (d.ps != bound) {
            cb->setGraphicsPipeline(d.ps);
            bound = d.ps;
        }
        const QRhiCommandBuffer::DynamicOffset ofs(0, quint32(i) * m_slotSize);
        cb->setShaderResources(m_srb.get(), 1, &ofs);
        const QRhiCommandBuffer::VertexInput vin(d.mesh->vbuf.get(), 0);
        cb->setVertexInput(0, 1, &vin, d.mesh->ibuf.get(), 0, QRhiCommandBuffer::IndexUInt32);
        cb->drawIndexed(d.mesh->indexCount);
    }
    cb->endPass();

    m_drawCalls = int(draws.size()) + (drawRemote ? 1 : 0);
    ++m_frames;
    const double gpu = cb->lastCompletedGpuTime();
    if (gpu > 0) m_gpuMs = gpu * 1000.0;
    m_cpuMs = double(cpu.nsecsElapsed()) / 1e6;
    if (m_interval.isValid()) {
        const double dt = double(m_interval.nsecsElapsed()) / 1e6;
        m_frameMs = m_frameMs <= 0 ? dt : m_frameMs * 0.9 + dt * 0.1;
    }
    m_interval.start();
    if (m_stats) {  // published for the GUI thread; read after QQuickWindow::frameSwapped
        m_stats->frameMs = m_frameMs;
        m_stats->cpuMs = m_cpuMs;
        m_stats->gpuMs = m_gpuMs;
        m_stats->drawCalls = m_drawCalls;
        m_stats->frames = m_frames;
    }
    if (m_state.continuous) update();
    else m_interval.invalidate();  // on-demand frames: intervals are idle gaps, not frame times
}
}  // namespace

// ---------------------------------------------------------------------------------------

ViewportItem::ViewportItem() {
    setAcceptedMouseButtons(Qt::AllButtons);
    setFlag(ItemAcceptsInputMethod, false);
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);
    setSampleCount(4);
}

ViewportItem::~ViewportItem() {
    if (m_remoteSource) m_remoteSource->setRepaintHook({});  // waits for a hook running on the I/O thread
}

QQuickRhiItemRenderer* ViewportItem::createRenderer() { return new ViewportRenderer; }

// ---- engine-rendered viewport ------------------------------------------------------------
void ViewportItem::updateRemoteMode() {
    const bool active = m_engine && m_engine->remoteViewport() && m_engine->projectOpen();
    if (active == m_remoteActive) return;
    m_remoteActive = active;
    if (m_remoteSource) m_remoteSource->setRepaintHook({});
    m_remoteSource.reset();
    m_remoteSize = {};
    m_cameraInFlight = m_cameraPending = false;
    if (active) {
        m_remoteSource = m_engine->frameSource();
        // Runs on the I/O thread when a frame is announced; only posts a repaint to the GUI thread.
        m_remoteSource->setRepaintHook([this] { QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection); });
        configureRemote();
    }
    update();
}

void ViewportItem::configureRemote() {
    if (!m_remoteActive || !m_engine) return;
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize size(std::max(16, int(std::lround(width() * dpr * m_engineScale))),
                     std::max(16, int(std::lround(height() * dpr * m_engineScale))));
    if (size == m_remoteSize && m_remoteSource && m_remoteSource->attached()) {
        sendRemoteCamera();
        return;
    }
    m_remoteSize = size;
    QPointer<ViewportItem> self(this);
    m_engine->call(QStringLiteral("viewport.configure"), {{"width", size.width()}, {"height", size.height()}},
                   [self, size](const QJsonObject& r, const QJsonObject& e) {
                       if (!self || !self->m_remoteActive || size != self->m_remoteSize) return;
                       if (!e.isEmpty()) {
                           self->m_engine->notify(tr("Engine viewport unavailable: %1").arg(e.value("message").toString()),
                                                  QStringLiteral("warning"));
                           return;
                       }
                       QString err;
                       if (!self->m_remoteSource->attach(r.value("shm").toString(), &err)) {
                           self->m_engine->notify(tr("Engine viewport unavailable: %1").arg(err), QStringLiteral("warning"));
                           return;
                       }
                       self->m_engine->call(QStringLiteral("viewport.render"), {{"continuous", self->m_continuous}});
                       self->m_cameraInFlight = false;
                       self->sendRemoteCamera();
                   });
}

void ViewportItem::sendRemoteCamera() {
    if (!m_remoteActive || !m_engine) return;
    if (m_cameraInFlight) {  // latest camera wins; at most one request in flight
        m_cameraPending = true;
        return;
    }
    m_cameraInFlight = true;
    m_cameraPending = false;
    const titan::Vec3 e = eye();
    const titan::Vec3 f = titan::normalize(m_target - e);
    QJsonArray view;
    const titan::Mat4 v = titan::Mat4::lookAt(e, m_target, {0, 1, 0});
    for (float x : v.m) view.append(double(x));
    QPointer<ViewportItem> self(this);
    m_engine->call(QStringLiteral("viewport.camera"),
                   {{"eye", QJsonArray{e.x, e.y, e.z}},
                    {"forward", QJsonArray{f.x, f.y, f.z}},
                    {"fovY", double(kFovY)},
                    {"view", view},
                    {"revision", double(++m_cameraRevision)}},
                   [self](const QJsonObject&, const QJsonObject&) {
                       if (!self) return;
                       self->m_cameraInFlight = false;
                       if (self->m_cameraPending) self->sendRemoteCamera();
                   });
}

void ViewportItem::cameraMoved() {
    update();
    sendRemoteCamera();
}

void ViewportItem::setEngineRenderScale(double s) {
    s = std::clamp(s, 0.25, 1.0);
    if (s == m_engineScale) return;
    m_engineScale = s;
    emit appearanceChanged();
    configureRemote();
}

void ViewportItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
    if (!m_remoteActive || newGeometry.size() == oldGeometry.size()) return;
    if (!m_remoteConfigTimer) {  // resizing: reconfigure once the size settles
        m_remoteConfigTimer = new QTimer(this);
        m_remoteConfigTimer->setSingleShot(true);
        m_remoteConfigTimer->setInterval(150);
        connect(m_remoteConfigTimer, &QTimer::timeout, this, &ViewportItem::configureRemote);
    }
    m_remoteConfigTimer->start();
}

void ViewportItem::setEngine(EngineClient* e) {
    if (m_engine == e) return;
    m_engine = e;
    if (m_engine) {
        connect(m_engine, &EngineClient::projectLoaded, this, [this] {
            clearPreview();
            const QVariantMap cam = m_engine->editorCamera();
            if (cam.isEmpty()) resetCamera();
            else applyCamera(cam);
            updateRemoteMode();
        });
        connect(m_engine, &EngineClient::connectionChanged, this, &ViewportItem::updateRemoteMode);
        connect(m_engine, &EngineClient::stateChanged, this, &ViewportItem::updateRemoteMode);
    }
    emit engineChanged();
}

void ViewportItem::setScene(SceneModel* s) {
    if (m_scene == s) return;
    if (m_scene) disconnect(m_scene, nullptr, this, nullptr);
    m_scene = s;
    if (m_scene) {
        connect(m_scene, &SceneModel::sceneDataChanged, this, &ViewportItem::rebuildDrawList);
        connect(m_scene, &SceneModel::selectionApplied, this, &ViewportItem::rebuildDrawList);
    }
    rebuildDrawList();
    emit sceneChanged();
}

void ViewportItem::setCanvasColor(const QColor& c) {
    m_canvasColor = c;
    emit appearanceChanged();
    update();
}
void ViewportItem::setGridColor(const QColor& c) {
    m_gridColor = c;
    emit appearanceChanged();
    update();
}
void ViewportItem::setAccentColor(const QColor& c) {
    m_accentColor = c;
    rebuildDrawList();
    emit appearanceChanged();
}

void ViewportItem::setContinuous(bool c) {
    if (m_continuous == c) return;
    m_continuous = c;
    emit continuousChanged();
    if (m_remoteActive && m_engine) m_engine->call(QStringLiteral("viewport.render"), {{"continuous", c}});
    update();
}

void ViewportItem::rebuildDrawList() {
    m_items.clear();
    m_boxes.clear();
    if (m_scene) {
        const float ar = float(m_accentColor.redF()), ag = float(m_accentColor.greenF()), ab = float(m_accentColor.blueF());
        for (const StudioObject& o : m_scene->objects()) {
            if (!o.visible) continue;
            const bool selected = m_scene->selectedIds().contains(o.id);
            const titan::Mat4 world = m_scene->worldMatrix(o.id);
            if (selected) {
                const titan::Mat4 boxScale = o.shape == titan::Shape::Plane
                                                 ? titan::Mat4::scaling({1.0f, 0.002f, 1.0f})
                                                 : titan::Mat4::scaling({1.02f, 1.02f, 1.02f});
                if (o.shape != titan::Shape::None) m_boxes.append(world * boxScale);
            }
            if (o.shape == titan::Shape::None) continue;
            DrawItem d;
            d.shape = o.shape;
            d.model = world;
            const float t = selected ? 0.28f : 0.0f;  // selection tint towards the TITAN accent
            d.color[0] = o.r * (1 - t) + ar * t;
            d.color[1] = o.g * (1 - t) + ag * t;
            d.color[2] = o.b * (1 - t) + ab * t;
            d.lit = o.type != QLatin1String("light");
            d.selected = selected;
            m_items.append(d);
        }
    }
    update();
}

void ViewportItem::setPreview(const QVariantList& objects) {
    m_preview.clear();
    for (const QVariant& v : objects) {
        const QVariantMap o = v.toMap();
        const QVariantMap t = o.value("transform").toMap();
        titan::Transform tr;
        tr.position = vecFrom(t.value("position"), {0, 0, 0});
        tr.rotation = vecFrom(t.value("rotation"), {0, 0, 0});
        tr.scale = vecFrom(t.value("scale"), {1, 1, 1});
        DrawItem d;
        d.shape = titan::shapeFromString(o.value("shape").toString().toStdString()).value_or(titan::Shape::Cube);
        d.model = tr.matrix();
        d.color[0] = float(m_accentColor.redF());
        d.color[1] = float(m_accentColor.greenF());
        d.color[2] = float(m_accentColor.blueF());
        d.color[3] = 0.55f;
        d.ghost = true;
        m_preview.append(d);
    }
    emit previewChanged();
    update();
}

void ViewportItem::clearPreview() {
    if (m_preview.isEmpty()) return;
    m_preview.clear();
    emit previewChanged();
    update();
}

titan::Vec3 ViewportItem::eye() const {
    const float yaw = titan::radians(m_yaw), pitch = titan::radians(m_pitch);
    return m_target + titan::Vec3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} *
                          m_distance;
}

ViewportFrameState ViewportItem::snapshot() const {
    ViewportFrameState s;
    s.items = m_items;
    s.items += m_preview;
    s.selectionBoxes = m_boxes;
    s.eye = eye();
    s.target = m_target;
    s.continuous = m_continuous;
    s.remote = m_remoteSource;
    s.clear[0] = float(m_canvasColor.redF());
    s.clear[1] = float(m_canvasColor.greenF());
    s.clear[2] = float(m_canvasColor.blueF());
    s.grid[0] = float(m_gridColor.redF());
    s.grid[1] = float(m_gridColor.greenF());
    s.grid[2] = float(m_gridColor.blueF());
    s.grid[3] = float(m_gridColor.alphaF());
    s.accent[0] = float(m_accentColor.redF());
    s.accent[1] = float(m_accentColor.greenF());
    s.accent[2] = float(m_accentColor.blueF());
    s.sky[0] = s.clear[0];
    s.sky[1] = s.clear[1];
    s.sky[2] = s.clear[2];
    if (m_scene) {
        for (const StudioObject& o : m_scene->objects()) {
            if (o.type != QLatin1String("light") || !o.visible) continue;
            const titan::Vec3 fwd = m_scene->worldMatrix(o.id).transformDirection({0, 0, -1});
            s.lightDir = -titan::normalize(fwd);
            s.lightIntensity = o.intensity;
            break;
        }
    }
    return s;
}

void ViewportItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickRhiItem::itemChange(change, value);
    if (change == ItemSceneChange) {
        disconnect(m_swapConnection);
        if (value.window)  // frameSwapped is emitted on the render thread -> queued to us
            m_swapConnection = connect(value.window, &QQuickWindow::frameSwapped, this, &ViewportItem::statsChanged,
                                       Qt::QueuedConnection);
    } else if (change == ItemVisibleHasChanged && value.boolValue) {
        update();  // edits may have arrived while the workspace was hidden
    }
}

titan::Ray ViewportItem::rayAt(QPointF p) const {
    const float w = float(std::max(1.0, width())), h = float(std::max(1.0, height()));
    const titan::Mat4 vpm =
        titan::Mat4::perspective(kFovY, w / h, kNear, kFar) * titan::Mat4::lookAt(eye(), m_target, {0, 1, 0});
    const auto inv = vpm.inverse();
    if (!inv) return {eye(), m_target - eye()};
    const float x = 2.0f * float(p.x()) / w - 1.0f, y = 1.0f - 2.0f * float(p.y()) / h;
    const titan::Vec3 nearP = inv->transformPoint({x, y, -1}), farP = inv->transformPoint({x, y, 1});
    return {nearP, titan::normalize(farP - nearP)};
}

void ViewportItem::mousePressEvent(QMouseEvent* e) {
    forceActiveFocus(Qt::MouseFocusReason);
    stopCameraAnimation();  // the user takes over
    m_pressPos = m_lastPos = e->position();
    m_moved = false;
    m_drag = Drag::None;
    const bool alt = e->modifiers() & Qt::AltModifier;
    if (e->button() == Qt::LeftButton && alt && m_scene && m_engine && !m_engine->selection().isEmpty()) {
        const quint64 id = quint64(m_engine->selection().first().toDouble());
        const auto it = m_scene->objects().find(id);
        if (it != m_scene->objects().end()) {
            const titan::Mat4 world = m_scene->worldMatrix(id);
            m_dragPlaneY = world.transformPoint({0, 0, 0}).y;
            if (auto hit = hitPlaneY(rayAt(e->position()), m_dragPlaneY)) {
                m_drag = Drag::Move;
                m_dragId = id;
                m_dragStartHit = *hit;
                m_dragStartLocal = it->transform.position;
                m_dragParentInv = m_scene->worldMatrix(it->parent).inverse().value_or(titan::Mat4{});
                ++m_dragCounter;
            }
        }
    }
    if (m_drag == Drag::None) {
        if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::ShiftModifier)) m_drag = Drag::Orbit;
        else m_drag = Drag::Pan;
    }
    e->accept();
}

void ViewportItem::mouseMoveEvent(QMouseEvent* e) {
    const QPointF d = e->position() - m_lastPos;
    m_lastPos = e->position();
    if ((e->position() - m_pressPos).manhattanLength() > 3) m_moved = true;
    if (!m_moved) return;
    switch (m_drag) {
        case Drag::Orbit:
            m_yaw -= float(d.x()) * 0.35f;
            m_pitch = std::clamp(m_pitch + float(d.y()) * 0.35f, -89.0f, 89.0f);
            cameraMoved();
            break;
        case Drag::Pan: {
            const titan::Vec3 fwd = titan::normalize(m_target - eye());
            const titan::Vec3 right = titan::normalize(titan::cross(fwd, {0, 1, 0}));
            const titan::Vec3 up = titan::cross(right, fwd);
            const float s = m_distance * 0.0018f;
            m_target = m_target - right * (float(d.x()) * s) + up * (float(d.y()) * s);
            cameraMoved();
            break;
        }
        case Drag::Move:
            if (auto hit = hitPlaneY(rayAt(e->position()), m_dragPlaneY)) {
                const titan::Vec3 local = m_dragParentInv.transformDirection(*hit - m_dragStartHit);
                m_dragPending = (m_dragStartLocal + local).snappedXZ(float(m_snapStep));
                m_dragHasPending = true;
                sendDrag();
            }
            break;
        case Drag::None: break;
    }
    e->accept();
}

void ViewportItem::sendDrag() {
    // At most one move request in flight; the latest position wins (engine is authoritative,
    // the viewport updates from its scene.changed events).
    if (!m_engine || m_dragInFlight || !m_dragHasPending) return;
    m_dragHasPending = false;
    m_dragInFlight = true;
    const titan::Vec3 p = m_dragPending;
    QJsonObject params{{"id", double(m_dragId)},
                       {"set", QJsonObject{{"transform", QJsonObject{{"position", QJsonArray{p.x, p.y, p.z}}}}}},
                       {"coalesce", QStringLiteral("viewport-move-%1").arg(m_dragCounter)}};
    m_engine->call(QStringLiteral("object.update"), params, [this](const QJsonObject&, const QJsonObject&) {
        m_dragInFlight = false;
        sendDrag();
    });
}

void ViewportItem::mouseReleaseEvent(QMouseEvent* e) {
    if (!m_moved && e->button() == Qt::LeftButton && m_drag != Drag::Move && m_engine) {
        const titan::Ray r = rayAt(e->position());
        const bool additive = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier);
        QPointer<ViewportItem> self(this);
        m_engine->call(QStringLiteral("scene.pick"),
                       {{"origin", QJsonArray{r.origin.x, r.origin.y, r.origin.z}},
                        {"direction", QJsonArray{r.direction.x, r.direction.y, r.direction.z}}},
                       [self, additive](const QJsonObject& res, const QJsonObject& err) {
                           if (!self || !err.isEmpty()) return;
                           QVariantList sel = additive ? self->m_engine->selection() : QVariantList{};
                           if (!res.value("id").isNull()) {
                               const double id = res.value("id").toDouble();
                               if (additive && sel.contains(id)) sel.removeAll(id);
                               else sel.append(id);
                           }
                           self->m_engine->select(sel);
                       });
    } else if (m_moved && (m_drag == Drag::Orbit || m_drag == Drag::Pan)) {
        persistCamera();
    }
    m_drag = Drag::None;
    e->accept();
}

void ViewportItem::wheelEvent(QWheelEvent* e) {
    stopCameraAnimation();
    const double delta = !e->pixelDelta().isNull() ? e->pixelDelta().y() * 2.0 : e->angleDelta().y();
    m_distance = std::clamp(m_distance * float(std::pow(0.9985, delta)), 0.5f, 500.0f);
    cameraMoved();
    if (!m_persistTimer) {
        m_persistTimer = new QTimer(this);
        m_persistTimer->setSingleShot(true);
        m_persistTimer->setInterval(400);
        connect(m_persistTimer, &QTimer::timeout, this, &ViewportItem::persistCamera);
    }
    m_persistTimer->start();
    e->accept();
}

void ViewportItem::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_F:
            if (!frameSelection() && m_engine) m_engine->notify(QStringLiteral("Nothing to frame: select an object first"));
            break;
        case Qt::Key_Home: resetCamera(); break;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (m_engine) m_engine->deleteSelection();
            break;
        default: e->ignore(); return;
    }
    e->accept();
}

bool ViewportItem::frameSelection() {
    if (!m_scene || !m_engine) return false;
    const auto& objects = m_scene->objects();
    QSet<quint64> selected;
    for (const QVariant& v : m_engine->selection())
        if (objects.contains(quint64(v.toDouble()))) selected.insert(quint64(v.toDouble()));  // skip deleted ids
    if (selected.isEmpty()) return false;
    // World-space bounds of the selection and its descendants: unit-cube corners for shapes, origins for groups.
    titan::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    auto grow = [&](const titan::Vec3& p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    };
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        bool in = false;
        int guard = 0;
        for (quint64 a = it->id; a && !in && guard < 1024; a = objects.value(a).parent, ++guard) in = selected.contains(a);
        if (!in) continue;
        const titan::Mat4 w = m_scene->worldMatrix(it->id);
        if (it->shape == titan::Shape::None) { grow(w.transformPoint({0, 0, 0})); continue; }
        for (int k = 0; k < 8; ++k)
            grow(w.transformPoint({k & 1 ? 0.5f : -0.5f, k & 2 ? 0.5f : -0.5f, k & 4 ? 0.5f : -0.5f}));
    }
    const float radius = std::max(0.5f, titan::length(hi - lo) * 0.5f);
    moveCamera({(lo + hi) * 0.5f, m_yaw, m_pitch, std::clamp(radius * 2.6f, 1.5f, 400.0f)});
    return true;
}

bool ViewportItem::cameraAnimating() const { return m_animTimer && m_animTimer->isActive(); }

void ViewportItem::flyTo(const QVariantMap& c) {
    moveCamera({vecFrom(c.value("target"), {0, 1, 0}), float(c.value("yaw", 35).toDouble()),
                std::clamp(float(c.value("pitch", 24).toDouble()), -89.0f, 89.0f),
                std::clamp(float(c.value("distance", 18).toDouble()), 0.5f, 500.0f)});
}

void ViewportItem::stopCameraAnimation() {
    if (m_animTimer && m_animTimer->isActive()) {
        m_animTimer->stop();
        persistCamera();
        emit cameraAnimatingChanged();
    }
}

void ViewportItem::moveCamera(const CameraPose& to) {
    stopCameraAnimation();
    if (!m_smoothFocus) {
        m_target = to.target; m_yaw = to.yaw; m_pitch = to.pitch; m_distance = to.distance;
        cameraMoved();
        persistCamera();
        return;
    }
    m_animFrom = {m_target, m_yaw, m_pitch, m_distance};
    m_animTo = to;
    // Turn the short way round: yaw is periodic.
    m_animTo.yaw = m_animFrom.yaw + std::remainder(to.yaw - m_animFrom.yaw, 360.0f);
    if (!m_animTimer) {
        m_animTimer = new QTimer(this);
        m_animTimer->setTimerType(Qt::PreciseTimer);
        m_animTimer->setInterval(16);
        connect(m_animTimer, &QTimer::timeout, this, [this] {
            const float t = std::min(1.0f, float(m_animClock.elapsed()) / 260.0f);
            const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);  // ease-out cubic
            auto mix = [e](float a, float b) { return a + (b - a) * e; };
            m_target = m_animFrom.target + (m_animTo.target - m_animFrom.target) * e;
            m_yaw = mix(m_animFrom.yaw, m_animTo.yaw);
            m_pitch = mix(m_animFrom.pitch, m_animTo.pitch);
            // Distance eases in log space so zooming far and near feel the same.
            m_distance = std::exp(mix(std::log(m_animFrom.distance), std::log(m_animTo.distance)));
            cameraMoved();
            if (t >= 1.0f) {
                m_yaw = m_animTo.yaw = std::remainder(m_animTo.yaw, 360.0f);
                stopCameraAnimation();
            }
        });
    }
    m_animClock.start();
    m_animTimer->start();
    emit cameraAnimatingChanged();
}

void ViewportItem::resetCamera() {
    stopCameraAnimation();
    m_target = {0, 1, 0};
    m_yaw = 35;
    m_pitch = 24;
    m_distance = 18;
    cameraMoved();
}

QVariantMap ViewportItem::cameraState() const {
    return {{"yaw", double(m_yaw)},
            {"pitch", double(m_pitch)},
            {"distance", double(m_distance)},
            {"target", QVariantList{double(m_target.x), double(m_target.y), double(m_target.z)}}};
}

void ViewportItem::applyCamera(const QVariantMap& c) {
    stopCameraAnimation();
    m_yaw = float(c.value("yaw", 35).toDouble());
    m_pitch = std::clamp(float(c.value("pitch", 24).toDouble()), -89.0f, 89.0f);
    m_distance = std::clamp(float(c.value("distance", 18).toDouble()), 0.5f, 500.0f);
    m_target = vecFrom(c.value("target"), {0, 1, 0});
    cameraMoved();
}

void ViewportItem::persistCamera() {
    if (m_engine && m_engine->projectOpen()) m_engine->setEditorCamera(cameraState());
}
