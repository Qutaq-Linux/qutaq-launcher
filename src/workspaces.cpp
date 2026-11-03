#include "workspaces.h"

#include <QDebug>

Workspace::Workspace(::ext_workspace_handle_v1 *handle)
    : QtWayland::ext_workspace_handle_v1(handle)
{
}

Workspace::~Workspace()
{
    if (isInitialized())
        destroy();
}

void Workspace::ext_workspace_handle_v1_removed()
{
    Q_EMIT removed();
    deleteLater();
}

WorkspaceGroup::~WorkspaceGroup()
{
    if (isInitialized())
        destroy();
}

WorkspaceManager::WorkspaceManager()
    : QWaylandClientExtensionTemplate<WorkspaceManager>(1)
{
    initialize();
    if (!isActive())
        qInfo() << "ext-workspace-v1 is not available: no workspace switcher";
}

WorkspaceManager::~WorkspaceManager()
{
    qDeleteAll(m_workspaces);
    if (isInitialized())
        stop();
}

void WorkspaceManager::ext_workspace_manager_v1_workspace_group(::ext_workspace_group_handle_v1 *group)
{
    new WorkspaceGroup(group);
}

void WorkspaceManager::ext_workspace_manager_v1_workspace(::ext_workspace_handle_v1 *handle)
{
    auto *w = new Workspace(handle);
    m_workspaces.append(w);
    connect(w, &Workspace::removed, this, [this, w] { m_workspaces.removeAll(w); });
}

void WorkspaceManager::ext_workspace_manager_v1_done()
{
    m_workspaces.removeAll(nullptr);
    Q_EMIT changed();
}

void WorkspaceManager::ext_workspace_manager_v1_finished()
{
    qDeleteAll(m_workspaces);
    m_workspaces.clear();
    Q_EMIT changed();
}

QList<Workspace *> WorkspaceManager::workspaces() const
{
    QList<Workspace *> out;
    for (const QPointer<Workspace> &w : m_workspaces)
        if (w && !w->isHidden())
            out << w;
    return out;
}

int WorkspaceManager::activeIndex() const
{
    const QList<Workspace *> all = workspaces();
    for (int i = 0; i < all.size(); ++i)
        if (all.at(i)->isActive())
            return i;
    return -1;
}

void WorkspaceManager::activate(int index)
{
    const QList<Workspace *> all = workspaces();
    if (index < 0 || index >= all.size() || all.at(index)->isActive())
        return;
    all.at(index)->QtWayland::ext_workspace_handle_v1::activate();
    commit();
}

void WorkspaceManager::activateRelative(int delta, bool wrap)
{
    const int count = int(workspaces().size());
    if (count < 2)
        return;
    int i = activeIndex() + delta;
    if (wrap)
        i = (i % count + count) % count;
    activate(qBound(0, i, count - 1));
}
