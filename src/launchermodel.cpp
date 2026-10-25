#include "launchermodel.h"

#include "icons.h"
#include "settings.h"
#include "toplevelmanager.h"

#include <QDebug>
#include <QTimer>

qint64 LauncherModel::uptime()
{
    static QElapsedTimer t;
    if (!t.isValid())
        t.start();
    return t.elapsed();
}

QString LauncherItem::name() const
{
    if (entry.isValid())
        return entry.name;
    const QList<Toplevel *> w = liveWindows();
    if (!w.isEmpty() && !w.first()->title().isEmpty())
        return w.first()->title();
    return appId;
}

QIcon LauncherItem::icon() const
{
    if (entry.isValid()) {
        QIcon i = DesktopEntries::icon(entry.icon, QString());
        if (!i.isNull())
            return i;
    }
    QIcon i = DesktopEntries::icon(appId, QString());
    return i.isNull() ? placeholderIcon(name()) : i;
}

QList<Toplevel *> LauncherItem::liveWindows() const
{
    QList<Toplevel *> out;
    for (const QPointer<Toplevel> &w : windows)
        if (w)
            out << w;
    return out;
}

bool LauncherItem::isActive() const
{
    for (Toplevel *w : liveWindows())
        if (w->isActivated())
            return true;
    return false;
}

// Applications pinned on first start: the first installed one of each group
static QStringList defaultFavorites()
{
    static const QList<QStringList> groups = {
        {"thunar", "org.gnome.Nautilus", "org.kde.dolphin", "pcmanfm-qt", "pcmanfm", "nemo", "caja"},
        {"firefox", "org.mozilla.firefox", "chromium", "google-chrome", "brave-browser", "org.kde.falkon", "epiphany"},
        {"libreoffice-writer"},
        {"xfce4-terminal", "org.xfce.terminal", "qterminal", "org.kde.konsole", "org.gnome.Terminal", "org.codeberg.dnkl.foot", "foot", "Alacritty", "kitty"},
        {"org.xfce.mousepad", "mousepad", "org.gnome.TextEditor", "featherpad", "org.kde.kate", "org.gnome.gedit"},
        {"xfce-settings-manager", "org.xfce.xfce4-settings-manager", "gnome-control-center", "systemsettings", "lxqt-config"},
    };
    QStringList out;
    for (const QStringList &g : groups)
        for (const QString &id : g)
            if (DesktopEntries::instance()->entry(id).isValid()) {
                out << id;
                break;
            }
    return out;
}

LauncherModel::LauncherModel(ToplevelManager *toplevels, QObject *parent)
    : QObject(parent)
    , m_toplevels(toplevels)
    , m_launchTimer(new QTimer(this))
{
    loadFavorites();

    connect(toplevels, &ToplevelManager::toplevelAdded, this, &LauncherModel::addWindow);
    connect(toplevels, &ToplevelManager::toplevelRemoved, this, &LauncherModel::removeWindow);
    connect(toplevels, &ToplevelManager::activeToplevelChanged, this, &LauncherModel::changed);
    for (Toplevel *t : toplevels->toplevels())
        addWindow(t);

    // Installed or removed applications: refresh names, icons and window matches
    connect(DesktopEntries::instance(), &DesktopEntries::changed, this, [this] {
        const QList<QPointer<Toplevel>> all = [this] {
            QList<QPointer<Toplevel>> w;
            for (const LauncherItem &i : std::as_const(m_items))
                w += i.windows;
            return w;
        }();
        m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [](const LauncherItem &i) { return !i.pinned; }),
                      m_items.end());
        for (LauncherItem &i : m_items) {
            i.windows.clear();
            if (i.entry.isValid())
                i.entry = DesktopEntries::instance()->entry(i.key);
        }
        for (const QPointer<Toplevel> &t : all)
            if (t)
                addWindow(t);
        Q_EMIT changed();
    });

    // Launch feedback (pulsing icon) ends when a window appears or after 10 seconds
    m_launchTimer->setInterval(500);
    connect(m_launchTimer, &QTimer::timeout, this, [this] {
        bool any = false, changedSomething = false;
        for (LauncherItem &i : m_items) {
            if (i.isLaunching() && uptime() - i.launchedAt > 10000) {
                i.launchedAt = -1;
                changedSomething = true;
            }
            any = any || i.isLaunching();
        }
        if (!any)
            m_launchTimer->stop();
        if (changedSomething)
            Q_EMIT changed();
    });
}

void LauncherModel::loadFavorites()
{
    QStringList ids;
    if (launcherSettings().contains(QStringLiteral("launcher/favorites")))
        ids = launcherSetting(QStringLiteral("launcher/favorites")).toStringList();
    else
        ids = defaultFavorites();
    for (QString id : ids) {
        id = id.trimmed();
        if (id.endsWith(QLatin1String(".desktop")))
            id.chop(8);
        const DesktopEntry e = DesktopEntries::instance()->entry(id);
        if (!e.isValid() || indexOf(id) >= 0)
            continue; // uninstalled: skip, but keep it in the file in case it comes back
        LauncherItem item;
        item.key = id;
        item.entry = e;
        item.pinned = true;
        m_items << item;
    }
}

void LauncherModel::saveFavorites()
{
    QStringList ids;
    for (const LauncherItem &i : std::as_const(m_items))
        if (i.pinned)
            ids << i.key;
    launcherSettingSet(QStringLiteral("launcher/favorites"), ids);
}

int LauncherModel::indexOf(const QString &key) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).key == key)
            return i;
    return -1;
}

QString LauncherModel::keyFor(Toplevel *t, DesktopEntry *entry) const
{
    const DesktopEntry e = DesktopEntries::instance()->forAppId(t->appId());
    if (entry)
        *entry = e;
    if (e.isValid())
        return e.id;
    return QStringLiteral("appid:") + (t->appId().isEmpty() ? t->title() : t->appId());
}

void LauncherModel::addWindow(Toplevel *t)
{
    if (t->hasParent())
        return; // dialogs belong to their main window
    DesktopEntry e;
    const QString key = keyFor(t, &e);
    qInfo().noquote() << "window: app_id" << t->appId() << "→" << key;
    int idx = indexOf(key);
    if (idx < 0) {
        LauncherItem item;
        item.key = key;
        item.entry = e;
        item.appId = t->appId();
        m_items << item;
        idx = m_items.size() - 1;
    }
    LauncherItem &item = m_items[idx];
    if (item.appId.isEmpty())
        item.appId = t->appId();
    if (!item.windows.contains(t))
        item.windows << t;
    item.launchedAt = -1;
    connect(t, &Toplevel::changed, this, &LauncherModel::changed, Qt::UniqueConnection);
    Q_EMIT changed();
}

void LauncherModel::removeWindow(Toplevel *t)
{
    for (LauncherItem &i : m_items)
        i.windows.removeAll(t);
    // The QPointer is cleared only after the object is deleted; remove it explicitly now
    cleanup();
    Q_EMIT changed();
}

void LauncherModel::cleanup()
{
    for (LauncherItem &i : m_items)
        i.windows.removeAll(QPointer<Toplevel>());
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
                                 [](const LauncherItem &i) { return !i.pinned && i.windows.isEmpty() && !i.isLaunching(); }),
                  m_items.end());
}

void LauncherModel::activate(const QString &key)
{
    const int idx = indexOf(key);
    if (idx < 0)
        return;
    LauncherItem &item = m_items[idx];
    QList<Toplevel *> windows = item.liveWindows();
    if (windows.isEmpty()) {
        launch(key);
        return;
    }
    // Most recently used first
    std::stable_sort(windows.begin(), windows.end(),
                     [](Toplevel *a, Toplevel *b) { return a->lastActivated() > b->lastActivated(); });

    if (!item.isActive()) {
        // Bring the application forward: its windows in their order, the last used one on top
        for (int i = windows.size() - 1; i >= 0; --i)
            if (!windows.at(i)->isMinimized() || i == 0)
                windows.at(i)->raise();
        return;
    }
    if (windows.size() > 1) {
        // Already active: switch to its least recently used window, i.e. cycle through them
        windows.last()->raise();
        return;
    }
    if (launcherSetting(QStringLiteral("launcher/minimizeOnClick"), true).toBool())
        windows.first()->minimize();
}

void LauncherModel::launch(const QString &key, const QString &actionId)
{
    const int idx = indexOf(key);
    if (idx < 0 || !m_items.at(idx).entry.isValid())
        return;
    if (DesktopEntries::launch(m_items.at(idx).entry, actionId) && actionId.isEmpty()) {
        m_items[idx].launchedAt = uptime();
        m_launchTimer->start();
        Q_EMIT changed();
    }
}

void LauncherModel::quit(const QString &key)
{
    const int idx = indexOf(key);
    if (idx < 0)
        return;
    for (Toplevel *w : m_items.at(idx).liveWindows())
        w->closeWindow();
}

void LauncherModel::setPinned(const QString &key, bool pinned)
{
    const int idx = indexOf(key);
    if (idx < 0 || !m_items.at(idx).entry.isValid())
        return;
    m_items[idx].pinned = pinned;
    if (pinned) {
        // A newly pinned item goes right after the other pinned ones
        LauncherItem item = m_items.takeAt(idx);
        int pos = 0;
        while (pos < m_items.size() && m_items.at(pos).pinned)
            ++pos;
        m_items.insert(pos, item);
    } else {
        // An unpinned running application moves below the pinned ones
        std::stable_partition(m_items.begin(), m_items.end(), [](const LauncherItem &i) { return i.pinned; });
    }
    cleanup();
    saveFavorites();
    Q_EMIT changed();
}

void LauncherModel::move(const QString &key, int toIndex)
{
    const int idx = indexOf(key);
    if (idx < 0)
        return;
    LauncherItem item = m_items.takeAt(idx);
    if (item.entry.isValid())
        item.pinned = true; // like Unity: dragging an icon into place keeps it in the launcher
    toIndex = qBound(0, toIndex, int(m_items.size()));
    m_items.insert(toIndex, item);
    // Pinned items stay together at the top
    std::stable_partition(m_items.begin(), m_items.end(), [](const LauncherItem &i) { return i.pinned; });
    saveFavorites();
    Q_EMIT changed();
}
