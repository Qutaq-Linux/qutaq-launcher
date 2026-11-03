#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWaylandClientExtensionTemplate>

#include "qwayland-ext-workspace-v1.h"

// One workspace (ext_workspace_handle_v1)
class Workspace : public QObject, public QtWayland::ext_workspace_handle_v1
{
    Q_OBJECT
public:
    explicit Workspace(::ext_workspace_handle_v1 *handle);
    ~Workspace() override;

    QString name() const { return m_name; }
    bool isActive() const { return m_state & state_active; }
    bool isUrgent() const { return m_state & state_urgent; }
    bool isHidden() const { return m_state & state_hidden; }
    bool canActivate() const { return m_capabilities & workspace_capabilities_activate; }

Q_SIGNALS:
    void removed();

protected:
    void ext_workspace_handle_v1_name(const QString &name) override { m_name = name; }
    void ext_workspace_handle_v1_state(uint32_t state) override { m_state = state; }
    void ext_workspace_handle_v1_capabilities(uint32_t capabilities) override { m_capabilities = capabilities; }
    void ext_workspace_handle_v1_removed() override;

private:
    QString m_name;
    uint32_t m_state = 0;
    uint32_t m_capabilities = 0;
};

// A group of workspaces (in labwc: one group for all monitors). Only kept to be destroyed properly.
class WorkspaceGroup : public QObject, public QtWayland::ext_workspace_group_handle_v1
{
    Q_OBJECT
public:
    explicit WorkspaceGroup(::ext_workspace_group_handle_v1 *handle)
        : QtWayland::ext_workspace_group_handle_v1(handle) {}
    ~WorkspaceGroup() override;

protected:
    void ext_workspace_group_handle_v1_removed() override { deleteLater(); }
};

// The compositor's workspaces through ext-workspace-v1 (labwc ≥ 0.9, sway, niri, …),
// for the workspace switcher in the launcher. All windows of a workspace aren't known:
// standard Wayland protocols don't tell which window is on which workspace (yet).
class WorkspaceManager : public QWaylandClientExtensionTemplate<WorkspaceManager>,
                         public QtWayland::ext_workspace_manager_v1
{
    Q_OBJECT
public:
    WorkspaceManager();
    ~WorkspaceManager() override;

    // Visible workspaces in the compositor's order
    QList<Workspace *> workspaces() const;
    int activeIndex() const; // -1 if unknown
    void activate(int index);
    void activateRelative(int delta, bool wrap); // the wheel: next / previous

Q_SIGNALS:
    void changed(); // after done: names, order, the active one

protected:
    void ext_workspace_manager_v1_workspace_group(::ext_workspace_group_handle_v1 *group) override;
    void ext_workspace_manager_v1_workspace(::ext_workspace_handle_v1 *workspace) override;
    void ext_workspace_manager_v1_done() override;
    void ext_workspace_manager_v1_finished() override;

private:
    QList<QPointer<Workspace>> m_workspaces;
};
