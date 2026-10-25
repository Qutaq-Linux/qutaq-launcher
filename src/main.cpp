#include "appmenu.h"
#include "desktopentries.h"
#include "devices.h"
#include "icons.h"
#include "launchermodel.h"
#include "launcherview.h"
#include "settings.h"
#include "toplevelmanager.h"
#include "trash.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QCursor>
#include <QDebug>
#include <QHash>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QScreen>
#include <QTimer>
#include <QTranslator>

#include <cstring>

static const char *kService = "org.qutaq.Launcher";
static const char *kPath = "/Launcher";

// D-Bus interface for keybinds: org.qutaq.Launcher /Launcher ToggleMenu
class LauncherBus : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qutaq.Launcher")
public:
    explicit LauncherBus(QHash<QScreen *, LauncherView *> *views) : m_views(views) {}

public Q_SLOTS:
    void ToggleMenu()
    {
        // On the monitor with the mouse pointer, or the first one
        LauncherView *view = nullptr;
        for (auto it = m_views->cbegin(); it != m_views->cend(); ++it)
            if (!view || it.key()->geometry().contains(QCursor::pos()))
                view = it.value();
        if (view)
            view->toggleAppMenu();
    }

private:
    QHash<QScreen *, LauncherView *> *m_views;
};

// qutaq-launcher --toggle-menu: for a keybind in labwc's rc.xml (e.g. the Super key)
static int remoteCommand(int argc, char *argv[], const char *method)
{
    QCoreApplication app(argc, argv);
    const QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath),
                                                            QLatin1String(kService), QLatin1String(method));
    const QDBusMessage r = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    if (r.type() == QDBusMessage::ErrorMessage) {
        fprintf(stderr, "qutaq-launcher: %s\n", qPrintable(r.errorMessage()));
        return 1;
    }
    return 0;
}

// UI language: [launcher] language= in launcher.conf, otherwise the system one (LANG).
// .qm files in ~/.local/share/qutaq-launcher/translations take priority over the built-in ones.
static void setupTranslations(QApplication &app)
{
    const QString lang = launcherSetting(QStringLiteral("launcher/language")).toString().trimmed();
    QLocale locale = QLocale::system();
    if (!lang.isEmpty() && lang != QLatin1String("auto")) {
        locale = QLocale(lang);
        QLocale::setDefault(locale);
    }
    auto *qt = new QTranslator(&app); // buttons of Qt's standard dialogs
    if (qt->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(qt);
    else
        delete qt;
    QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("qutaq-launcher/translations"),
                                                 QStandardPaths::LocateDirectory);
    dirs << QStringLiteral(":/i18n");
    for (const QString &dir : std::as_const(dirs)) {
        auto *own = new QTranslator(&app);
        if (own->load(locale, QStringLiteral("qutaq-launcher"), QStringLiteral("_"), dir)) {
            app.installTranslator(own);
            qInfo().noquote() << "translation:" << own->filePath();
            return;
        }
        delete own;
    }
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--toggle-menu"))
            return remoteCommand(argc, argv, "ToggleMenu");

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qutaq-launcher"));
    app.setQuitOnLastWindowClosed(false);
    // Unity-style dark labels next to the icons
    app.setStyleSheet(QStringLiteral("QToolTip { color: #f2f2f4; background: #1c1c20; border: 1px solid #44444c;"
                                     " border-radius: 4px; padding: 4px 8px; }"));
    setupTranslations(app);
    setupIconTheme();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.interface()->isServiceRegistered(QLatin1String(kService))) {
        qWarning() << "qutaq-launcher is already running";
        return 1;
    }

    ToplevelManager toplevels;
    LauncherModel model(&toplevels);
    Trash trash;
    Devices devices;
    QObject::connect(&devices, &Devices::error, &app, [](const QString &title, const QString &message) {
        auto *box = new QMessageBox(QMessageBox::Warning, title, message);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();
    });
    AppMenu appMenu;

    QHash<QScreen *, LauncherView *> views;
    LauncherBus dbus(&views);
    bus.registerObject(QLatin1String(kPath), &dbus, QDBusConnection::ExportAllSlots);
    bus.registerService(QLatin1String(kService));

    // monitors=all (default) or primary
    const bool allMonitors = launcherSetting(QStringLiteral("launcher/monitors"), QStringLiteral("all")).toString() != QLatin1String("primary");
    auto addView = [&](QScreen *screen) {
        if (!allMonitors && screen != QGuiApplication::primaryScreen())
            return;
        auto *v = new LauncherView(&model, &devices, &trash, &appMenu, screen);
        v->setupLayerShell();
        v->show();
        views.insert(screen, v);
    };
    for (QScreen *s : app.screens())
        addView(s);
    QObject::connect(&app, &QGuiApplication::screenAdded, &app, addView);
    QObject::connect(&app, &QGuiApplication::screenRemoved, &app, [&](QScreen *s) {
        if (LauncherView *v = views.take(s))
            v->deleteLater();
    });

    // labwc stacks layer surfaces in creation order: if the top panel (qutaq-panel) starts after
    // the launcher, it would be placed to the right of it. The launcher then re-creates its
    // surfaces so that the panel keeps the full width and the launcher sits below it, as in Unity.
    // The same check runs after every size change of the launcher (resolution or monitor changes).
    auto *panelWatcher = new QDBusServiceWatcher(&app);
    panelWatcher->setConnection(bus);
    panelWatcher->setWatchMode(QDBusServiceWatcher::WatchForRegistration);
    panelWatcher->setWatchedServices(LauncherView::topPanelServices());
    QObject::connect(panelWatcher, &QDBusServiceWatcher::serviceRegistered, &app, [&] {
        for (LauncherView *v : std::as_const(views))
            v->checkPlacementSoon();
    });

    return app.exec();
}

#include "main.moc"
