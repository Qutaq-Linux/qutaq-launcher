#pragma once

#include <QDBusMessage>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class QTimer;

// A volume shown in the launcher above the trash, as in Unity: USB sticks, memory cards,
// external and internal drives, optical discs, floppies.
struct Device {
    QString path;        // UDisks2 block object, e.g. /org/freedesktop/UDisks2/block_devices/sdb1
    QString devFile;     // /dev/sdb1
    QString drive;       // UDisks2 drive object
    QString name;        // label, or "16 GB Volume"
    QString iconName;
    QStringList mountPoints;
    qint64 size = 0;
    bool hasFilesystem = false; // can be mounted (an audio CD or a blank disc can't)
    bool encrypted = false;     // LUKS: unlocked from the file manager
    bool ejectable = false;     // optical, floppy, some card readers
    bool canPowerOff = false;   // USB drives: "Safely Remove"
    bool removable = false;

    bool isMounted() const { return !mountPoints.isEmpty(); }
};

// "16 GB", "1.5 MB": one decimal only for small numbers
QString formatSize(qint64 bytes);

// Volumes from UDisks2 (system bus). Updates itself when media are plugged in or removed.
// launcher/devices in launcher.conf: all (default), removable, never.
class Devices : public QObject
{
    Q_OBJECT
public:
    explicit Devices(QObject *parent = nullptr);

    QList<Device> devices() const { return m_devices; }
    Device device(const QString &path) const;

    void open(const QString &path);      // mount if needed, then open in the file manager
    void mount(const QString &path, bool thenOpen = false);
    void unmount(const QString &path);
    void eject(const QString &path);     // unmount everything on the drive, then eject or power off

Q_SIGNALS:
    void changed();
    void error(const QString &title, const QString &message);

private Q_SLOTS:
    void onUDisksSignal(const QDBusMessage &msg);

private:
    void refresh();
    void call(const QString &path, const QString &iface, const QString &method,
              const QVariantList &args, const std::function<void(const QDBusMessage &)> &done);

    QList<Device> m_devices;
    QTimer *m_debounce;
};
