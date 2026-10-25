#pragma once

#include <QWidget>

class LauncherModel;
class Trash;
class Devices;
class AppMenu;
class QScreen;
class QTimer;

// The launcher on one monitor: a vertical bar on the left edge, as in Ubuntu Unity.
//
//   [◎]   ← applications menu (Dash later)
//   [▣]   ← pinned and running applications; ▸ on the left = open windows,
//   [▣]     ◂ on the right = the active application
//   …
//   [💾]   ← drives and media (USB sticks, cards, discs, other partitions); ▸ = mounted
//   [🗑]   ← trash, always at the bottom
//
// Left click: start / switch to / cycle windows / minimize. Middle click: new instance.
// Right click: quicklist (desktop actions, Lock/Unlock, Quit). Drag: reorder; drag out: unpin.
class LauncherView : public QWidget
{
    Q_OBJECT
public:
    LauncherView(LauncherModel *model, Devices *devices, Trash *trash, AppMenu *appMenu, QScreen *screen);
    void setupLayerShell();
    void remap(); // re-create the layer surface (to get placed below the top panel)
    // Checks a little later whether the launcher took the full height while qutaq-panel runs
    // (then the panel was placed to the right of it) and re-creates the launcher below the panel.
    void checkPlacementSoon();
    void toggleAppMenu();
    static bool topPanelRunning();
    static QStringList topPanelServices(); // D-Bus names that mean "a top panel is running"

    static int barWidth();
    QPoint slotCenter(int slot) const; // -1 = Dash button, -2 = trash, 1000+i = device i, otherwise an item (for tests)

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;
    bool event(QEvent *e) override;

private:
    enum SlotKind { None, DashSlot, ItemSlot, DeviceSlot, TrashSlot };
    struct Hit {
        SlotKind kind = None;
        int index = -1; // item index for ItemSlot, device index for DeviceSlot
    };

    int tileSize() const;          // shrinks when there are too many icons, down to launcher/minIconSize
    QRect itemsArea() const;       // between the Dash button and the trash; scrolls if icons don't fit
    int maxScroll() const;
    void scrollBy(int dy);
    void checkPlacement();
    int slotHeight() const { return tileSize() + kSpacing; }
    QRect tileRect(SlotKind kind, int index) const;
    Hit hitTest(const QPoint &pos) const;
    int dropIndex(int y) const;    // where a dragged item would land
    void showQuicklist(int index);
    void showTrashMenu();
    void showDeviceMenu(int index);
    int deviceCount() const;
    void popupMenu(QMenu *menu, const QRect &tile);
    void drawTile(QPainter &p, const QRect &r, const QIcon &icon, const QColor &color, bool hover, double opacity);

    static constexpr int kPadding = 6;
    static constexpr int kSpacing = 6;

    LauncherModel *m_model;
    Devices *m_devices;
    Trash *m_trash;
    AppMenu *m_appMenu;
    QScreen *m_screen;
    Hit m_hover;
    Hit m_pressed;
    QPoint m_pressPos;
    QString m_dragKey;   // item being dragged
    int m_dragY = 0;
    int m_scroll = 0;    // pixels the items are scrolled up
    QString m_activeKey; // to scroll a newly activated application into view
    QTimer *m_pulse;     // animation of launching applications
    QList<qint64> m_remaps; // times of recent re-creations, to never loop
    bool m_placementBlocked = false;
};
