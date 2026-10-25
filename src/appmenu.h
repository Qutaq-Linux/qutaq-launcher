#pragma once

#include <QMenu>

class QLineEdit;
class QWidgetAction;
struct DesktopEntry;

// The applications menu behind the button at the top of the launcher, until Dash exists:
//
//   [ Search…                ]   ← type to filter, Enter starts the first match
//   ─────────────
//   Accessories        ▸
//   Development        ▸
//   …                             ← freedesktop main categories
//
// If launcher/dashCommand is set in launcher.conf, the button runs that command instead.
class AppMenu : public QMenu
{
    Q_OBJECT
public:
    explicit AppMenu(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void rebuild();
    void filter(const QString &text);
    QAction *appAction(const DesktopEntry &e, QMenu *into);

    QLineEdit *m_search;
    QWidgetAction *m_searchAction;
    QAction *m_separator;
    QList<QAction *> m_categories; // submenus
    QList<QAction *> m_results;    // search results, shown instead of the categories
};
