#include "launcherview.h"

#include "appmenu.h"
#include "devices.h"
#include "desktopentries.h"
#include "icons.h"
#include "launchermodel.h"
#include "settings.h"
#include "trash.h"

#include <LayerShellQt/Window>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QToolTip>
#include <QWindow>
#include <QtMath>

bool LauncherView::s_dashVisible = false;

void LauncherView::setDashVisible(bool visible)
{
    s_dashVisible = visible;
}

int LauncherView::barWidth()
{
    const int tile = qBound(24, launcherSetting(QStringLiteral("launcher/iconSize"), 48).toInt(), 96);
    return tile + 16;
}

LauncherView::LauncherView(LauncherModel *model, Devices *devices, Trash *trash, AppMenu *appMenu, QScreen *screen)
    : m_model(model)
    , m_devices(devices)
    , m_trash(trash)
    , m_appMenu(appMenu)
    , m_screen(screen)
    , m_pulse(new QTimer(this))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlag(Qt::FramelessWindowHint);
    setMouseTracking(true);
    setFixedWidth(barWidth());
    resize(barWidth(), screen->geometry().height());

    connect(model, &LauncherModel::changed, this, [this] {
        bool launching = false;
        for (const LauncherItem &i : m_model->items())
            launching = launching || i.isLaunching();
        if (launching && !m_pulse->isActive())
            m_pulse->start();
        else if (!launching)
            m_pulse->stop();
        // A newly activated (or newly started) application that is scrolled out of view: show it
        const QList<LauncherItem> &items = m_model->items();
        for (int i = 0; i < items.size(); ++i) {
            if (!items.at(i).isActive() || items.at(i).key == m_activeKey)
                continue;
            m_activeKey = items.at(i).key;
            const QRect r = tileRect(ItemSlot, i), area = itemsArea();
            if (r.top() < area.top())
                scrollBy(r.top() - area.top() - kSpacing / 2);
            else if (r.bottom() > area.bottom())
                scrollBy(r.bottom() - area.bottom() + kSpacing / 2);
        }
        scrollBy(0); // icons removed: clamp
        update();
    });
    connect(trash, &Trash::changed, this, qOverload<>(&QWidget::update));
    connect(devices, &Devices::changed, this, [this] {
        scrollBy(0);
        update();
    });
    m_pulse->setInterval(40);
    connect(m_pulse, &QTimer::timeout, this, qOverload<>(&QWidget::update));

    // Safety net: the panel may re-create its surface at any moment (output changes, restarts)
    auto *placement = new QTimer(this);
    connect(placement, &QTimer::timeout, this, &LauncherView::checkPlacement);
    placement->start(5000);
}

void LauncherView::setupLayerShell()
{
    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        // For debugging under X11/offscreen: a plain frameless window on the left edge.
        setWindowFlag(Qt::WindowStaysOnTopHint);
        setGeometry(m_screen->geometry().x(), m_screen->geometry().y(), barWidth(), m_screen->geometry().height());
        return;
    }
    setScreen(m_screen);
    winId(); // creates the QWindow while it is not shown yet
    LayerShellQt::Window *ls = LayerShellQt::Window::get(windowHandle());
    if (!ls)
        return;
    ls->setScope(QStringLiteral("qutaq-launcher"));
    ls->setLayer(LayerShellQt::Window::LayerTop);
    ls->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorTop
                                                 | LayerShellQt::Window::AnchorBottom));
    ls->setExclusiveZone(barWidth()); // windows won't go under the launcher
    // OnDemand: the search field of the applications menu needs the keyboard.
    ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
#if LSQ_VERSION_MAJOR > 6 || (LSQ_VERSION_MAJOR == 6 && LSQ_VERSION_MINOR >= 6)
    ls->setWantsToBeOnActiveScreen(false);
    ls->setScreen(m_screen);
#endif
}

bool LauncherView::topPanelRunning()
{
    // launcher/belowTopPanel: auto (detect qutaq-panel), yes (any top panel, e.g. waybar), no
    const QString mode = launcherSetting(QStringLiteral("launcher/belowTopPanel"), QStringLiteral("auto")).toString();
    if (mode == QLatin1String("yes") || mode == QLatin1String("true"))
        return true;
    if (mode == QLatin1String("no") || mode == QLatin1String("false"))
        return false;
    // qutaq-panel by its own name, by its older names, or by the global menu registrar it owns
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    for (const QString &name : topPanelServices())
        if (bus->isServiceRegistered(name))
            return true;
    return false;
}

QStringList LauncherView::topPanelServices()
{
    return {QStringLiteral("org.qutaq.Panel"), QStringLiteral("org.globalmenu.Panel"),
            QStringLiteral("com.canonical.AppMenu.Registrar")};
}

void LauncherView::checkPlacementSoon()
{
    m_placementBlocked = false;
    // The panel's surface may appear a moment after its D-Bus name or after an output change
    for (int ms : {1200, 4000})
        QTimer::singleShot(ms, this, &LauncherView::checkPlacement);
}

void LauncherView::checkPlacement()
{
    if (!isVisible() || m_placementBlocked || QGuiApplication::platformName() != QLatin1String("wayland"))
        return;
    if (height() < m_screen->geometry().height() - 1)
        return; // something (the top panel) is above us: placed correctly
    if (!topPanelRunning())
        return; // no top panel: the full height is right
    const qint64 now = LauncherModel::uptime();
    m_remaps.erase(std::remove_if(m_remaps.begin(), m_remaps.end(), [now](qint64 t) { return now - t > 30000; }), m_remaps.end());
    if (m_remaps.size() >= 3) {
        if (!m_placementBlocked)
            qInfo().noquote() << "launcher keeps the full height on" << m_screen->name()
                              << ": qutaq-panel is probably on another layer or monitor; not re-creating again";
        m_placementBlocked = true; // until the size changes or the panel restarts
        return;
    }
    m_remaps << now;
    qInfo().noquote() << "launcher took the full height above the top panel on" << m_screen->name() << ": re-creating it below the panel";
    remap();
}

void LauncherView::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    checkPlacementSoon();
}

void LauncherView::remap()
{
    // labwc places layer surfaces of one layer in the order they were created: a surface
    // created after the top panel goes below it instead of taking the full height.
    hide();
    show();
}

int LauncherView::tileSize() const
{
    // Like Unity: icons shrink to fit the screen, but not below launcher/minIconSize;
    // whatever still doesn't fit scrolls.
    const int wanted = barWidth() - 16;
    const int minimum = qBound(16, launcherSetting(QStringLiteral("launcher/minIconSize"), 32).toInt(), wanted);
    const int slotCount = m_model->items().size() + deviceCount() + 2; // + Dash button and trash
    const int fit = (height() - 2 * kPadding) / qMax(1, slotCount) - kSpacing;
    return qBound(minimum, fit, wanted);
}

QRect LauncherView::itemsArea() const
{
    const int top = kPadding + slotHeight() - kSpacing / 2;
    const int bottom = height() - kPadding - tileSize() - kSpacing / 2;
    return QRect(0, top, width(), qMax(0, bottom - top));
}

int LauncherView::maxScroll() const
{
    return qMax(0, int(m_model->items().size() + deviceCount()) * slotHeight() - itemsArea().height());
}

int LauncherView::deviceCount() const
{
    return m_devices->devices().size();
}

void LauncherView::scrollBy(int dy)
{
    const int s = qBound(0, m_scroll + dy, maxScroll());
    if (s != m_scroll) {
        m_scroll = s;
        update();
    }
}

void LauncherView::wheelEvent(QWheelEvent *e)
{
    const QPoint d = e->angleDelta();
    scrollBy(-(d.y() != 0 ? d.y() : d.x()) * slotHeight() / 120);
    m_hover = hitTest(e->position().toPoint());
    QToolTip::hideText();
}

void LauncherView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (e->oldSize().height() != height())
        qInfo().noquote() << "launcher on" << m_screen->name() << "is" << height() << "px high, the screen"
                          << m_screen->geometry().height();
    scrollBy(0); // clamp to the new size
    // An output change (new resolution, monitor re-plugged) re-creates the panel's surfaces too,
    // and labwc may then place the panel after us
    checkPlacementSoon();
}

QPoint LauncherView::slotCenter(int slot) const
{
    if (slot >= 1000)
        return tileRect(DeviceSlot, slot - 1000).center();
    return slot == -1 ? tileRect(DashSlot, 0).center() : slot == -2 ? tileRect(TrashSlot, 0).center() : tileRect(ItemSlot, slot).center();
}

QRect LauncherView::tileRect(SlotKind kind, int index) const
{
    const int t = tileSize();
    const int x = (width() - t) / 2;
    switch (kind) {
    case DashSlot:
        return QRect(x, kPadding, t, t);
    case ItemSlot:
        return QRect(x, itemsArea().top() + kSpacing / 2 + index * slotHeight() - m_scroll, t, t);
    case DeviceSlot: // after the applications, as in Unity
        return tileRect(ItemSlot, int(m_model->items().size()) + index);
    case TrashSlot:
        return QRect(x, height() - kPadding - t, t, t);
    default:
        return {};
    }
}

LauncherView::Hit LauncherView::hitTest(const QPoint &pos) const
{
    if (pos.x() < 0 || pos.x() >= width())
        return {};
    auto inside = [&](const QRect &r) { return pos.y() >= r.top() - kSpacing / 2 && pos.y() <= r.bottom() + kSpacing / 2; };
    if (inside(tileRect(DashSlot, 0)))
        return {DashSlot, -1};
    if (inside(tileRect(TrashSlot, 0)))
        return {TrashSlot, -1};
    if (!itemsArea().contains(QPoint(0, pos.y())))
        return {};
    for (int i = 0; i < m_model->items().size(); ++i)
        if (inside(tileRect(ItemSlot, i)))
            return {ItemSlot, i};
    for (int i = 0; i < deviceCount(); ++i)
        if (inside(tileRect(DeviceSlot, i)))
            return {DeviceSlot, i};
    return {};
}

int LauncherView::dropIndex(int y) const
{
    const int first = tileRect(ItemSlot, 0).center().y(); // item i is centered at first + i * slotHeight
    const int idx = qRound(double(y - first) / slotHeight());
    return qBound(0, idx, int(m_model->items().size()) - 1);
}

void LauncherView::drawTile(QPainter &p, const QRect &r, const QIcon &icon, const QColor &color, bool hover, double opacity)
{
    p.save();
    p.setOpacity(opacity);
    const qreal radius = r.width() * 0.18;
    QPainterPath path;
    path.addRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    if (color.isValid()) {
        // Unity-style tile: the icon's color, a soft vertical gradient and a glossy top
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        g.setColorAt(0, color.lighter(hover ? 135 : 118));
        g.setColorAt(1, color.darker(hover ? 105 : 118));
        p.fillPath(path, g);
        QLinearGradient shine(r.topLeft(), QPoint(r.left(), r.center().y()));
        shine.setColorAt(0, QColor(255, 255, 255, 70));
        shine.setColorAt(1, QColor(255, 255, 255, 0));
        p.fillPath(path, shine);
        p.setPen(QPen(QColor(255, 255, 255, 45), 1));
        p.drawPath(path);
    } else if (hover) {
        p.fillPath(path, QColor(255, 255, 255, 35));
    }
    const int is = qRound(r.width() * 0.74);
    const QRect ir(r.center().x() - is / 2 + 1, r.center().y() - is / 2 + 1, is, is);
    icon.paint(&p, ir);
    p.restore();
}

static void triangle(QPainter &p, qreal x, qreal cy, qreal h, bool pointsRight)
{
    QPainterPath t;
    if (pointsRight) {
        t.moveTo(x, cy - h);
        t.lineTo(x + h, cy);
        t.lineTo(x, cy + h);
    } else {
        t.moveTo(x, cy - h);
        t.lineTo(x - h, cy);
        t.lineTo(x, cy + h);
    }
    t.closeSubpath();
    p.fillPath(t, QColor(0xf0, 0xf0, 0xf2));
}

void LauncherView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), QColor(24, 25, 30, 235)); // same as the top panel
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.fillRect(QRect(width() - 1, 0, 1, height()), QColor(255, 255, 255, 28));

    // Dash button: the theme's "start-here" (often the distribution logo) or a drawn one
    static const QIcon themed = QIcon::fromTheme(launcherSetting(QStringLiteral("launcher/dashIcon"), QStringLiteral("start-here")).toString());
    const QIcon dashIcon = themed.isNull() ? dashButtonIcon() : themed;
    const bool menuOpen = m_appMenu->isVisible() || s_dashVisible;
    drawTile(p, tileRect(DashSlot, 0), dashIcon, QColor(0x3a, 0x3b, 0x44), m_hover.kind == DashSlot || menuOpen, 1.0);

    const QList<LauncherItem> &items = m_model->items();
    const QRect area = itemsArea();
    p.save();
    p.setClipRect(area);
    const int dragFrom = m_dragKey.isEmpty() ? -1 : m_model->indexOf(m_dragKey);
    const int dragTo = dragFrom >= 0 ? dropIndex(m_dragY) : -1;
    const int t = tileSize();
    for (int i = 0; i < items.size(); ++i) {
        if (i == dragFrom)
            continue;
        // Make room for the dragged icon
        int slot = i;
        if (dragFrom >= 0) {
            if (i > dragFrom && i <= dragTo) slot = i - 1;
            else if (i < dragFrom && i >= dragTo) slot = i + 1;
        }
        const LauncherItem &item = items.at(i);
        const QRect r = tileRect(ItemSlot, slot);
        const QIcon icon = item.icon();
        double opacity = 1.0;
        if (item.isLaunching()) // pulse while the application starts
            opacity = 0.55 + 0.45 * qCos((LauncherModel::uptime() - item.launchedAt) / 1000.0 * 2 * M_PI);
        drawTile(p, r, icon, tileColor(icon), m_hover.kind == ItemSlot && m_hover.index == i && m_dragKey.isEmpty(), opacity);

        // Windows: up to three arrows on the left; active application: an arrow on the right
        const int windows = qMin(3, int(item.liveWindows().size()));
        const qreal h = qMax(2.5, t / 14.0);
        for (int w = 0; w < windows; ++w)
            triangle(p, 0, r.center().y() + (w - (windows - 1) / 2.0) * (h * 2 + 2), h, true);
        if (item.isActive())
            triangle(p, width() - 1, r.center().y(), h, false);
    }
    // Drives and media: a pip on the left while mounted
    const QList<Device> devices = m_devices->devices();
    for (int i = 0; i < devices.size(); ++i) {
        const Device &d = devices.at(i);
        const QRect r = tileRect(DeviceSlot, i);
        const QIcon icon = DesktopEntries::icon(d.iconName, QStringLiteral("drive-harddisk"));
        drawTile(p, r, icon, tileColor(icon), m_hover.kind == DeviceSlot && m_hover.index == i && m_dragKey.isEmpty(), 1.0);
        if (d.isMounted())
            triangle(p, 0, r.center().y(), qMax(2.5, t / 14.0), true);
    }
    if (dragFrom >= 0) {
        const LauncherItem &item = items.at(dragFrom);
        QRect r = tileRect(ItemSlot, 0);
        r.moveCenter(QPoint(r.center().x(), m_dragY));
        const QIcon icon = item.icon();
        drawTile(p, r, icon, tileColor(icon), true, 0.9);
    }
    p.restore();

    // More icons above or below: the edge fades out, with a small chevron
    auto fade = [&](bool top) {
        const int h = qMin(slotHeight(), area.height() / 3);
        const QRect r = top ? QRect(0, area.top(), width() - 1, h) : QRect(0, area.bottom() - h + 1, width() - 1, h);
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        g.setColorAt(top ? 0 : 1, QColor(24, 25, 30, 245));
        g.setColorAt(top ? 1 : 0, QColor(24, 25, 30, 0));
        p.fillRect(r, g);
        const qreal cx = width() / 2.0, cy = top ? area.top() + 6 : area.bottom() - 6, w = 6;
        QPainterPath chevron;
        chevron.moveTo(cx - w, cy + (top ? 3 : -3));
        chevron.lineTo(cx, cy + (top ? -3 : 3));
        chevron.lineTo(cx + w, cy + (top ? 3 : -3));
        p.setPen(QPen(QColor(0xf0, 0xf0, 0xf2, 200), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(chevron);
    };
    if (m_scroll > 0)
        fade(true);
    if (m_scroll < maxScroll())
        fade(false);

    const QIcon trashIcon = QIcon::fromTheme(m_trash->isEmpty() ? QStringLiteral("user-trash") : QStringLiteral("user-trash-full"),
                                             QIcon::fromTheme(QStringLiteral("user-trash")));
    drawTile(p, tileRect(TrashSlot, 0), trashIcon, QColor(0x55, 0x56, 0x5e), m_hover.kind == TrashSlot, 1.0);
}

bool LauncherView::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const Hit h = hitTest(he->pos());
        QString tip;
        if (h.kind == DashSlot)
            tip = tr("Applications");
        else if (h.kind == TrashSlot)
            tip = tr("Trash");
        else if (h.kind == ItemSlot)
            tip = m_model->items().at(h.index).name();
        else if (h.kind == DeviceSlot)
            tip = m_devices->devices().at(h.index).name;
        if (tip.isEmpty() || !m_dragKey.isEmpty()) {
            QToolTip::hideText();
        } else {
            const QRect r = tileRect(h.kind, h.index);
            // To the right of the icon, like Unity's labels
            QToolTip::showText(mapToGlobal(QPoint(width() + 2, r.center().y() - 12)), tip, this, r);
        }
        return true;
    }
    return QWidget::event(e);
}

void LauncherView::mousePressEvent(QMouseEvent *e)
{
    QToolTip::hideText();
    m_pressed = hitTest(e->position().toPoint());
    m_pressPos = e->position().toPoint();
    if (e->button() == Qt::RightButton) {
        if (m_pressed.kind == ItemSlot)
            showQuicklist(m_pressed.index);
        else if (m_pressed.kind == TrashSlot)
            showTrashMenu();
        else if (m_pressed.kind == DeviceSlot)
            showDeviceMenu(m_pressed.index);
        else if (m_pressed.kind == DashSlot)
            toggleAppMenu();
        m_pressed = {};
    }
}

void LauncherView::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if ((e->buttons() & Qt::LeftButton) && m_pressed.kind == ItemSlot && m_dragKey.isEmpty()
        && (pos - m_pressPos).manhattanLength() > 8) {
        m_dragKey = m_model->items().at(m_pressed.index).key;
        QToolTip::hideText();
    }
    if (!m_dragKey.isEmpty()) {
        const QRect area = itemsArea();
        // Near the top or bottom edge the list scrolls under the dragged icon
        if (pos.y() < area.top() + slotHeight() / 2)
            scrollBy(-slotHeight() / 4);
        else if (pos.y() > area.bottom() - slotHeight() / 2)
            scrollBy(slotHeight() / 4);
        m_dragY = qBound(area.top() + tileSize() / 2, pos.y(), area.bottom() - tileSize() / 2);
        // Dragged far to the right: the icon will be removed from the launcher on release
        setCursor(pos.x() > width() + 40 ? Qt::ForbiddenCursor : Qt::ClosedHandCursor);
        update();
        return;
    }
    const Hit h = hitTest(pos);
    if (h.kind != m_hover.kind || h.index != m_hover.index) {
        m_hover = h;
        update();
    }
}

void LauncherView::mouseReleaseEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (!m_dragKey.isEmpty()) {
        const QString key = m_dragKey;
        m_dragKey.clear();
        unsetCursor();
        if (pos.x() > width() + 40)
            m_model->setPinned(key, false); // dragged off the launcher
        else
            m_model->move(key, dropIndex(m_dragY));
        m_pressed = {};
        update();
        return;
    }
    const Hit h = hitTest(pos);
    const Hit pressed = m_pressed;
    m_pressed = {};
    if (h.kind != pressed.kind || h.index != pressed.index)
        return;
    if (e->button() == Qt::LeftButton) {
        if (h.kind == DashSlot)
            toggleAppMenu();
        else if (h.kind == TrashSlot)
            m_trash->open();
        else if (h.kind == ItemSlot)
            m_model->activate(m_model->items().at(h.index).key);
        else if (h.kind == DeviceSlot)
            m_devices->open(m_devices->devices().at(h.index).path);
    } else if (e->button() == Qt::MiddleButton && h.kind == ItemSlot) {
        m_model->launch(m_model->items().at(h.index).key); // a new instance
    }
}

void LauncherView::leaveEvent(QEvent *)
{
    m_hover = {};
    update();
}

void LauncherView::popupMenu(QMenu *menu, const QRect &tile)
{
    // On Wayland a popup must know its parent surface to be placed next to it
    menu->winId();
    if (menu->windowHandle())
        menu->windowHandle()->setTransientParent(windowHandle());
    menu->popup(mapToGlobal(QPoint(width(), tile.top())));
}

void LauncherView::toggleAppMenu()
{
    if (m_appMenu->isVisible()) {
        m_appMenu->close();
        return;
    }
    const QString cmd = launcherSetting(QStringLiteral("launcher/dashCommand")).toString();
    if (!cmd.isEmpty()) {
        QStringList args = QProcess::splitCommand(cmd);
        QProcess::startDetached(args.takeFirst(), args);
        return;
    }
    // qutaq-dash: running (a call over D-Bus is instant) or installed (start it);
    // launcher/dash=menu keeps the simple menu instead
    if (launcherSetting(QStringLiteral("launcher/dash"), QStringLiteral("auto")).toString() != QLatin1String("menu")) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (bus.interface()->isServiceRegistered(QStringLiteral("org.qutaq.Dash"))) {
            QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.qutaq.Dash"), QStringLiteral("/Dash"),
                                                              QStringLiteral("org.qutaq.Dash"), QStringLiteral("ToggleOn"));
            msg << m_screen->name();
            bus.call(msg, QDBus::NoBlock);
            return;
        }
        if (!QStandardPaths::findExecutable(QStringLiteral("qutaq-dash")).isEmpty()) {
            QProcess::startDetached(QStringLiteral("qutaq-dash"), {QStringLiteral("--toggle"), QStringLiteral("--screen"), m_screen->name()});
            return;
        }
    }
    connect(m_appMenu, &QMenu::aboutToHide, this, qOverload<>(&QWidget::update), Qt::UniqueConnection);
    popupMenu(m_appMenu, tileRect(DashSlot, 0));
    update();
}

void LauncherView::showQuicklist(int index)
{
    const LauncherItem item = m_model->items().at(index);
    const QString key = item.key;
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    // The application's name in bold, as in Unity: starts it or switches to it
    QAction *title = menu->addAction(QString(item.name()).replace('&', QLatin1String("&&")));
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    connect(title, &QAction::triggered, this, [this, key] { m_model->activate(key); });

    if (item.entry.isValid() && !item.entry.actions.isEmpty()) {
        menu->addSeparator();
        for (const DesktopEntry::Action &a : item.entry.actions) {
            QAction *act = menu->addAction(QString(a.name).replace('&', QLatin1String("&&")));
            if (!a.icon.isEmpty())
                act->setIcon(DesktopEntries::icon(a.icon, QString()));
            const QString actionId = a.id;
            connect(act, &QAction::triggered, this, [this, key, actionId] { m_model->launch(key, actionId); });
        }
    }
    menu->addSeparator();
    if (item.entry.isValid()) {
        if (item.pinned)
            menu->addAction(tr("Unlock from Launcher"), this, [this, key] { m_model->setPinned(key, false); });
        else
            menu->addAction(tr("Lock to Launcher"), this, [this, key] { m_model->setPinned(key, true); });
    }
    if (item.isRunning())
        menu->addAction(tr("Quit"), this, [this, key] { m_model->quit(key); });
    popupMenu(menu, tileRect(ItemSlot, index));
}

void LauncherView::showDeviceMenu(int index)
{
    const Device d = m_devices->devices().at(index);
    const QString path = d.path;
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // As in Unity: the name in bold opens the volume
    QAction *title = menu->addAction(QString(d.name).replace('&', QLatin1String("&&")));
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    connect(title, &QAction::triggered, this, [this, path] { m_devices->open(path); });
    menu->addSeparator();
    if (d.hasFilesystem) {
        if (d.isMounted())
            menu->addAction(tr("Unmount"), this, [this, path] { m_devices->unmount(path); });
        else
            menu->addAction(tr("Mount"), this, [this, path] { m_devices->mount(path); });
    }
    if (d.ejectable)
        menu->addAction(tr("Eject"), this, [this, path] { m_devices->eject(path); });
    else if (d.canPowerOff)
        menu->addAction(tr("Safely Remove"), this, [this, path] { m_devices->eject(path); });
    if (d.size > 0) {
        QAction *info = menu->addAction(formatSize(d.size)
                                        + (d.isMounted() ? QStringLiteral(" · ") + d.mountPoints.first() : QString()));
        info->setEnabled(false);
    }
    popupMenu(menu, tileRect(DeviceSlot, index));
}

void LauncherView::showTrashMenu()
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction *title = menu->addAction(tr("Trash"), m_trash, &Trash::open);
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    menu->addSeparator();
    menu->addAction(tr("Open"), m_trash, &Trash::open);
    QAction *empty = menu->addAction(tr("Empty Trash…"), m_trash, &Trash::empty);
    empty->setEnabled(!m_trash->isEmpty());
    popupMenu(menu, tileRect(TrashSlot, 0));
}
