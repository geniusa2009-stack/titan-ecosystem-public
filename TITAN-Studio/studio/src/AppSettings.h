// TITAN Studio — persistent editor preferences (QSettings). Original TITAN code.
#pragma once

#include <QObject>
#include <QSettings>
#include <QVariantList>

class AppSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY changed)       // system | light | dark
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY changed)          // en | ar
    Q_PROPERTY(double textScale READ textScale WRITE setTextScale NOTIFY changed)        // 0.85 – 1.4
    Q_PROPERTY(bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY changed)
    Q_PROPERTY(QVariantList recentProjects READ recentProjects NOTIFY recentChanged)
    Q_PROPERTY(int workspace READ workspace WRITE setWorkspace NOTIFY changed)
    Q_PROPERTY(QString engineKind READ engineKind WRITE setEngineKind NOTIFY changed)  // reference | unreal
    Q_PROPERTY(double snapStep READ snapStep WRITE setSnapStep NOTIFY changed)  // viewport move grid, 0 = off

public:
    explicit AppSettings(QObject* parent = nullptr);

    QString themeMode() const;
    void setThemeMode(const QString& m);
    QString language() const;
    void setLanguage(const QString& l);
    double textScale() const;
    void setTextScale(double s);
    bool reducedMotion() const;
    void setReducedMotion(bool r);
    int workspace() const;
    void setWorkspace(int w);
    QVariantList recentProjects() const;
    QString engineKind() const;
    void setEngineKind(const QString& k);
    double snapStep() const;
    void setSnapStep(double s);

    Q_INVOKABLE void addRecent(const QString& path, const QString& name);
    Q_INVOKABLE void removeRecent(const QString& path);

signals:
    void changed();
    void recentChanged();

private:
    mutable QSettings m_settings;
};
