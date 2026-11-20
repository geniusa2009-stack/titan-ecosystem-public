// TITAN Studio — scene mirror implementation. Original TITAN code.
#include "SceneModel.h"

#include <QJsonObject>

#include <algorithm>
#include <functional>

namespace {
titan::Vec3 vec(const QJsonValue& v, titan::Vec3 def) {
    const QJsonArray a = v.toArray();
    if (a.size() != 3) return def;
    return {float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble())};
}
QVariantList list3(const titan::Vec3& v) { return {double(v.x), double(v.y), double(v.z)}; }
}  // namespace

StudioObject SceneModel::fromJson(const QJsonObject& o) {
    StudioObject s;
    s.id = quint64(o.value("id").toDouble());
    s.name = o.value("name").toString();
    s.type = o.value("type").toString();
    s.shape = titan::shapeFromString(o.value("shape").toString().toStdString()).value_or(titan::Shape::None);
    s.parent = quint64(o.value("parent").toDouble());
    const QJsonObject t = o.value("transform").toObject();
    s.transform.position = vec(t.value("position"), {0, 0, 0});
    s.transform.rotation = vec(t.value("rotation"), {0, 0, 0});
    s.transform.scale = vec(t.value("scale"), {1, 1, 1});
    titan::Vec3 c = vec(o.value("color"), {0.8f, 0.8f, 0.8f});
    s.r = c.x;
    s.g = c.y;
    s.b = c.z;
    s.visible = o.value("visible").toBool(true);
    s.locked = o.value("locked").toBool(false);
    s.intensity = float(o.value("intensity").toDouble(1.0));
    s.recipeId = o.value("generator").toObject().value("recipeId").toString();
    return s;
}

int SceneModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QHash<int, QByteArray> SceneModel::roleNames() const {
    return {{IdRole, "objectId"},       {NameRole, "name"},         {TypeRole, "type"},
            {ShapeRole, "shape"},       {DepthRole, "depth"},       {HasChildrenRole, "hasChildren"},
            {ExpandedRole, "expanded"}, {SelectedRole, "selected"}, {ObjectVisibleRole, "objectVisible"},
            {GeneratedRole, "generated"}, {ObjectLockedRole, "objectLocked"}};
}

QVariant SceneModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const Row& row = m_rows[index.row()];
    const StudioObject& o = m_objects[row.id];
    switch (role) {
        case IdRole: return double(o.id);
        case NameRole: return o.name;
        case TypeRole: return o.type;
        case ShapeRole: return QString::fromLatin1(titan::toString(o.shape));
        case DepthRole: return row.depth;
        case HasChildrenRole: return row.hasChildren;
        case ExpandedRole: return !m_collapsed.contains(o.id);
        case SelectedRole: return m_selected.contains(o.id);
        case ObjectVisibleRole: return o.visible;
        case GeneratedRole: return !o.recipeId.isEmpty();
        case ObjectLockedRole: return o.locked;
        default: return {};
    }
}

bool SceneModel::matchesFilter(quint64 id) const {
    const StudioObject& o = m_objects[id];
    return (m_typeFilter.isEmpty() || o.type == m_typeFilter) &&
           (m_filter.isEmpty() || o.name.contains(m_filter, Qt::CaseInsensitive));
}

QVariantList SceneModel::matchingIds() const {
    QVariantList out;
    for (const Row& r : m_rows)
        if (matchesFilter(r.id)) out.append(double(r.id));
    return out;
}

void SceneModel::rebuildRows() {
    QHash<quint64, QVector<quint64>> children;
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        quint64 p = m_objects.contains(it->parent) ? it->parent : 0;
        children[p].append(it->id);
    }
    for (auto& v : children) std::sort(v.begin(), v.end());  // engine order: by id

    // Each subtree is evaluated once (memoized), so a filter pass is linear in the object count.
    const bool filtering = this->filtering();
    QHash<quint64, bool> memo;
    std::function<bool(quint64)> subtreeMatches = [&](quint64 id) {
        auto it = memo.constFind(id);
        if (it != memo.constEnd()) return *it;
        bool m = matchesFilter(id);
        for (quint64 c : children.value(id)) m = subtreeMatches(c) || m;
        memo.insert(id, m);
        return m;
    };
    QVector<Row> rows;
    std::function<void(quint64, int)> visit = [&](quint64 id, int depth) {
        if (filtering && !subtreeMatches(id)) return;
        const auto& kids = children.value(id);
        rows.append({id, depth, !kids.isEmpty()});
        if (!filtering && m_collapsed.contains(id)) return;
        for (quint64 c : kids) visit(c, depth + 1);
    };
    for (quint64 root : children.value(0)) visit(root, 0);
    m_rows = rows;
    m_rowOf.clear();
    for (int i = 0; i < m_rows.size(); ++i) m_rowOf.insert(m_rows[i].id, i);
}

void SceneModel::resetFromJson(const QJsonArray& objects) {
    beginResetModel();
    m_objects.clear();
    m_collapsed.clear();
    for (const auto& v : objects) {
        StudioObject o = fromJson(v.toObject());
        if (!o.recipeId.isEmpty() && o.type == QLatin1String("group")) m_collapsed.insert(o.id);  // tidy by default
        m_objects.insert(o.id, o);
    }
    rebuildRows();
    endResetModel();
    emit sceneDataChanged();
}

void SceneModel::applyChange(const QJsonArray& upserted, const QJsonArray& removed) {
    bool structural = !removed.isEmpty();
    QVector<quint64> touched;
    for (const auto& v : upserted) {
        StudioObject o = fromJson(v.toObject());
        auto it = m_objects.find(o.id);
        if (it == m_objects.end() || it->parent != o.parent || (!m_filter.isEmpty() && it->name != o.name))
            structural = true;
        if (it == m_objects.end() && !o.recipeId.isEmpty() && o.type == QLatin1String("group")) m_collapsed.insert(o.id);
        m_objects.insert(o.id, o);
        touched.append(o.id);
    }
    for (const auto& v : removed) {
        const auto id = quint64(v.toDouble());
        m_objects.remove(id);
        m_selected.remove(id);
        m_collapsed.remove(id);
    }
    if (structural) {
        beginResetModel();
        rebuildRows();
        endResetModel();
    } else {
        for (quint64 id : touched) {
            const int row = m_rowOf.value(id, -1);
            if (row >= 0) emit dataChanged(index(row), index(row));
        }
    }
    emit sceneDataChanged();
}

void SceneModel::clear() {
    beginResetModel();
    m_objects.clear();
    m_selected.clear();
    m_collapsed.clear();
    m_rows.clear();
    m_rowOf.clear();
    endResetModel();
    emit sceneDataChanged();
}

void SceneModel::setSelection(const QSet<quint64>& ids) {
    QSet<quint64> changed = (m_selected - ids) + (ids - m_selected);
    m_selected = ids;
    for (quint64 id : changed) {
        const int row = m_rowOf.value(id, -1);
        if (row >= 0) emit dataChanged(index(row), index(row), {SelectedRole});
    }
    emit selectionApplied();
}

void SceneModel::setFilter(const QString& f) {
    if (f == m_filter) return;
    beginResetModel();
    m_filter = f;
    rebuildRows();
    endResetModel();
    emit filterChanged();
}

void SceneModel::setTypeFilter(const QString& t) {
    if (t == m_typeFilter) return;
    beginResetModel();
    m_typeFilter = t;
    rebuildRows();
    endResetModel();
    emit filterChanged();
}

void SceneModel::toggleExpanded(int row) {
    if (row < 0 || row >= m_rows.size()) return;
    const quint64 id = m_rows[row].id;
    beginResetModel();
    if (!m_collapsed.remove(id)) m_collapsed.insert(id);
    rebuildRows();
    endResetModel();
}

int SceneModel::rowForId(double id) const { return m_rowOf.value(quint64(id), -1); }

double SceneModel::idByName(const QString& name) const {
    for (const auto& o : m_objects)
        if (o.name == name) return double(o.id);
    return 0;
}

QVariantMap SceneModel::object(double idv) const {
    const auto id = quint64(idv);
    auto it = m_objects.find(id);
    if (it == m_objects.end()) return {};
    const StudioObject& o = *it;
    QVariantMap m;
    m["id"] = double(o.id);
    m["name"] = o.name;
    m["type"] = o.type;
    m["shape"] = QString::fromLatin1(titan::toString(o.shape));
    m["parent"] = double(o.parent);
    m["parentName"] = m_objects.contains(o.parent) ? m_objects[o.parent].name : QString();
    m["position"] = list3(o.transform.position);
    m["rotation"] = list3(o.transform.rotation);
    m["scale"] = list3(o.transform.scale);
    m["color"] = QVariantList{double(o.r), double(o.g), double(o.b)};
    m["visible"] = o.visible;
    m["locked"] = o.locked;
    m["intensity"] = double(o.intensity);
    m["recipeId"] = o.recipeId;
    return m;
}

titan::Mat4 SceneModel::worldMatrix(quint64 id) const {
    titan::Mat4 m;
    int guard = 0;
    for (auto it = m_objects.find(id); it != m_objects.end() && guard < 1024; it = m_objects.find(it->parent), ++guard)
        m = it->transform.matrix() * m;
    return m;
}
