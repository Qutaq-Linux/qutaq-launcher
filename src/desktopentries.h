#pragma once

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QStringList>

class QFileSystemWatcher;
class QTimer;

// One application from a .desktop file (freedesktop Desktop Entry Specification).
struct DesktopEntry {
    struct Action {
        QString id, name, icon, exec;
    };

    QString id;          // file name without .desktop; subdirectories joined with '-'
    QString path;        // full path of the file
    QString name;        // localized Name
    QString genericName; // localized GenericName
    QString comment;     // localized Comment
    QString icon;        // icon name or absolute path
    QString exec;
    QString workDir;     // Path=
    QString startupWMClass;
    QStringList categories;
    QStringList keywords;  // localized Keywords
    QList<Action> actions; // [Desktop Action …]
    bool terminal = false;
    bool noDisplay = false; // not shown in menus, but can still match a window

    bool isValid() const { return !id.isEmpty(); }
};

// All installed applications from $XDG_DATA_HOME/applications and $XDG_DATA_DIRS/applications.
// Re-scans by itself when .desktop files are installed or removed.
class DesktopEntries : public QObject
{
    Q_OBJECT
public:
    static DesktopEntries *instance();

    DesktopEntry entry(const QString &id) const { return m_entries.value(id); }
    QList<DesktopEntry> all() const { return m_entries.values(); }

    // The application of a window: Wayland app_id → .desktop file (exact id, StartupWMClass,
    // reverse-DNS ids, Exec). Empty if nothing matches.
    DesktopEntry forAppId(const QString &appId) const;

    // Starts the application (or one of its [Desktop Action]s). Returns false if it could not start.
    static bool launch(const DesktopEntry &e, const QString &actionId = QString());

    static QIcon icon(const QString &iconName, const QString &fallback = QStringLiteral("application-x-executable"));

Q_SIGNALS:
    void changed();

private:
    DesktopEntries();
    void scan();
    void watchDirs();

    QHash<QString, DesktopEntry> m_entries;
    mutable QHash<QString, QString> m_appIdCache; // app_id → id ("" means no match)
    QFileSystemWatcher *m_watcher;
    QTimer *m_rescan;
};

// Exec= with field codes removed and quoting resolved: program and arguments.
QStringList parseExec(const QString &exec, const DesktopEntry &e);
