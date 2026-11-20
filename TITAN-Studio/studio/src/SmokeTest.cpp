// TITAN Studio — UI smoke test implementation. Original TITAN code.
#include "SmokeTest.h"

#include <algorithm>

#include "AppSettings.h"
#include "EngineClient.h"
#include "SceneModel.h"
#include "ViewportItem.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSysInfo>
#include <QTimer>

#include <cmath>
#include <cstdio>

namespace {
bool near3(const QVariant& v, double x, double y, double z) {
    const QVariantList l = v.toList();
    return l.size() == 3 && std::fabs(l[0].toDouble() - x) < 1e-4 && std::fabs(l[1].toDouble() - y) < 1e-4 &&
           std::fabs(l[2].toDouble() - z) < 1e-4;
}
}  // namespace

SmokeTest::SmokeTest(EngineClient* engine, SceneModel* model, AppSettings* settings, QString projectDir,
                     QString shotDir, QString reportPath, QObject* parent)
    : QObject(parent), m_engine(engine), m_model(model), m_settings(settings), m_projectDir(std::move(projectDir)),
      m_shotDir(std::move(shotDir)), m_reportPath(std::move(reportPath)) {}

void SmokeTest::step(const QString& name, Cond until, int timeoutMs, std::function<void()> action) {
    m_steps.append({name, std::move(action), std::move(until), timeoutMs});
}

bool SmokeTest::shoot(const QString& file) {
    if (!m_window || m_shotDir.isEmpty()) return false;
    QDir().mkpath(m_shotDir);
    const QImage img = m_window->grabWindow();
    return !img.isNull() && img.save(QDir(m_shotDir).filePath(file));
}

void SmokeTest::clickViewportAt(double fx, double fy) {
    if (!m_viewport || !m_window) return;
    const QPointF local(m_viewport->width() * fx, m_viewport->height() * fy);
    const QPointF scenePos = m_viewport->mapToScene(local);
    const QPointF global = m_window->mapToGlobal(scenePos);
    QMouseEvent press(QEvent::MouseButtonPress, scenePos, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, scenePos, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window, &release);
}

void SmokeTest::dragViewport(double fx0, double fy0, double fx1, double fy1) {
    if (!m_viewport || !m_window) return;
    auto at = [this](double fx, double fy) { return m_viewport->mapToScene(QPointF(m_viewport->width() * fx, m_viewport->height() * fy)); };
    dragScene(at(fx0, fy0), at(fx1, fy1));
}

void SmokeTest::dragScene(QPointF a, QPointF b) {
    if (!m_window) return;
    QMouseEvent press(QEvent::MouseButtonPress, a, m_window->mapToGlobal(a), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window, &press);
    for (int i = 1; i <= 12; ++i) {  // a real drag: several move events while the button is held
        const QPointF p = a + (b - a) * (i / 12.0);
        QMouseEvent move(QEvent::MouseMove, p, m_window->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window, &move);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, b, m_window->mapToGlobal(b), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window, &release);
}

void SmokeTest::start(QQuickWindow* window) {
    m_window = window;
    if (m_window) {  // background-launched, occluded windows are throttled by macOS
        m_window->raise();
        m_window->requestActivate();
    }
    m_viewport = window ? window->findChild<ViewportItem*>(QStringLiteral("viewport")) : nullptr;
    m_total.start();
    m_savedTheme = m_settings->themeMode();
    auto obj = [this](double id) { return m_model->object(id); };
    // Walk the visual item tree: Repeater/Loader-created items are not QObject children.
    auto findItem = [this](const char* name) -> QObject* {
        if (!m_window) return nullptr;
        const QString wanted = QString::fromLatin1(name);
        std::function<QQuickItem*(QQuickItem*)> walk = [&](QQuickItem* it) -> QQuickItem* {
            if (!it) return nullptr;
            if (it->objectName() == wanted && it->isVisible()) return it;
            for (QQuickItem* c : it->childItems())
                if (QQuickItem* r = walk(c)) return r;
            return nullptr;
        };
        return walk(m_window->contentItem());
    };
    static int baseCount = 0;
    static QString proposalId;
    static int proposed = 0;
    // Unreal (UE 5.8.4 editor + TitanStudioBridge) loads hundreds of modules and may compile shaders on its
    // first launch, so it gets longer timeouts; the reference host keeps the original ones.
    m_unreal = m_engine->engineKind() == QLatin1String("unreal");

    step("engine process starts and negotiates protocol", [this] { return m_engine->connected(); },
         m_unreal ? 3600000 : 15000, [] {});
    // Capability contract: a method of a capability the engine does not advertise is refused by Studio itself.
    step("unsupported operations are refused locally with a clear error", [this] { return m_skipCurrent || m_unsupportedChecked; },
         t(2000), [this] {
             m_unsupportedChecked = false;
             const QString probe = m_engine->supports(QStringLiteral("viewport.frames")) ? QStringLiteral("templates.list")
                                                                                          : QStringLiteral("viewport.configure");
             if (m_engine->supports(QStringLiteral("viewport.frames")) && m_engine->supports(QStringLiteral("templates"))) {
                 m_skipCurrent = true;
                 m_skipWhy = QStringLiteral("engine supports every probed capability");
                 return;
             }
             m_engine->call(probe, {{"width", 64}, {"height", 64}}, [this](const QJsonObject&, const QJsonObject& e) {
                 if (e.value("code").toInt() != 1007) return fail("unsupported operations are refused locally with a clear error",
                                                                  QStringLiteral("expected code 1007, got %1").arg(e.value("code").toInt()));
                 m_unsupportedChecked = e.value("message").toString().contains(QStringLiteral("is not supported by the connected engine"));  // refused by Studio, never sent
             });
         });
    step("launcher screenshot", [] { return true; }, t(1000), [this] { shoot(QStringLiteral("launcher.png")); });
    step("create project through engine",
         [this] { return m_engine->projectOpen() && m_model->objectCount() >= 10; }, t(10000), [this] {
             QDir(m_projectDir).removeRecursively();
             m_engine->createProject(m_projectDir, QStringLiteral("Smoke"));
         });
    step("editor shell + viewport render real frames", [this] {
        if (!m_viewport) m_viewport = m_window->findChild<ViewportItem*>(QStringLiteral("viewport"));
        return m_viewport && m_viewport->frameCount() >= 1 && m_viewport->width() > 100;  // on-demand: one frame per change
    }, t(30000), [this] { m_heroId = m_model->idByName(QStringLiteral("Hero Cube")); });
    step("click in viewport selects Hero Cube via engine pick",
         [this] { return m_engine->selection().contains(m_heroId); }, t(5000), [this, obj] {
             // Expected positions below keep the engine's own starting Y/Z for the cube (the reference scene
             // and the Unreal bridge's starter level place it differently).
             const QVariantList p = obj(m_heroId).value("position").toList();
             if (p.size() == 3) m_heroY = p[1].toDouble(), m_heroZ = p[2].toDouble();
             clickViewportAt(0.5, 0.52);
         });
    step("inspector shows the selected object", [findItem] {
        QObject* f = findItem("inspectorName");
        return f && f->property("text").toString() == QLatin1String("Hero Cube");
    }, t(5000), [] {});
    step("inspector number field accepts typed numbers", [findItem] {
        QObject* f = findItem("inspectorPosX");
        if (!f) return false;
        const QVariant before = f->property("text");
        f->setProperty("text", QStringLiteral("\u200E-1.25e2"));
        const bool number = f->property("acceptableInput").toBool();
        f->setProperty("text", QStringLiteral("1.2.3x"));
        const bool garbage = f->property("acceptableInput").toBool();
        f->setProperty("text", before);
        return number && !garbage;
    }, t(5000), [] {});
    step("edit position X through the Inspector field", [obj, this] {
        return near3(obj(m_heroId).value("position"), 2.5, m_heroY, m_heroZ);
    }, t(5000), [findItem, this] {
        QObject* f = findItem("inspectorPosX");
        if (f) QMetaObject::invokeMethod(f, "commitValue", Q_ARG(QVariant, QVariant(2.5)));
        else fail("edit position X through the Inspector field", "inspectorPosX not found");
    });
    step("edit position Z through the engine API", [obj, this] {
        return near3(obj(m_heroId).value("position"), 2.5, m_heroY, -1.5);
    }, t(5000), [this] {
        m_engine->updateObject(m_heroId, {{"transform", QVariantMap{{"position", QVariantList{2.5, m_heroY, -1.5}}}}});
    });
    // Multi-object Inspector: two objects selected, X differs -> "Mixed"; typing a value moves both, one undo.
    step("inspector shows mixed values for a multi-selection", [this, findItem] {
        if (m_skipCurrent) return true;
        QObject* f = findItem("inspectorPosX");
        return f && f->property("mixed").toBool() && f->property("text").toString().isEmpty();
    }, t(5000), [this] {
        if (!m_engine->supports(QStringLiteral("edit.multi"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability edit.multi");
            return;
        }
        m_sphereId = m_model->idByName(QStringLiteral("Sphere"));
        m_engine->select({m_heroId, m_sphereId});
    });
    step("one inspector edit moves every selected object; one undo restores them", [this, obj] {
        if (m_skipCurrent) return true;
        const auto x = [&](double id) { return obj(id).value("position").toList().value(0).toDouble(); };
        if (m_multiPhase == 0 && std::fabs(x(m_heroId) - 4.0) < 1e-4 && std::fabs(x(m_sphereId) - 4.0) < 1e-4) {
            m_engine->undo();
            m_multiPhase = 1;
        }
        return m_multiPhase == 1 && std::fabs(x(m_heroId) - 2.5) < 1e-4 && std::fabs(x(m_sphereId) - 4.0) > 1e-4;
    }, t(5000), [this, findItem] {
        if (!m_engine->supports(QStringLiteral("edit.multi"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability edit.multi");
            return;
        }
        m_multiPhase = 0;
        if (QObject* f = findItem("inspectorPosX")) {
            f->setProperty("text", QStringLiteral("4"));
            QMetaObject::invokeMethod(f, "commitValue", Q_ARG(QVariant, QVariant(4.0)));
        }
    });
    step("hierarchy rows show their type glyphs", [findItem] {
        QObject* g = findItem("hierarchyGlyph:Hero Cube");
        QObject* l = findItem("hierarchyGlyph:Sun");
        return g && l && g->property("text").toString() == QStringLiteral("◼") && l->property("text").toString() == QStringLiteral("✺");
    }, t(3000), [] {});
    // Hierarchy drag-and-drop: drag the Sphere row onto the Props group; its world position is kept.
    step("dragging a hierarchy row onto a group parents it, keeping its world position; undo restores", [this, obj] {
        if (m_skipCurrent) return true;
        const double props = m_model->idByName(QStringLiteral("Props"));
        const QVariantMap s = obj(m_sphereId);
        const double z = s.value("position").toList().value(2).toDouble();
        if (m_reparentPhase == 0 && s.value("parent").toDouble() == props && std::fabs(z - 5.5) < 1e-3) {
            m_engine->undo();
            m_reparentPhase = 1;
        }
        return m_reparentPhase == 1 && s.value("parent").toDouble() == 0 && std::fabs(z - 1.5) < 1e-3;
    }, t(5000), [this, findItem] {
        if (!m_engine->supports(QStringLiteral("edit.reparent"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability edit.reparent");
            return;
        }
        m_reparentPhase = 0;
        m_sphereId = m_model->idByName(QStringLiteral("Sphere"));
        m_engine->select({m_sphereId});
        auto* from = qobject_cast<QQuickItem*>(findItem("hierarchyRow:Sphere"));
        auto* to = qobject_cast<QQuickItem*>(findItem("hierarchyRow:Props"));
        if (!from || !to) { fail("dragging a hierarchy row onto a group parents it, keeping its world position; undo restores", "hierarchy rows not found"); return; }
        dragScene(from->mapToScene(QPointF(from->width() / 2, from->height() / 2)), to->mapToScene(QPointF(to->width() / 2, to->height() / 2)));
    });
    // Smart outliner: filters never change the selection; hidden selected objects are counted.
    step("type filter shows only lights and keeps the selection", [this, findItem] {
        QObject* info = findItem("hierarchySelectionInfo");
        return m_model->rowCount() == 1 && m_model->typeFilter() == QLatin1String("light") &&
               m_engine->selection().size() == 1 && m_engine->selection().value(0).toDouble() == m_heroId &&
               info && info->property("text").toString().contains(QLatin1String("1 hidden by filter"));
    }, t(3000), [this, findItem] {
        m_engine->select({m_heroId});
        if (QObject* seg = findItem("hierarchyType")) QMetaObject::invokeMethod(seg, "activated", Q_ARG(int, 2));
        else fail("type filter shows only lights and keeps the selection", "hierarchyType not found");
    });
    step("Select all selects every filtered match", [this] {
        return m_engine->selection().size() == 1 && m_engine->selection().value(0).toDouble() == m_model->idByName(QStringLiteral("Sun"));
    }, t(3000), [this, findItem] {
        if (QObject* b = findItem("hierarchySelectMatches")) QMetaObject::invokeMethod(b, "clicked");
        else fail("Select all selects every filtered match", "hierarchySelectMatches not found");
    });
    step("search applies once typing pauses", [this] {
        return m_searchDeferred && m_model->filter() == QLatin1String("Crate") && m_model->rowCount() == 4;  // Props + 3 crates
    }, t(3000), [this, findItem] {
        m_model->setTypeFilter(QString());
        QObject* s = findItem("hierarchySearch");
        if (!s) { fail("search applies once typing pauses", "hierarchySearch not found"); return; }
        s->setProperty("text", QStringLiteral("Crate"));
        m_searchDeferred = m_model->filter().isEmpty();  // not rebuilt on the keystroke itself
    });
    step("clear the outliner filters", [this] { return m_model->rowCount() > 4; }, t(3000), [this, findItem] {
        if (QObject* s = findItem("hierarchySearch")) s->setProperty("text", QString());
        m_model->setFilter(QString());
    });
    step("Lock on a multi-selection locks every object, disables the transform fields, one undo", [this, obj, findItem] {
        if (m_skipCurrent) return true;
        const bool locked = obj(m_heroId).value("locked").toBool() && obj(m_sphereId).value("locked").toBool();
        QObject* f = findItem("inspectorPosX");
        if (m_lockPhase == 0 && locked && f && !f->property("enabled").toBool()) {
            m_engine->undo();
            m_lockPhase = 1;
        }
        return m_lockPhase == 1 && !obj(m_heroId).value("locked").toBool() && !obj(m_sphereId).value("locked").toBool();
    }, t(5000), [this] {
        if (!m_engine->supports(QStringLiteral("edit.lock")) || !m_engine->supports(QStringLiteral("edit.multi"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capabilities edit.lock and edit.multi");
            return;
        }
        m_lockPhase = 0;
        m_sphereId = m_model->idByName(QStringLiteral("Sphere"));
        m_engine->select({m_heroId, m_sphereId});
        QTimer::singleShot(200, this, [this] {  // the action bar appears once two objects are selected
            QQuickItem* b = nullptr;
            std::function<void(QQuickItem*)> walk = [&](QQuickItem* it) {
                if (!b && it->objectName() == QLatin1String("hierarchyLockSelected") && it->isVisible()) b = it;
                for (QQuickItem* c : it->childItems()) walk(c);
            };
            if (m_window) walk(m_window->contentItem());
            if (b) QMetaObject::invokeMethod(b, "clicked");
        });
    });
    step("restore single selection", [this] { return m_engine->selection().size() == 1; }, t(3000),
         [this] { m_engine->select({m_heroId}); });
    // Camera tools: F with nothing selected does nothing; F flies to the selection's world bounds.
    step("Frame Selection with an empty selection leaves the camera alone", [this] {
        return m_engine->selection().isEmpty() && m_viewport->cameraState() == m_cameraBefore;
    }, t(3000), [this] {
        m_engine->select({});
        QTimer::singleShot(100, this, [this] {
            m_cameraBefore = m_viewport->cameraState();
            QMetaObject::invokeMethod(m_viewport, "frameSelection");
        });
    });
    step("Frame Selection flies to the selected object", [this, obj] {
        if (m_viewport->property("cameraAnimating").toBool()) m_sawAnimating = true;
        const QVariantList tg = m_viewport->cameraState().value("target").toList();
        const QVariantList p = obj(m_heroId).value("position").toList();
        bool there = tg.size() == 3 && p.size() == 3;
        for (int k = 0; there && k < 3; ++k) there = std::fabs(tg[k].toDouble() - p[k].toDouble()) < 1e-3;
        return there && !m_viewport->property("cameraAnimating").toBool() &&
               (m_sawAnimating || !m_viewport->property("smoothFocus").toBool());
    }, t(3000), [this] {
        m_sawAnimating = false;
        m_engine->select({m_heroId});
        QTimer::singleShot(100, this, [this] { QMetaObject::invokeMethod(m_viewport, "frameSelection"); });
    });
    step("camera bookmark saves, persists in the engine, recalls and deletes", [this] {
        if (m_skipCurrent) return true;
        const int n = m_engine->cameraBookmarks().size();
        if (m_bookmarkPhase == 0 && n == m_bookmarksBefore + 1) {  // saved; move away, then recall it
            m_bookmarkCam = m_engine->cameraBookmarks().last().toMap().value("camera").toMap();
            QMetaObject::invokeMethod(m_viewport, "resetCamera");
            QMetaObject::invokeMethod(m_window, "recallBookmark", Q_ARG(QVariant, QVariant(n - 1)));
            m_bookmarkPhase = 1;
        } else if (m_bookmarkPhase == 1 && !m_viewport->property("cameraAnimating").toBool()) {
            const QVariantMap now = m_viewport->cameraState();
            if (std::fabs(now.value("distance").toDouble() - m_bookmarkCam.value("distance").toDouble()) < 1e-3 &&
                std::fabs(now.value("yaw").toDouble() - m_bookmarkCam.value("yaw").toDouble()) < 1e-3) {
                QMetaObject::invokeMethod(m_window, "deleteBookmark", Q_ARG(QVariant, QVariant(n - 1)));
                m_bookmarkPhase = 2;
            }
        }
        return m_bookmarkPhase == 2 && n == m_bookmarksBefore;
    }, t(5000), [this] {
        if (!m_engine->supports(QStringLiteral("camera.bookmarks"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability camera.bookmarks");
            return;
        }
        m_bookmarkPhase = 0;
        m_bookmarksBefore = int(m_engine->cameraBookmarks().size());
        const QVariantMap view{{"yaw", 70.0}, {"pitch", 30.0}, {"distance", 11.0}, {"target", QVariantList{1.0, 0.5, -2.0}}};
        m_viewport->applyCamera(view);
        QMetaObject::invokeMethod(m_window, "saveBookmark");
    });
    // Copy/paste: a copied hierarchy pastes with fresh ids and its children attached; one undo removes it.
    step("copy and paste a group keeps its children with fresh ids; one undo removes the paste", [this, obj] {
        if (m_skipCurrent) return true;
        const int n = m_model->objectCount();
        if (m_pastePhase == 0 && m_engine->clipboardHasObjects()) {
            m_engine->pasteClipboard(false);
            m_pastePhase = 1;
        } else if (m_pastePhase == 1 && n == m_pasteBaseCount + 4 && m_engine->selection().size() == 1) {
            const double copy = m_engine->selection().value(0).toDouble();
            int kids = 0;
            for (const auto& o : m_model->objects())
                if (double(o.parent) == copy) ++kids;
            if (copy != m_propsId && obj(copy).value("name").toString() == QLatin1String("Props") && kids == 3) {
                m_engine->undo();
                m_pastePhase = 2;
            }
        }
        return m_pastePhase == 2 && n == m_pasteBaseCount;
    }, t(5000), [this] {
        if (!m_engine->supports(QStringLiteral("edit.clipboard"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability edit.clipboard");
            return;
        }
        m_engine->setPrivateClipboard(true);  // never touch the user's system clipboard
        m_pastePhase = 0;
        m_pasteBaseCount = m_model->objectCount();
        m_propsId = m_model->idByName(QStringLiteral("Props"));  // before the paste adds a second "Props"
        m_engine->select({m_propsId});
        QTimer::singleShot(100, this, [this] { m_engine->copySelection(false); });
    });
    // Cut whose delete fails (the object vanished first): reported as an error, the copy stays on the clipboard.
    step("a cut whose delete fails reports an error and keeps the clipboard", [this] {
        if (m_skipCurrent) return true;
        if (m_cutPhase == 1 && m_engine->messageLevel() == QLatin1String("error") &&
            m_engine->lastMessage().contains(QLatin1String("could not remove")) && m_engine->clipboardHasObjects()) {
            m_engine->undo();  // restores the object deleted underneath the cut
            m_cutPhase = 2;
        }
        return m_cutPhase == 2 && m_model->objectCount() == m_pasteBaseCount;
    }, t(5000), [this] {
        if (!m_engine->supports(QStringLiteral("edit.clipboard"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability edit.clipboard");
            return;
        }
        m_cutPhase = 0;
        m_pasteBaseCount = m_model->objectCount();
        const double sphere = m_model->idByName(QStringLiteral("Sphere"));
        m_engine->select({sphere});
        QTimer::singleShot(100, this, [this, sphere] {
            m_engine->copySelection(true);
            // Queued behind the copy and ahead of the cut's own delete, which then finds nothing to remove.
            m_engine->call(QStringLiteral("object.delete"), {{"ids", QJsonArray{sphere}}});
            m_cutPhase = 1;
        });
    });
    // Undo history panel: lists the engine's undo stack; clicking an earlier row jumps back several steps.
    step("history panel lists edits and jumps several steps back and forward", [this, obj] {
        if (m_skipCurrent) return true;
        auto row = [this](int i) -> QQuickItem* {
            QQuickItem* hit = nullptr;
            const QString want = QStringLiteral("historyRow:%1").arg(i);
            std::function<void(QQuickItem*)> walk = [&](QQuickItem* it) {
                if (!hit && it->objectName() == want && it->isVisible()) hit = it;
                for (QQuickItem* c : it->childItems()) walk(c);
            };
            // Popups live in the window's overlay.
            if (m_window) walk(m_window->contentItem()->parentItem() ? m_window->contentItem()->parentItem() : m_window->contentItem());
            return hit;
        };
        const QString name = obj(m_heroId).value("name").toString();
        QObject* popup = nullptr;
        if (m_window) popup = m_window->findChild<QObject*>(QStringLiteral("historyPopup"));
        const int pos = popup ? popup->property("position").toInt() : -1;
        const int n = popup ? popup->property("entries").toList().size() : 0;
        if (m_historyPhase == 0 && n >= 2 && name == QLatin1String("Hist 2")) {
            if (QQuickItem* r = row(pos - 2)) {  // two steps back: before both renames
                m_historyStart = pos - 2;
                const QPointF c = r->mapToScene(QPointF(r->width() / 2, r->height() / 2));
                QMouseEvent press(QEvent::MouseButtonPress, c, m_window->mapToGlobal(c), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, c, m_window->mapToGlobal(c), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(m_window, &press);
                QCoreApplication::sendEvent(m_window, &release);
                m_historyPhase = 1;
            }
        } else if (m_historyPhase == 1 && name == m_historyName) {
            if (QQuickItem* r = row(pos + 2)) {  // forward again to the newest edit
                const QPointF c = r->mapToScene(QPointF(r->width() / 2, r->height() / 2));
                QMouseEvent press(QEvent::MouseButtonPress, c, m_window->mapToGlobal(c), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, c, m_window->mapToGlobal(c), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(m_window, &press);
                QCoreApplication::sendEvent(m_window, &release);
                m_historyPhase = 2;
            }
        }
        return m_historyPhase == 2 && name == QLatin1String("Hist 2");
    }, t(5000), [this, findItem] {
        if (!m_engine->supports(QStringLiteral("edit.history"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability edit.history");
            return;
        }
        m_historyPhase = 0;
        m_historyName = m_model->object(m_heroId).value("name").toString();
        m_engine->updateObject(m_heroId, {{"name", QStringLiteral("Hist 1")}});
        m_engine->updateObject(m_heroId, {{"name", QStringLiteral("Hist 2")}});
        QObject* shell = findItem("editorShell");
        QTimer::singleShot(150, this, [shell] { if (shell) QMetaObject::invokeMethod(shell, "toggleHistory"); });
    });
    // Back to where the history stood before the renames, so later steps undo the edit they expect.
    step("close the history panel and return to the earlier position", [this, obj] {
        return obj(m_heroId).value("name").toString() == m_historyName && m_engine->canRedo();
    }, t(3000), [this, findItem] {
        if (QObject* shell = findItem("editorShell")) QMetaObject::invokeMethod(shell, "toggleHistory");
        m_engine->call(QStringLiteral("edit.goto"), {{"position", m_historyStart}});
    });
    step("continuous rendering, light theme screenshot", [this] {
        return m_viewport->frameCount() >= m_startFrames + 120;
    }, t(15000), [this] {
        m_settings->setThemeMode(QStringLiteral("light"));
        m_viewport->setContinuous(true);
        m_startFrames = m_viewport->frameCount();
    });
    step("record frame metrics", [] { return true; }, t(1000), [this] {
        m_metrics["viewportFrameMs"] = m_viewport->frameMs();
        m_metrics["viewportFps"] = m_viewport->fps();
        m_metrics["viewportCpuRenderMs"] = m_viewport->cpuRenderMs();
        m_metrics["viewportGpuMs"] = m_viewport->gpuMs();
        m_metrics["drawCalls"] = m_viewport->drawCalls();
        m_metrics["rhiBackend"] = m_viewport->backendName();
        if (!shoot(QStringLiteral("world-light.png"))) fail("record frame metrics", "screenshot failed");
    });
    // The 0.6 report measured 12.9-15.7 ms per request while the viewport rendered continuously
    // (replies were read on the vsync-paced GUI thread). Measure it again under the same condition.
    step("engine round trips during continuous rendering", [this] { return m_liveCalls >= 60; }, t(15000),
         [this] { liveRoundTrip(); });
    step("record live round-trip metrics", [] { return true; }, t(1000), [this] {
        m_metrics["liveRenderRoundTripAvgMs"] = m_liveRtt / std::max(1, m_liveCalls);
        m_metrics["liveRenderRoundTripMaxMs"] = m_liveRttMax;
        m_metrics["liveRenderGuiDeliveryAvgMs"] = m_liveDelivery / std::max(1, m_liveCalls);
        m_metrics["liveRenderGuiDeliveryMaxMs"] = m_liveDeliveryMax;
        m_metrics["liveRenderSamples"] = m_liveCalls;
    });
    // Engines that render the viewport themselves (capability viewport.frames, e.g. Unreal) must have their
    // frames on screen, not Studio's own renderer.
    step("viewport displays engine-rendered frames", [this] {
        return m_skipCurrent || (m_viewport->showingEngineFrames() && m_viewport->engineFrameCount() >= 1);
    }, t(10000), [this] {
        if (!m_engine->capabilities().contains(QStringLiteral("viewport.frames"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability viewport.frames (Studio renders)");
        }
    });
    step("record engine frame metrics", [] { return true; }, t(1000), [this] {
        m_metrics["engineFrames"] = m_viewport->engineFrameCount();
        m_metrics["engineFrameLatencyMs"] = m_viewport->engineFrameLatencyMs();
        m_metrics["engineGpuMs"] = m_viewport->engineGpuMs();
        m_metrics["engineFps"] = m_viewport->engineFps();
        m_metrics["engineFrameSize"] = m_viewport->engineFrameSize();
    });
    step("drag in the viewport orbits the camera and new frames arrive", [this] {
        const bool moved = m_viewport->cameraState() != m_cameraBefore;
        const double now = m_engine->capabilities().contains(QStringLiteral("viewport.frames"))
                               ? m_viewport->engineFrameCount() : m_viewport->frameCount();
        return moved && now >= m_framesBefore + 5;
    }, t(10000), [this] {
        m_cameraBefore = m_viewport->cameraState();
        m_framesBefore = m_engine->capabilities().contains(QStringLiteral("viewport.frames"))
                             ? m_viewport->engineFrameCount() : m_viewport->frameCount();
        dragViewport(0.45, 0.5, 0.6, 0.45);
    });
    step("orbit screenshot", [] { return true; }, t(1000), [this] { shoot(QStringLiteral("world-orbit.png")); });
    step("create a cube through the engine", [this] { return m_model->objectCount() == m_countBefore + 1; }, t(5000),
         [this] {
             m_countBefore = m_model->objectCount();
             m_engine->createPrimitive(QStringLiteral("cube"));
         });
    step("undo removes the created cube", [this] { return m_model->objectCount() == m_countBefore; }, t(5000),
         [this] { m_engine->undo(); });
    step("dark theme renders", [this] { return m_viewport->frameCount() >= m_startFrames + 30; }, t(10000), [this] {
        m_settings->setThemeMode(QStringLiteral("dark"));
        m_startFrames = m_viewport->frameCount();
    });
    step("dark screenshot", [] { return true; }, t(1000), [this] {
        if (!shoot(QStringLiteral("world-dark.png"))) fail("dark screenshot", "screenshot failed");
    });
    step("undo reverts last edit (engine-side)", [obj, this] {
        return near3(obj(m_heroId).value("position"), 2.5, m_heroY, m_heroZ);
    }, t(5000), [this] { m_engine->undo(); });
    step("redo re-applies it", [obj, this] { return near3(obj(m_heroId).value("position"), 2.5, m_heroY, -1.5); }, t(5000),
         [this] { m_engine->redo(); });
    // World Builder is a TITAN extension feature (capability "worldbuilder.preview"). Engines that do not
    // advertise it (the Unreal bridge today) get these steps reported as SKIP, never as passed.
    auto noWorldBuilder = [this] {
        m_skipCurrent = true;
        m_skipWhy = QStringLiteral("engine does not advertise capability worldbuilder.preview");
    };
    step("world builder preview renders ghosts", [this] { return m_skipCurrent || m_viewport->previewCount() > 0; },
         t(5000), [this, noWorldBuilder] {
        baseCount = m_model->objectCount();
        proposed = 0;
        if (!worldBuilder()) return noWorldBuilder();
        QJsonObject recipe{{"seed", 2026}, {"count", 18}, {"radius", 10}, {"minSpacing", 1.0}, {"shape", "cylinder"}};
        m_engine->call(QStringLiteral("worldbuilder.preview"),
                       {{"extension", "titan.procedural.scatter"}, {"recipe", recipe}},
                       [this](const QJsonObject& r, const QJsonObject& e) {
                           if (!e.isEmpty()) return fail("world builder preview renders ghosts", e.value("message").toString());
                           proposalId = r.value("proposalId").toString();
                           proposed = r.value("objects").toArray().size();
                           m_viewport->setPreview(r.value("objects").toArray().toVariantList());
                       });
    });
    step("preview screenshot", [] { return true; }, t(1000), [this, noWorldBuilder] {
        if (!worldBuilder()) return noWorldBuilder();
        shoot(QStringLiteral("world-preview.png"));
    });
    step("approve commits one undoable transaction", [this] {
        return m_skipCurrent || (m_model->objectCount() == baseCount + proposed + 1 && m_engine->recipes().size() == 1);
    }, t(5000), [this, noWorldBuilder] {
        if (!worldBuilder()) return noWorldBuilder();
        m_viewport->clearPreview();
        m_engine->call(QStringLiteral("worldbuilder.approve"), {{"proposalId", proposalId}});
    });
    // Asset pipeline through the real UI: files in <project>/Assets, the Assets workspace imports and lists them.
    step("assets workspace imports and lists project assets", [this, findItem] {
        if (m_skipCurrent) return true;
        QObject* list = findItem("assetsList");
        return list && list->property("count").toInt() == 4;
    }, t(10000), [this, findItem] {
        if (!m_engine->capabilities().contains(QStringLiteral("assets"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability assets");
            return;
        }
        const QDir assets(QDir(m_projectDir).filePath(QStringLiteral("Assets")));
        QDir().mkpath(assets.filePath(QStringLiteral("Audio")));
        QDir().mkpath(assets.filePath(QStringLiteral("Models")));
        auto le = [](quint32 v, int n) { QByteArray b; for (int i = 0; i < n; ++i) b.append(char(v >> (8 * i))); return b; };
        const QByteArray fmt = le(1, 2) + le(1, 2) + le(22050, 4) + le(44100, 4) + le(2, 2) + le(16, 2);
        const QByteArray data(2000, '\0');
        const QByteArray body = "WAVE" + QByteArray("fmt ") + le(16, 4) + fmt + "data" + le(quint32(data.size()), 4) + data;
        auto write = [&](const QString& rel, const QByteArray& bytes) {
            QFile f(assets.filePath(rel));
            if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        };
        write(QStringLiteral("Audio/click.wav"), "RIFF" + le(quint32(body.size()), 4) + body);
        write(QStringLiteral("Models/tri.bin"), QByteArray(36, '\1'));
        QDir().mkpath(assets.filePath(QStringLiteral("Textures")));
        QImage tex(64, 32, QImage::Format_RGB32);
        tex.fill(QColor(200, 90, 40));
        tex.save(assets.filePath(QStringLiteral("Textures/brick wall.png")));
        write(QStringLiteral("Models/tri.gltf"), R"({"asset":{"version":"2.0"},"buffers":[{"uri":"tri.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"meshes":[{"primitives":[]}]})");
        const QVariantList ws = m_window->property("workspaces").toList();
        for (int i = 0; i < ws.size(); ++i)
            if (ws[i].toMap().value(QStringLiteral("key")).toString() == QLatin1String("Assets")) m_settings->setWorkspace(i);
        QTimer::singleShot(200, this, [findItem] {
            if (QObject* w = findItem("assetsWorkspace")) QMetaObject::invokeMethod(w, "refresh");
        });
    });
    step("texture filter shows a loaded thumbnail", [this, findItem] {
        if (m_skipCurrent) return true;
        QObject* list = findItem("assetsList");
        QObject* thumb = findItem("assetThumbnail");
        return list && list->property("count").toInt() == 1 && thumb && thumb->property("status").toInt() == 1;  // Image.Ready
    }, t(5000), [this, findItem] {
        if (!m_engine->capabilities().contains(QStringLiteral("assets"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability assets");
            return;
        }
        if (QObject* w = findItem("assetsWorkspace")) w->setProperty("kindIndex", 1);  // Textures
    });
    step("assets screenshot", [] { return true; }, t(1000), [this, findItem] {
        if (m_engine->capabilities().contains(QStringLiteral("assets"))) shoot(QStringLiteral("assets.png"));
        if (QObject* w = findItem("assetsWorkspace")) w->setProperty("kindIndex", 0);
        m_settings->setWorkspace(0);
    });
    // Prefabs through the UI layer: save the Hero Cube as a prefab, place it from the Assets workspace, undo.
    step("prefab created from the selection and placed from the Assets workspace", [this] {
        return m_skipCurrent || (m_prefabObjects > 0 && m_model->objectCount() == m_countBefore + m_prefabObjects);
    }, t(10000), [this, findItem] {
        if (!m_engine->capabilities().contains(QStringLiteral("prefabs"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability prefabs");
            return;
        }
        m_countBefore = m_model->objectCount();
        m_prefabObjects = 0;
        m_engine->call(QStringLiteral("prefab.create"), {{"id", m_heroId}, {"name", "Smoke Prop"}, {"overwrite", true}},
                       [this, findItem](const QJsonObject& r, const QJsonObject& e) {
                           if (!e.isEmpty()) return fail("prefab created from the selection and placed from the Assets workspace", e.value("message").toString());
                           m_prefabObjects = r.value("objects").toInt();
                           const QVariantList ws = m_window->property("workspaces").toList();
                           for (int i = 0; i < ws.size(); ++i)
                               if (ws[i].toMap().value(QStringLiteral("key")).toString() == QLatin1String("Assets"))
                                   m_settings->setWorkspace(i);
                           const QString path = r.value("path").toString();
                           QTimer::singleShot(200, this, [findItem, path] {
                               if (QObject* w = findItem("assetsWorkspace"))
                                   QMetaObject::invokeMethod(w, "place", Q_ARG(QVariant, QVariant(path)));
                           });
                       });
    });
    step("undo removes the placed prefab instance", [this] {
        return m_skipCurrent || m_model->objectCount() == m_countBefore;
    }, t(5000), [this] {
        if (!m_engine->capabilities().contains(QStringLiteral("prefabs"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability prefabs");
            return;
        }
        m_settings->setWorkspace(0);
        m_engine->undo();
    });
    // Scene editing through the editor's own commands (the functions behind Cmd+D / Cmd+G).
    step("duplicate command copies the selection and selects the copy", [this] {
        return m_skipCurrent || (m_model->objectCount() == m_countBefore + 1 && m_engine->selection().size() == 1 &&
                                 m_engine->selection().value(0).toDouble() != m_heroId);
    }, t(5000), [this] {
        if (!m_engine->supports(QStringLiteral("edit.duplicate"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability edit.duplicate");
            return;
        }
        m_countBefore = m_model->objectCount();
        m_engine->select({m_heroId});
        QTimer::singleShot(200, this, [this] {
            QMetaObject::invokeMethod(m_window, "editSelection", Q_ARG(QVariant, QVariant(QStringLiteral("object.duplicate"))));
        });
    });
    step("group command wraps the copy; undo twice restores the scene", [this] {
        if (m_skipCurrent) return true;
        if (m_groupPhase == 0 && m_model->objectCount() == m_countBefore + 2) {  // group created
            m_engine->undo();
            m_groupPhase = 1;
        } else if (m_groupPhase == 1 && m_model->objectCount() == m_countBefore + 1) {
            m_engine->undo();
            m_groupPhase = 2;
        }
        return m_groupPhase == 2 && m_model->objectCount() == m_countBefore;
    }, t(8000), [this] {
        if (!m_engine->supports(QStringLiteral("edit.group"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability edit.group");
            return;
        }
        m_groupPhase = 0;
        QMetaObject::invokeMethod(m_window, "editSelection", Q_ARG(QVariant, QVariant(QStringLiteral("object.group"))));
    });
    step("save project", [this] { return !m_engine->dirty(); }, t(5000), [this] { m_engine->saveProject(); });
    step("close project", [this] { return !m_engine->projectOpen(); }, t(5000), [this] { m_engine->closeProject(); });
    step(m_unreal ? QStringLiteral("reopen restores edited transform (world builder skipped)")
                  : QStringLiteral("reopen restores edited transform and recipe"), [obj, this] {
        if (!m_engine->projectOpen() || !near3(obj(m_heroId).value("position"), 2.5, m_heroY, -1.5)) return false;
        if (!worldBuilder()) return m_model->objectCount() == baseCount;  // nothing added, nothing lost
        return m_engine->recipes().size() == 1 && m_model->objectCount() == baseCount + proposed + 1;
    }, t(10000), [this] { m_engine->openProject(m_projectDir); });
    step("disconnect and reconnect restores the project", [obj, this] {
        return m_engine->connected() && m_engine->projectOpen() &&
               near3(obj(m_heroId).value("position"), 2.5, m_heroY, -1.5);
    }, m_unreal ? 600000 : 15000, [this] { m_engine->restart(); });
    // Crash recovery through the UI: unsaved move + recovery point, engine restarted without a clean close,
    // the banner offers the recovery point and "Recover" restores the move as an unsaved, undoable edit.
    step("recovery banner restores unsaved work after an engine restart", [this, obj, findItem] {
        if (m_skipCurrent) return true;
        // Phase 0: banner shown -> hide the recovery file so the engine rejects Recover, click it.
        // Phase 1: the failure was reported -> the banner must still offer recovery; restore the file, retry.
        // Phase 2: the retry restores the unsaved move.
        const QString file = QDir(m_projectDir).filePath(QStringLiteral("Saved/Autosave.json"));
        QObject* restore = findItem("recoveryRestore");
        if (m_recoverPhase == 0 && restore) {
            QFile::rename(file, file + QStringLiteral(".held"));
            QMetaObject::invokeMethod(restore, "clicked");
            m_recoverPhase = 1;
        } else if (m_recoverPhase == 1 && m_engine->lastMessage().startsWith(QStringLiteral("Recovery failed"))) {
            if (!restore || !m_engine->recovery().value(QStringLiteral("available")).toBool()) {
                fail("recovery banner restores unsaved work after an engine restart",
                     "a failed Recover hid the recovery offer");
                return false;
            }
            QFile::rename(file + QStringLiteral(".held"), file);
            QMetaObject::invokeMethod(restore, "clicked");
            m_recoverPhase = 2;
        }
        return m_recoverPhase == 2 && m_engine->dirty() && near3(obj(m_heroId).value("position"), 6.0, m_heroY, -1.5);
    }, m_unreal ? 600000 : 20000, [this] {
        if (!m_engine->capabilities().contains(QStringLiteral("recovery"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability recovery");
            return;
        }
        m_recoverPhase = 0;
        m_engine->updateObject(m_heroId, {{"transform", QVariantMap{{"position", QVariantList{6.0, m_heroY, -1.5}}}}});
        QTimer::singleShot(300, this, [this] {
            m_engine->call(QStringLiteral("project.autosave"), {}, [this](const QJsonObject&, const QJsonObject&) {
                m_engine->restart();  // stops the engine without project.close/save: the recovery point stays
            });
        });
    });
    step("undo reverts the recovered edit", [this, obj] {
        return m_skipCurrent || near3(obj(m_heroId).value("position"), 2.5, m_heroY, -1.5);
    }, t(5000), [this] {
        if (!m_engine->capabilities().contains(QStringLiteral("recovery"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability recovery");
            return;
        }
        m_engine->undo();
    });
    step("final screenshot", [this] { return m_viewport && m_viewport->frameCount() > m_startFrames + 10; }, t(5000),
         [this] { m_startFrames = m_viewport ? m_viewport->frameCount() : 0; });
    step("final capture", [] { return true; }, t(1000), [this] { shoot(QStringLiteral("world-reopened.png")); });
    // New Project dialog with a scene template, through the real dialog (last: it replaces the open project).
    step("new project dialog lists the engine's templates", [this] {
        if (m_skipCurrent) return true;
        QObject* dlg = m_window->findChild<QObject*>(QStringLiteral("newProjectDialog"));
        return dlg && dlg->property("opened").toBool() && dlg->property("templates").toList().size() >= 3;
    }, t(5000), [this] {
        if (!m_engine->capabilities().contains(QStringLiteral("templates"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability templates");
            return;
        }
        m_engine->saveProject();  // creating a project must not discard unsaved work
        if (QObject* dlg = m_window->findChild<QObject*>(QStringLiteral("newProjectDialog"))) QMetaObject::invokeMethod(dlg, "open");
    });
    step("dialog creates a project from the Ground and Light template", [this] {
        return m_skipCurrent || (m_engine->projectOpen() && m_engine->projectPath().endsWith(QStringLiteral("Untitled World")) &&
                                 m_model->objectCount() == 3 && m_model->idByName(QStringLiteral("Hero Cube")) == 0);
    }, t(10000), [this, findItem] {
        if (!m_engine->capabilities().contains(QStringLiteral("templates"))) {
            m_skipCurrent = true;
            m_skipWhy = QStringLiteral("engine does not advertise capability templates");
            return;
        }
        QObject* dlg = m_window->findChild<QObject*>(QStringLiteral("newProjectDialog"));
        const QString location = QDir(m_projectDir).absolutePath() + QStringLiteral("-template");
        QDir(location).removeRecursively();
        QDir().mkpath(location);
        dlg->setProperty("location", location);
        const QVariantList tl = dlg->property("templates").toList();
        int index = -1;
        for (int i = 0; i < tl.size(); ++i)
            if (tl[i].toMap().value(QStringLiteral("name")).toString() == QLatin1String("Ground and Light")) index = i;
        QObject* box = findItem("templateBox");
        if (!box || index < 0) return fail("dialog creates a project from the Ground and Light template", "template picker not found");
        box->setProperty("currentIndex", index);
        QMetaObject::invokeMethod(dlg, "create");
    });
    next();
}

void SmokeTest::next() {
    ++m_index;
    if (m_index >= m_steps.size()) return finish(true);
    m_stepTimer.start();
    const Step& s = m_steps[m_index];
    std::fprintf(stderr, "[smoke] %s\n", qPrintable(s.name));
    if (s.action) s.action();
    if (m_index < 0) return;  // a failing action already finished the run
    auto* poll = new QTimer(this);
    poll->setInterval(20);
    const int idx = m_index;
    connect(poll, &QTimer::timeout, this, [this, poll, idx] {
        if (idx != m_index) {
            poll->deleteLater();
            return;
        }
        const Step& st = m_steps[idx];
        if (st.until()) {
            poll->deleteLater();
            if (m_skipCurrent) {
                m_skipCurrent = false;
                skip(st.name, m_skipWhy);
            } else {
                pass(st.name, double(m_stepTimer.nsecsElapsed()) / 1e6);
            }
            next();
        } else if (m_stepTimer.elapsed() > st.timeoutMs) {
            poll->deleteLater();
            QString diag;
            if (m_window)
                diag = QStringLiteral(" [window exposed=%1 visible=%2 active=%3 state=%4]")
                           .arg(m_window->isExposed()).arg(m_window->isVisible()).arg(m_window->isActive())
                           .arg(int(m_window->windowStates()));
            if (m_viewport)
                diag += QStringLiteral(" [viewport visible=%1 size=%2x%3 frames=%4]")
                            .arg(m_viewport->isVisible()).arg(m_viewport->width()).arg(m_viewport->height())
                            .arg(m_viewport->frameCount());
            fail(st.name, QStringLiteral("timed out after %1 ms").arg(st.timeoutMs) + diag);
        }
    });
    poll->start();
}

void SmokeTest::pass(const QString& name, double ms, const QString& note) {
    std::printf("[ PASS ] %-55s %9.2f ms %s\n", qPrintable(name), ms, qPrintable(note));
    std::fflush(stdout);
    m_results.append(QJsonObject{{"step", name}, {"pass", true}, {"ms", ms}});
}

bool SmokeTest::worldBuilder() const {
    return m_engine && m_engine->capabilities().contains(QStringLiteral("worldbuilder.preview"));
}

void SmokeTest::skip(const QString& name, const QString& why) {
    std::printf("[ SKIP ] %-55s %s\n", qPrintable(name), qPrintable(why));
    std::fflush(stdout);
    m_skipped << name;
    m_results.append(QJsonObject{{"step", name}, {"pass", false}, {"skipped", true}, {"detail", why}});
}

void SmokeTest::fail(const QString& name, const QString& why) {
    if (m_index < 0) return;
    std::printf("[ FAIL ] %s: %s\n", qPrintable(name), qPrintable(why));
    std::fflush(stdout);
    m_results.append(QJsonObject{{"step", name}, {"pass", false}, {"detail", why}});
    m_index = -100;
    finish(false);
}

void SmokeTest::finish(bool ok) {
    m_metrics["engineConnectMs"] = m_engine->connectMs();
    m_metrics["projectOpenMs"] = m_engine->lastOpenMs();
    m_metrics["sceneSaveMs"] = m_engine->lastSaveMs();
    m_metrics["avgRoundTripMs"] = m_engine->avgRttMs();        // request written -> reply read (I/O thread)
    m_metrics["avgGuiDeliveryMs"] = m_engine->avgDeliveryMs();  // ... -> reply handled on the GUI thread
    m_metrics["totalMs"] = double(m_total.elapsed());
    QJsonObject report{{"ok", ok},
                       {"engine", m_engine->engineKind()},
                       {"capabilities", QJsonArray::fromStringList(m_engine->capabilities())},
                       {"skipped", QJsonArray::fromStringList(m_skipped)},
                       {"steps", m_results},
                       {"metrics", m_metrics},
                       {"machine", QJsonObject{{"cpu", QSysInfo::currentCpuArchitecture()},
                                               {"os", QSysInfo::prettyProductName()},
                                               {"kernel", QSysInfo::kernelVersion()},
                                               {"qt", QString::fromLatin1(qVersion())}}}};
    if (!m_reportPath.isEmpty()) {
        QDir().mkpath(QFileInfo(m_reportPath).absolutePath());
        QFile f(m_reportPath);
        if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(report).toJson());
    }
    std::printf("smoke test %s (%lld ms)%s\n", ok ? "PASSED" : "FAILED", static_cast<long long>(m_total.elapsed()),
                m_skipped.isEmpty() ? ""
                                    : qPrintable(QStringLiteral(", %1 step(s) SKIPPED (not passed): %2")
                                                     .arg(m_skipped.size()).arg(m_skipped.join(QStringLiteral("; ")))));
    std::fflush(stdout);
    if (m_settings) m_settings->setThemeMode(m_savedTheme);
    m_engine->shutdownEngine();
    QTimer::singleShot(300, qApp, [ok] { QCoreApplication::exit(ok ? 0 : 1); });
}

void SmokeTest::liveRoundTrip() {
    // Sequential requests while the viewport renders continuously.
    m_engine->call(QStringLiteral("engine.status"), {}, [this](const QJsonObject&, const QJsonObject&) {
        ++m_liveCalls;
        m_liveRtt += m_engine->lastRttMs();
        m_liveDelivery += m_engine->lastDeliveryMs();
        m_liveRttMax = std::max(m_liveRttMax, m_engine->lastRttMs());
        m_liveDeliveryMax = std::max(m_liveDeliveryMax, m_engine->lastDeliveryMs());
        if (m_liveCalls < 60) liveRoundTrip();
    });
}
