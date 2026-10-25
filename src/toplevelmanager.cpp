#include "toplevelmanager.h"

#include <QGuiApplication>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QtGui/qguiapplication_platform.h>
#else
#include <qpa/qplatformnativeinterface.h> // Qt 6.4: private API (link Qt6::GuiPrivate)
#endif

// The seat for zwlr_foreign_toplevel_handle_v1.activate
static wl_seat *waylandSeat()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (auto *wl = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        return wl->seat();
    return nullptr;
#else
    return static_cast<wl_seat *>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("wl_seat"));
#endif
}

void Toplevel::raise()
{
    if (m_minimized)
        unset_minimized();
    if (wl_seat *seat = waylandSeat())
        activate(seat);
}

Toplevel::Toplevel(::zwlr_foreign_toplevel_handle_v1 *handle)
    : QtWayland::zwlr_foreign_toplevel_handle_v1(handle)
{
}

Toplevel::~Toplevel()
{
    if (isInitialized())
        destroy();
}

void Toplevel::zwlr_foreign_toplevel_handle_v1_title(const QString &title) { m_title = title; }
void Toplevel::zwlr_foreign_toplevel_handle_v1_app_id(const QString &appId) { m_appId = appId; }

void Toplevel::zwlr_foreign_toplevel_handle_v1_state(wl_array *state)
{
    m_activated = m_maximized = m_fullscreen = m_minimized = false;
    const auto *s = static_cast<const uint32_t *>(state->data);
    for (size_t i = 0; i < state->size / sizeof(uint32_t); ++i) {
        switch (s[i]) {
        case state_activated:  m_activated = true; break;
        case state_minimized:  m_minimized = true; break;
        case state_maximized:  m_maximized = true; break;
        case state_fullscreen: m_fullscreen = true; break;
        default: break;
        }
    }
}

void Toplevel::zwlr_foreign_toplevel_handle_v1_done() { Q_EMIT changed(); }

void Toplevel::zwlr_foreign_toplevel_handle_v1_parent(struct ::zwlr_foreign_toplevel_handle_v1 *parent)
{
    m_hasParent = parent != nullptr;
}

void Toplevel::zwlr_foreign_toplevel_handle_v1_closed()
{
    Q_EMIT closed();
    deleteLater();
}

// ---------------------------------------------------------------------------

static QElapsedTimer &uptime()
{
    static QElapsedTimer t;
    if (!t.isValid())
        t.start();
    return t;
}

ToplevelManager::ToplevelManager()
    : QWaylandClientExtensionTemplate<ToplevelManager>(3)
{
    uptime();
    connect(this, &QWaylandClientExtension::activeChanged, this, [this] {
        if (isActive())
            m_activatedAt = uptime().elapsed();
    });
    initialize();
    if (isActive())
        m_activatedAt = uptime().elapsed();
}

QList<Toplevel *> ToplevelManager::toplevels() const
{
    QList<Toplevel *> out;
    for (const QPointer<Toplevel> &t : m_toplevels)
        if (t && t->isReady())
            out << t;
    return out;
}

ToplevelManager::~ToplevelManager()
{
    if (isInitialized())
        stop();
}

void ToplevelManager::zwlr_foreign_toplevel_manager_v1_toplevel(::zwlr_foreign_toplevel_handle_v1 *handle)
{
    auto *t = new Toplevel(handle);
    t->setParent(this);
    t->m_sequence = ++m_nextSequence;
    // The compositor sends already open windows right after the manager is bound.
    t->m_initial = m_activatedAt >= 0 && uptime().elapsed() - m_activatedAt < 500;
    m_toplevels << t;

    connect(t, &Toplevel::changed, this, [this, t] {
        // Our own windows (if layer-shell is unavailable) don't count.
        if (t->appId() == QGuiApplication::desktopFileName() || t->appId() == QCoreApplication::applicationName())
            return;
        if (!t->m_ready) {
            t->m_ready = true;
            Q_EMIT toplevelAdded(t);
        }
        if (t->isActivated() && (m_active != t || !t->m_lastActivated))
            t->m_lastActivated = ++m_activationCounter;
        if (t->isActivated() && m_active != t) {
            m_active = t;
            Q_EMIT activeToplevelChanged();
        } else if (m_active == t) {
            Q_EMIT activeToplevelChanged(); // the title/state changed
        }
    });
    connect(t, &Toplevel::closed, this, [this, t] {
        m_toplevels.removeAll(t);
        if (t->m_ready)
            Q_EMIT toplevelRemoved(t);
        if (m_active == t) {
            m_active = nullptr;
            Q_EMIT activeToplevelChanged();
        }
    });
}

void ToplevelManager::zwlr_foreign_toplevel_manager_v1_finished()
{
    m_active = nullptr;
    Q_EMIT activeToplevelChanged();
}
