#include "desktopentries.h"

#include "settings.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLocale>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

DesktopEntries *DesktopEntries::instance()
{
    static DesktopEntries *s = new DesktopEntries;
    return s;
}

DesktopEntries::DesktopEntries()
    : m_watcher(new QFileSystemWatcher(this))
    , m_rescan(new QTimer(this))
{
    m_rescan->setSingleShot(true);
    m_rescan->setInterval(1000); // package managers install many files at once
    connect(m_rescan, &QTimer::timeout, this, [this] {
        scan();
        watchDirs();
        Q_EMIT changed();
    });
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_rescan, qOverload<>(&QTimer::start));
    scan();
    watchDirs();
}

static QStringList applicationDirs()
{
    // User directory first: its files override system ones with the same id
    return QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
}

void DesktopEntries::watchDirs()
{
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());
    for (const QString &dir : applicationDirs()) {
        if (!QFileInfo::exists(dir))
            continue;
        m_watcher->addPath(dir);
        QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext())
            m_watcher->addPath(it.next());
    }
}

// Value of a desktop-file string: \s \n \t \r \\ escapes
static QString unescape(const QString &v)
{
    QString out;
    out.reserve(v.size());
    for (int i = 0; i < v.size(); ++i) {
        if (v.at(i) == '\\' && i + 1 < v.size()) {
            const QChar c = v.at(++i);
            out += c == 's' ? QChar(' ') : c == 'n' ? QChar('\n') : c == 't' ? QChar('\t') : c == 'r' ? QChar('\r') : c;
        } else {
            out += v.at(i);
        }
    }
    return out;
}

static QStringList splitList(const QString &v)
{
    QStringList out;
    QString cur;
    for (int i = 0; i < v.size(); ++i) {
        if (v.at(i) == '\\' && i + 1 < v.size() && v.at(i + 1) == ';') {
            cur += ';';
            ++i;
        } else if (v.at(i) == ';') {
            if (!cur.isEmpty())
                out << unescape(cur);
            cur.clear();
        } else {
            cur += v.at(i);
        }
    }
    if (!cur.isEmpty())
        out << unescape(cur);
    return out;
}

// Groups of a desktop file: group → key → value (localized keys kept as "Name[ru]")
static QHash<QString, QHash<QString, QString>> readGroups(const QString &path)
{
    QHash<QString, QHash<QString, QString>> groups;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return groups;
    QTextStream ts(&f);
    QString group;
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (line.startsWith('[') && line.endsWith(']')) {
            group = line.mid(1, line.size() - 2);
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq <= 0 || group.isEmpty())
            continue;
        QHash<QString, QString> &g = groups[group];
        const QString key = line.left(eq).trimmed();
        if (!g.contains(key)) // the first occurrence wins
            g.insert(key, line.mid(eq + 1).trimmed());
    }
    return groups;
}

// Localized value: Name[ru_RU], Name[ru], Name
static QString localized(const QHash<QString, QString> &g, const QString &key)
{
    const QString loc = QLocale().name(); // ru_RU
    const QString lang = loc.section('_', 0, 0);
    for (const QString &k : {QStringLiteral("%1[%2]").arg(key, loc), QStringLiteral("%1[%2]").arg(key, lang), key}) {
        const auto it = g.constFind(k);
        if (it != g.constEnd())
            return it.value();
    }
    return {};
}

static bool isTrue(const QString &v) { return v == QLatin1String("true"); }

void DesktopEntries::scan()
{
    m_entries.clear();
    m_appIdCache.clear();
    QSet<QString> seen; // ids already decided (also hidden ones)
    const QStringList desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':', Qt::SkipEmptyParts);

    for (const QString &dir : applicationDirs()) {
        QDirIterator it(dir, {QStringLiteral("*.desktop")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QString id = QDir(dir).relativeFilePath(path);
            id.chop(8); // ".desktop"
            id.replace('/', '-');
            if (seen.contains(id))
                continue;
            seen.insert(id);

            const auto groups = readGroups(path);
            const QHash<QString, QString> g = groups.value(QStringLiteral("Desktop Entry"));
            if (g.value(QStringLiteral("Type")) != QLatin1String("Application") || isTrue(g.value(QStringLiteral("Hidden"))))
                continue;
            // NotShowIn is respected; OnlyShowIn is not: under labwc the Xfce, LXQt… tools are wanted too.
            const QStringList notIn = splitList(g.value(QStringLiteral("NotShowIn")));
            bool excluded = false;
            for (const QString &d : desktops)
                excluded = excluded || notIn.contains(d, Qt::CaseInsensitive);
            if (excluded)
                continue;
            const QString tryExec = g.value(QStringLiteral("TryExec"));
            if (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty() && !QFileInfo(tryExec).isExecutable())
                continue;

            DesktopEntry e;
            e.id = id;
            e.path = path;
            e.name = unescape(localized(g, QStringLiteral("Name")));
            e.genericName = unescape(localized(g, QStringLiteral("GenericName")));
            e.comment = unescape(localized(g, QStringLiteral("Comment")));
            e.icon = unescape(g.value(QStringLiteral("Icon")));
            e.exec = g.value(QStringLiteral("Exec"));
            e.workDir = unescape(g.value(QStringLiteral("Path")));
            e.startupWMClass = g.value(QStringLiteral("StartupWMClass"));
            e.categories = splitList(g.value(QStringLiteral("Categories")));
            e.keywords = splitList(localized(g, QStringLiteral("Keywords")));
            e.terminal = isTrue(g.value(QStringLiteral("Terminal")));
            e.noDisplay = isTrue(g.value(QStringLiteral("NoDisplay")));
            for (const QString &a : splitList(g.value(QStringLiteral("Actions")))) {
                const QHash<QString, QString> ag = groups.value(QStringLiteral("Desktop Action ") + a);
                if (ag.isEmpty())
                    continue;
                e.actions << DesktopEntry::Action{a, unescape(localized(ag, QStringLiteral("Name"))),
                                                  unescape(ag.value(QStringLiteral("Icon"))), ag.value(QStringLiteral("Exec"))};
            }
            if (e.name.isEmpty() || e.exec.isEmpty())
                continue;
            m_entries.insert(id, e);
        }
    }
}

static QString execProgram(const QString &exec)
{
    const QStringList args = QProcess::splitCommand(exec);
    QString prog = args.value(0);
    if ((prog == QLatin1String("env") || prog.endsWith(QLatin1String("/env"))) && args.size() > 1) {
        for (int i = 1; i < args.size(); ++i)
            if (!args.at(i).contains('=')) {
                prog = args.at(i);
                break;
            }
    }
    return QFileInfo(prog).fileName();
}

DesktopEntry DesktopEntries::forAppId(const QString &appId) const
{
    if (appId.isEmpty())
        return {};
    const auto cached = m_appIdCache.constFind(appId);
    if (cached != m_appIdCache.constEnd())
        return m_entries.value(cached.value());

    const QString lower = appId.toLower();
    const QString lastPart = lower.section('.', -1); // org.gnome.Nautilus → nautilus
    QString found;
    int best = 0;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        const DesktopEntry &e = it.value();
        const QString id = e.id.toLower();
        int score = 0;
        if (e.id == appId)
            score = 100;
        else if (id == lower)
            score = 90;
        else if (!e.startupWMClass.isEmpty() && e.startupWMClass.compare(appId, Qt::CaseInsensitive) == 0)
            score = 80;
        else if (id.section('.', -1) == lower || id == lastPart)
            score = 60;
        else if (execProgram(e.exec).toLower() == lower)
            score = 50;
        else if (e.name.toLower() == lower)
            score = 30;
        if (score && e.noDisplay)
            score -= 5; // prefer the visible entry (e.g. foot.desktop over foot-server.desktop)
        if (score > best) {
            best = score;
            found = e.id;
        }
    }
    m_appIdCache.insert(appId, found);
    return m_entries.value(found);
}

QStringList parseExec(const QString &exec, const DesktopEntry &e)
{
    // Field codes are expanded before splitting into arguments; %% is a literal percent.
    QString cmd;
    for (int i = 0; i < exec.size(); ++i) {
        const QChar c = exec.at(i);
        if (c != '%' || i + 1 >= exec.size()) {
            cmd += c;
            continue;
        }
        const QChar code = exec.at(++i);
        switch (code.toLatin1()) {
        case '%': cmd += '%'; break;
        case 'c': cmd += '"' + QString(e.name).replace('"', QLatin1String("\\\"")) + '"'; break;
        case 'k': cmd += '"' + e.path + '"'; break;
        case 'i':
            if (!e.icon.isEmpty())
                cmd += QStringLiteral("--icon \"%1\"").arg(e.icon);
            break;
        default: break; // %f %F %u %U and deprecated codes: no files to pass
        }
    }
    return QProcess::splitCommand(unescape(cmd));
}

// Wraps a command to run in a terminal emulator (Terminal=true)
static QStringList inTerminal(const QStringList &cmd)
{
    const QString configured = launcherSetting(QStringLiteral("launcher/terminal")).toString();
    if (!configured.isEmpty()) // e.g. "alacritty -e"
        return QProcess::splitCommand(configured) + cmd;
    if (!QStandardPaths::findExecutable(QStringLiteral("xdg-terminal-exec")).isEmpty())
        return QStringList{QStringLiteral("xdg-terminal-exec")} + cmd;
    // Each terminal takes the command differently
    struct Term { const char *name; const char *flag; bool oneString; };
    static const Term terms[] = {
        {"qterminal", "-e", true}, {"xfce4-terminal", "-x", false}, {"konsole", "-e", false},
        {"foot", nullptr, false}, {"alacritty", "-e", false}, {"kitty", nullptr, false},
        {"gnome-terminal", "--", false}, {"lxterminal", "-e", true}, {"xterm", "-e", false},
    };
    for (const Term &t : terms) {
        if (QStandardPaths::findExecutable(QLatin1String(t.name)).isEmpty())
            continue;
        QStringList out{QLatin1String(t.name)};
        if (t.flag)
            out << QLatin1String(t.flag);
        if (t.oneString) {
            QStringList quoted;
            for (const QString &a : cmd)
                quoted << (a.contains(' ') ? '"' + a + '"' : a);
            out << quoted.join(' ');
        } else {
            out << cmd;
        }
        return out;
    }
    return {};
}

bool DesktopEntries::launch(const DesktopEntry &e, const QString &actionId)
{
    QString exec = e.exec;
    for (const DesktopEntry::Action &a : e.actions)
        if (a.id == actionId)
            exec = a.exec;
    QStringList args = parseExec(exec, e);
    if (args.isEmpty())
        return false;
    if (e.terminal) {
        args = inTerminal(args);
        if (args.isEmpty()) {
            qWarning().noquote() << "no terminal emulator found for" << e.id;
            return false;
        }
    }
    const QString program = args.takeFirst();
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.setWorkingDirectory(e.workDir.isEmpty() ? QDir::homePath() : e.workDir);
    // Tell the application which desktop file started it (GIO and Qt use this)
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIO_LAUNCHED_DESKTOP_FILE"), e.path);
    // Never pass our own shell integration on: a Qt application would become a layer surface
    env.remove(QStringLiteral("QT_WAYLAND_SHELL_INTEGRATION"));
    p.setProcessEnvironment(env);
    const bool ok = p.startDetached();
    qInfo().noquote() << "launch" << e.id << (actionId.isEmpty() ? QString() : actionId) << (ok ? "ok" : "FAILED") << program << args;
    return ok;
}

QIcon DesktopEntries::icon(const QString &iconName, const QString &fallback)
{
    if (iconName.isEmpty())
        return QIcon::fromTheme(fallback);
    if (iconName.startsWith('/'))
        return QFileInfo::exists(iconName) ? QIcon(iconName) : QIcon::fromTheme(fallback);
    QIcon i = QIcon::fromTheme(iconName);
    if (!i.isNull())
        return i;
    for (const char *ext : {".png", ".svg", ".xpm"}) {
        const QString p = QStringLiteral("/usr/share/pixmaps/") + iconName + QLatin1String(ext);
        if (QFileInfo::exists(p))
            return QIcon(p);
    }
    return QIcon::fromTheme(fallback);
}
