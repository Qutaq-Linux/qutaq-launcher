#pragma once

#include "desktopentries.h"

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>

class Toplevel;
class ToplevelManager;
class QTimer;

// One icon in the launcher: a pinned application, a running one, or both.
struct LauncherItem {
    QString key;           // desktop id, or "appid:<app_id>" for windows without a .desktop file
    DesktopEntry entry;    // invalid for unmatched windows
    QString appId;         // app_id of the windows (for unmatched ones)
    bool pinned = false;
    QList<QPointer<Toplevel>> windows; // ordered by appearance
    qint64 launchedAt = -1; // ms of uptime when we launched it; -1 = not launching

    QString name() const;
    QIcon icon() const;
    QList<Toplevel *> liveWindows() const;
    bool isRunning() const { return !liveWindows().isEmpty(); }
    bool isActive() const;      // one of its windows has the focus
    bool isLaunching() const { return launchedAt >= 0; }
};

// The launcher's contents: pinned applications (launcher/favorites in launcher.conf) followed by
// running applications that are not pinned. Shared by the launchers on all monitors.
class LauncherModel : public QObject
{
    Q_OBJECT
public:
    explicit LauncherModel(ToplevelManager *toplevels, QObject *parent = nullptr);

    const QList<LauncherItem> &items() const { return m_items; }
    int indexOf(const QString &key) const;
    static qint64 uptime();

    // Click on an icon, as in Unity: start it, bring its windows to the front,
    // cycle through them, or minimize the only window of the active application.
    void activate(const QString &key);
    void launch(const QString &key, const QString &actionId = QString()); // a new instance
    void quit(const QString &key);                                        // close all its windows

    void setPinned(const QString &key, bool pinned);
    void move(const QString &key, int toIndex); // drag and drop; pins the item

Q_SIGNALS:
    void changed();

private:
    void loadFavorites();
    void saveFavorites();
    void addWindow(Toplevel *t);
    void removeWindow(Toplevel *t);
    void cleanup(); // drop items that are neither pinned nor running
    QString keyFor(Toplevel *t, DesktopEntry *entry = nullptr) const;

    ToplevelManager *m_toplevels;
    QList<LauncherItem> m_items;
    QTimer *m_launchTimer;
};
