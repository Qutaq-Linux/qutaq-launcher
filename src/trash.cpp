#include "trash.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>

Trash::Trash(QObject *parent)
    : QObject(parent)
    , m_dir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash"))
    , m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        watch(); // Trash/files may have just been created
        update();
    });
    watch();
    update();
}

void Trash::watch()
{
    const QString data = QFileInfo(m_dir).path(); // ~/.local/share: notices Trash being created
    for (const QString &d : {data, m_dir, m_dir + QStringLiteral("/files")})
        if (QFileInfo::exists(d) && !m_watcher->directories().contains(d))
            m_watcher->addPath(d);
}

void Trash::update()
{
    const bool empty = QDir(m_dir + QStringLiteral("/files")).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    if (empty != m_empty) {
        m_empty = empty;
        Q_EMIT changed();
    }
}

void Trash::open()
{
    // trash:/// needs gvfs (Thunar, Nautilus, Nemo) or KIO (Dolphin); without them, open the folder
    if (!QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty()
        && QProcess::startDetached(QStringLiteral("gio"), {QStringLiteral("open"), QStringLiteral("trash:///")}))
        return;
    QDir().mkpath(m_dir + QStringLiteral("/files"));
    QProcess::startDetached(QStringLiteral("xdg-open"), {m_dir + QStringLiteral("/files")});
}

void Trash::empty()
{
    QMessageBox box(QMessageBox::Question, tr("Empty Trash"),
                    tr("Empty all items from the Trash?"),
                    QMessageBox::NoButton);
    box.setInformativeText(tr("All items in the Trash will be permanently deleted."));
    QPushButton *yes = box.addButton(tr("Empty Trash"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != yes)
        return;

    // gio also cleans trash folders on other drives (needs gvfs); our own folder is cleaned below anyway
    if (!QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty()) {
        QProcess p;
        p.start(QStringLiteral("gio"), {QStringLiteral("trash"), QStringLiteral("--empty")});
        p.waitForFinished(30000);
    }
    for (const char *sub : {"/files", "/info", "/expunged"}) {
        QDir d(m_dir + QLatin1String(sub));
        if (d.exists())
            for (const QFileInfo &fi : d.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
                fi.isDir() && !fi.isSymLink() ? QDir(fi.filePath()).removeRecursively() : QFile::remove(fi.filePath());
    }
    QFile::remove(m_dir + QStringLiteral("/directorysizes"));
    update();
}
