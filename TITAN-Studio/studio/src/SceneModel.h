// TITAN Studio — read-only mirror of the engine's scene for the hierarchy, inspector and
// viewport. Updated exclusively from engine snapshots and scene.changed events.
// Original TITAN code.
#pragma once

#include "titan/math.h"
#include "titan/scene.h"

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QVariantMap>

struct StudioObject {
    quint64 id = 0;
    QString name;
    QString type;
    titan::Shape shape = titan::Shape::None;
    quint64 parent = 0;
    titan::Transform transform;
    float r = 0.8f, g = 0.8f, b = 0.8f;
    bool visible = true;
    bool locked = false;
    float intensity = 1.0f;
    QString recipeId;
};

class SceneModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int objectCount READ objectCount NOTIFY sceneDataChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // "" = all types, else an object type ("mesh", "light", "camera", "group"). Ancestors of matches stay visible.
    Q_PROPERTY(QString typeFilter READ typeFilter WRITE setTypeFilter NOTIFY filterChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        ShapeRole,
        DepthRole,
        HasChildrenRole,
        ExpandedRole,
        SelectedRole,
        ObjectVisibleRole,
        GeneratedRole,
        ObjectLockedRole,
    };

    explicit SceneModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void resetFromJson(const QJsonArray& objects);
    void applyChange(const QJsonArray& upserted, const QJsonArray& removed);
    void clear();
    void setSelection(const QSet<quint64>& ids);

    int objectCount() const { return int(m_objects.size()); }
    QString filter() const { return m_filter; }
    void setFilter(const QString& f);
    QString typeFilter() const { return m_typeFilter; }
    void setTypeFilter(const QString& t);
    bool filtering() const { return !m_filter.isEmpty() || !m_typeFilter.isEmpty(); }

    Q_INVOKABLE QVariantMap object(double id) const;
    Q_INVOKABLE void toggleExpanded(int row);
    Q_INVOKABLE int rowForId(double id) const;
    Q_INVOKABLE double idByName(const QString& name) const;
    Q_INVOKABLE QVariantList matchingIds() const;  // objects that match the filters themselves (not ancestors)

    const QHash<quint64, StudioObject>& objects() const { return m_objects; }
    const QSet<quint64>& selectedIds() const { return m_selected; }
    titan::Mat4 worldMatrix(quint64 id) const;

signals:
    void sceneDataChanged();
    void filterChanged();
    void selectionApplied();

private:
    struct Row {
        quint64 id;
        int depth;
        bool hasChildren;
    };
    static StudioObject fromJson(const QJsonObject& o);
    void rebuildRows();
    bool matchesFilter(quint64 id) const;

    QHash<quint64, StudioObject> m_objects;
    QSet<quint64> m_selected;
    QSet<quint64> m_collapsed;
    QVector<Row> m_rows;
    QHash<quint64, int> m_rowOf;
    QString m_filter;
    QString m_typeFilter;
};
