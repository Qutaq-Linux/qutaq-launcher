#include "appmenu.h"

#include "desktopentries.h"

#include <QCollator>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QTimer>
#include <QWidgetAction>

namespace {
struct Category {
    const char *id;       // main category from the Desktop Menu Specification
    const char *name;     // shown in the menu (translated)
    const char *icon;
};
// The order in which an application's categories are checked: "Settings;System" goes to Settings.
const Category kCategories[] = {
    {"Settings", QT_TRANSLATE_NOOP("AppMenu", "Settings"), "preferences-desktop"},
    {"Game", QT_TRANSLATE_NOOP("AppMenu", "Games"), "applications-games"},
    {"Education", QT_TRANSLATE_NOOP("AppMenu", "Education"), "applications-education"},
    {"Science", QT_TRANSLATE_NOOP("AppMenu", "Science"), "applications-science"},
    {"Development", QT_TRANSLATE_NOOP("AppMenu", "Development"), "applications-development"},
    {"Graphics", QT_TRANSLATE_NOOP("AppMenu", "Graphics"), "applications-graphics"},
    {"AudioVideo", QT_TRANSLATE_NOOP("AppMenu", "Multimedia"), "applications-multimedia"},
    {"Network", QT_TRANSLATE_NOOP("AppMenu", "Internet"), "applications-internet"},
    {"Office", QT_TRANSLATE_NOOP("AppMenu", "Office"), "applications-office"},
    {"System", QT_TRANSLATE_NOOP("AppMenu", "System"), "applications-system"},
    {"Utility", QT_TRANSLATE_NOOP("AppMenu", "Accessories"), "applications-accessories"},
};
const char *kOther = QT_TRANSLATE_NOOP("AppMenu", "Other");

QString categoryOf(const DesktopEntry &e)
{
    for (const Category &c : kCategories) {
        const QString id = QLatin1String(c.id);
        if (e.categories.contains(id) || (id == QLatin1String("AudioVideo")
                                          && (e.categories.contains(QStringLiteral("Audio")) || e.categories.contains(QStringLiteral("Video")))))
            return id;
    }
    return QStringLiteral("Other");
}

// How well an application matches the search text (0 = not at all)
int matchScore(const DesktopEntry &e, const QString &q)
{
    const QString name = e.name.toLower();
    if (name.startsWith(q))
        return 100;
    for (const QString &w : name.split(' ', Qt::SkipEmptyParts))
        if (w.startsWith(q))
            return 80;
    if (name.contains(q))
        return 60;
    if (e.genericName.toLower().contains(q))
        return 40;
    for (const QString &k : e.keywords)
        if (k.toLower().startsWith(q))
            return 30;
    if (e.id.toLower().contains(q) || e.exec.section(' ', 0, 0).toLower().contains(q))
        return 20;
    if (e.comment.toLower().contains(q))
        return 10;
    return 0;
}
} // namespace

AppMenu::AppMenu(QWidget *parent)
    : QMenu(parent)
    , m_search(new QLineEdit)
    , m_searchAction(new QWidgetAction(this))
{
    m_search->setPlaceholderText(tr("Search applications…"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(240);
    // Margins come from a layout around the field: stylesheet margins inside a menu clip the text
    auto *box = new QWidget;
    auto *layout = new QHBoxLayout(box);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->addWidget(m_search);
    box->setFocusProxy(m_search);
    m_searchAction->setDefaultWidget(box);
    addAction(m_searchAction);
    m_separator = addSeparator();

    connect(m_search, &QLineEdit::textChanged, this, &AppMenu::filter);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (!m_results.isEmpty() && m_results.first()->isEnabled()) {
            m_results.first()->trigger();
            close();
        }
    });
    connect(this, &QMenu::aboutToHide, this, [this] {
        QTimer::singleShot(0, this, [this] { m_search->clear(); });
    });
    connect(DesktopEntries::instance(), &DesktopEntries::changed, this, &AppMenu::rebuild);
    rebuild();
}

void AppMenu::showEvent(QShowEvent *e)
{
    QMenu::showEvent(e);
    setActiveAction(m_searchAction);
    m_search->setFocus(Qt::PopupFocusReason); // type right away
}

QAction *AppMenu::appAction(const DesktopEntry &e, QMenu *into)
{
    QAction *a = new QAction(DesktopEntries::icon(e.icon), QString(e.name).replace('&', QLatin1String("&&")), into);
    if (!e.genericName.isEmpty() || !e.comment.isEmpty())
        a->setToolTip(e.comment.isEmpty() ? e.genericName : e.comment);
    const QString id = e.id;
    connect(a, &QAction::triggered, this, [id] { DesktopEntries::launch(DesktopEntries::instance()->entry(id)); });
    return a;
}

void AppMenu::rebuild()
{
    for (QAction *a : std::as_const(m_categories)) {
        removeAction(a);
        a->menu()->deleteLater();
    }
    m_categories.clear();

    QHash<QString, QList<DesktopEntry>> byCategory;
    for (const DesktopEntry &e : DesktopEntries::instance()->all())
        if (!e.noDisplay)
            byCategory[categoryOf(e)] << e;

    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    auto addCategory = [&](const QString &id, const QString &name, const char *icon) {
        QList<DesktopEntry> list = byCategory.value(id);
        if (list.isEmpty())
            return;
        std::sort(list.begin(), list.end(), [&](const DesktopEntry &a, const DesktopEntry &b) { return collator.compare(a.name, b.name) < 0; });
        auto *sub = new QMenu(name, this);
        sub->setIcon(QIcon::fromTheme(QLatin1String(icon)));
        sub->setToolTipsVisible(true);
        for (const DesktopEntry &e : std::as_const(list))
            sub->addAction(appAction(e, sub));
        m_categories << sub->menuAction();
    };
    // Sorted by the translated category name, "Other" last
    QList<const Category *> cats;
    for (const Category &c : kCategories)
        cats << &c;
    std::sort(cats.begin(), cats.end(), [&](const Category *a, const Category *b) {
        return collator.compare(tr(a->name), tr(b->name)) < 0;
    });
    for (const Category *c : std::as_const(cats))
        addCategory(QLatin1String(c->id), tr(c->name), c->icon);
    addCategory(QStringLiteral("Other"), tr(kOther), "applications-other");
    addActions(m_categories);
    filter(m_search->text());
}

void AppMenu::filter(const QString &text)
{
    for (QAction *a : std::as_const(m_results)) {
        removeAction(a);
        a->deleteLater();
    }
    m_results.clear();
    const QString q = text.trimmed().toLower();
    for (QAction *a : std::as_const(m_categories))
        a->setVisible(q.isEmpty());
    if (q.isEmpty())
        return;

    QList<QPair<int, DesktopEntry>> found;
    for (const DesktopEntry &e : DesktopEntries::instance()->all())
        if (!e.noDisplay)
            if (const int s = matchScore(e, q))
                found << qMakePair(s, e);
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second.name.compare(b.second.name, Qt::CaseInsensitive) < 0;
    });
    for (int i = 0; i < found.size() && i < 20; ++i)
        m_results << appAction(found.at(i).second, this);
    if (m_results.isEmpty()) {
        QAction *none = new QAction(tr("No applications found"), this);
        none->setEnabled(false);
        m_results << none;
    }
    addActions(m_results);
}
