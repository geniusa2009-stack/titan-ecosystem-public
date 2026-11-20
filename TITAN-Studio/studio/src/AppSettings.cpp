// TITAN Studio — preferences implementation. Original TITAN code.
#include "AppSettings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

AppSettings::AppSettings(QObject* parent) : QObject(parent), m_settings(QStringLiteral("TITAN"), QStringLiteral("TITAN Studio")) {}

QString AppSettings::themeMode() const { return m_settings.value("appearance/theme", "system").toString(); }
void AppSettings::setThemeMode(const QString& m) {
    if (m == themeMode()) return;
    m_settings.setValue("appearance/theme", m);
    emit changed();
}

QString AppSettings::language() const { return m_settings.value("appearance/language", "en").toString(); }
void AppSettings::setLanguage(const QString& l) {
    if (l == language()) return;
    m_settings.setValue("appearance/language", l);
    emit changed();
}

double AppSettings::textScale() const { return m_settings.value("appearance/textScale", 1.0).toDouble(); }
void AppSettings::setTextScale(double s) {
    s = qBound(0.85, s, 1.4);
    if (qFuzzyCompare(s, textScale())) return;
    m_settings.setValue("appearance/textScale", s);
    emit changed();
}

bool AppSettings::reducedMotion() const { return m_settings.value("appearance/reducedMotion", false).toBool(); }
void AppSettings::setReducedMotion(bool r) {
    if (r == reducedMotion()) return;
    m_settings.setValue("appearance/reducedMotion", r);
    emit changed();
}

int AppSettings::workspace() const { return m_settings.value("layout/workspace", 0).toInt(); }
void AppSettings::setWorkspace(int w) {
    if (w == workspace()) return;
    m_settings.setValue("layout/workspace", w);
    emit changed();
}

QVariantList AppSettings::recentProjects() const {
    QVariantList out;
    for (const QVariant& v : m_settings.value("projects/recent").toList()) {
        QVariantMap m = v.toMap();
        m["exists"] = QFileInfo::exists(QDir(m.value("path").toString()).filePath("project.titan"));
        out.append(m);
    }
    return out;
}

void AppSettings::addRecent(const QString& path, const QString& name) {
    if (path.isEmpty()) return;
    QVariantList list = m_settings.value("projects/recent").toList();
    for (int i = list.size() - 1; i >= 0; --i)
        if (list[i].toMap().value("path").toString() == path) list.removeAt(i);
    list.prepend(QVariantMap{{"path", path}, {"name", name}, {"opened", QDateTime::currentDateTime().toString(Qt::ISODate)}});
    while (list.size() > 12) list.removeLast();
    m_settings.setValue("projects/recent", list);
    emit recentChanged();
}

void AppSettings::removeRecent(const QString& path) {
    QVariantList list = m_settings.value("projects/recent").toList();
    for (int i = list.size() - 1; i >= 0; --i)
        if (list[i].toMap().value("path").toString() == path) list.removeAt(i);
    m_settings.setValue("projects/recent", list);
    emit recentChanged();
}

double AppSettings::snapStep() const { return m_settings.value(QStringLiteral("editor/snapStep"), 0.0).toDouble(); }
void AppSettings::setSnapStep(double s) {
    s = (s > 0 && s <= 100) ? s : 0.0;  // 0 disables snapping
    if (s == snapStep()) return;
    m_settings.setValue(QStringLiteral("editor/snapStep"), s);
    emit changed();
}

QString AppSettings::engineKind() const {
    return m_settings.value(QStringLiteral("engine/kind"), QStringLiteral("reference")).toString();
}

void AppSettings::setEngineKind(const QString& k) {
    if (k == engineKind()) return;
    m_settings.setValue(QStringLiteral("engine/kind"), k == QLatin1String("unreal") ? k : QStringLiteral("reference"));
    emit changed();
}
