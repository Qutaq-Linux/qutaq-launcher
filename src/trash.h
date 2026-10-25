#pragma once

#include <QObject>

class QFileSystemWatcher;

// The trash at the bottom of the launcher (freedesktop Trash in ~/.local/share/Trash).
class Trash : public QObject
{
    Q_OBJECT
public:
    explicit Trash(QObject *parent = nullptr);

    bool isEmpty() const { return m_empty; }
    void open();      // in the file manager
    void empty();     // asks for confirmation first

Q_SIGNALS:
    void changed();

private:
    void update();
    void watch();

    QString m_dir;    // ~/.local/share/Trash
    bool m_empty = true;
    QFileSystemWatcher *m_watcher;
};
