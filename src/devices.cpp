#include "devices.h"

#include "settings.h"

#include <QCollator>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QLocale>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <QCoreApplication>
#include <QSet>

#include <functional>
#include <memory>

static const char *kService = "org.freedesktop.UDisks2";
static const char *kBlock = "org.freedesktop.UDisks2.Block";
static const char *kFilesystem = "org.freedesktop.UDisks2.Filesystem";
static const char *kDrive = "org.freedesktop.UDisks2.Drive";
static const char *kPartition = "org.freedesktop.UDisks2.Partition";

static QDBusConnection sysBus() { return QDBusConnection::systemBus(); }

using Objects = QHash<QString, QHash<QString, QVariantMap>>; // path → interface → properties

static Objects managedObjects()
{
    Objects out;
    QDBusConnectionInterface *iface = sysBus().interface();
    if (!sysBus().isConnected() || !iface || !iface->isServiceRegistered(QLatin1String(kService)).value())
        return out;
    const QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(kService), QStringLiteral("/org/freedesktop/UDisks2"),
                                                            QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                                            QStringLiteral("GetManagedObjects"));
    const QDBusMessage r = sysBus().call(msg, QDBus::Block, 3000);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return out;
    const QDBusArgument arg = r.arguments().first().value<QDBusArgument>(); // a{oa{sa{sv}}}
    arg.beginMap();
    while (!arg.atEnd()) {
        QDBusObjectPath path;
        arg.beginMapEntry();
        arg >> path;
        QHash<QString, QVariantMap> ifaces;
        arg.beginMap();
        while (!arg.atEnd()) {
            QString name;
            QVariantMap props;
            arg.beginMapEntry();
            arg >> name >> props;
            arg.endMapEntry();
            ifaces.insert(name, props);
        }
        arg.endMap();
        arg.endMapEntry();
        out.insert(path.path(), ifaces);
    }
    arg.endMap();
    return out;
}

// aay: NUL-terminated byte strings (mount points, symlinks)
static QStringList byteStrings(const QVariant &v)
{
    QStringList out;
    if (!v.canConvert<QDBusArgument>())
        return out;
    const QDBusArgument arg = v.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QByteArray b;
        arg >> b;
        if (b.endsWith('\0'))
            b.chop(1);
        out << QString::fromLocal8Bit(b);
    }
    arg.endArray();
    return out;
}

static QStringList stringList(const QVariant &v)
{
    if (v.canConvert<QDBusArgument>()) {
        QStringList out;
        v.value<QDBusArgument>() >> out;
        return out;
    }
    return v.toStringList();
}

static QString objectPath(const QVariant &v)
{
    return v.value<QDBusObjectPath>().path();
}

// Mounted by the system itself (root, /boot, /home, /var…): not a "device" for the user
static bool isSystemMount(const QString &mp)
{
    if (mp.startsWith(QLatin1String("/media/")) || mp.startsWith(QLatin1String("/run/media/"))
        || mp.startsWith(QLatin1String("/mnt/")))
        return false;
    return true;
}

QString formatSize(qint64 bytes)
{
    QString size = QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
    const QString dec = QLocale().decimalPoint();
    const int unitStart = size.indexOf(QChar(' '));
    if (unitStart > 0) {
        const QString number = size.left(unitStart);
        const int point = number.indexOf(dec);
        if (point > 0 && (number.endsWith(dec + '0') || point >= 2))
            size = number.left(point) + size.mid(unitStart);
    }
    return size;
}

Devices::Devices(QObject *parent)
    : QObject(parent)
    , m_debounce(new QTimer(this))
{
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250); // plugging in a drive produces a burst of signals
    connect(m_debounce, &QTimer::timeout, this, &Devices::refresh);

    QDBusConnection bus = sysBus();
    const QString svc = QLatin1String(kService);
    bus.connect(svc, QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                QStringLiteral("InterfacesAdded"), this, SLOT(onUDisksSignal(QDBusMessage)));
    bus.connect(svc, QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                QStringLiteral("InterfacesRemoved"), this, SLOT(onUDisksSignal(QDBusMessage)));
    // Mounting, media changes: PropertiesChanged on any object of the service
    bus.connect(svc, QString(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                this, SLOT(onUDisksSignal(QDBusMessage)));
    auto *watcher = new QDBusServiceWatcher(svc, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, m_debounce, qOverload<>(&QTimer::start));
    refresh();
}

void Devices::onUDisksSignal(const QDBusMessage &)
{
    m_debounce->start();
}

Device Devices::device(const QString &path) const
{
    for (const Device &d : m_devices)
        if (d.path == path)
            return d;
    return {};
}

void Devices::refresh()
{
    const QString mode = launcherSetting(QStringLiteral("launcher/devices"), QStringLiteral("all")).toString();
    QList<Device> out;
    if (mode != QLatin1String("never")) {
        const Objects objs = managedObjects();
        QSet<QString> unlocked; // LUKS containers that have a cleartext device
        for (auto it = objs.cbegin(); it != objs.cend(); ++it) {
            const QString backing = objectPath(it.value().value(QLatin1String(kBlock)).value(QStringLiteral("CryptoBackingDevice")));
            if (!backing.isEmpty() && backing != QLatin1String("/"))
                unlocked.insert(backing);
        }
        for (auto it = objs.cbegin(); it != objs.cend(); ++it) {
            const QVariantMap block = it.value().value(QLatin1String(kBlock));
            if (block.isEmpty() || block.value(QStringLiteral("HintIgnore")).toBool())
                continue;
            const QVariantMap drive = objs.value(objectPath(block.value(QStringLiteral("Drive")))).value(QLatin1String(kDrive));
            const QString usage = block.value(QStringLiteral("IdUsage")).toString();
            const bool hasFs = it.value().contains(QLatin1String(kFilesystem));
            const bool optical = drive.value(QStringLiteral("Optical")).toBool();
            const QStringList media = stringList(drive.value(QStringLiteral("MediaCompatibility")));
            const bool floppy = std::any_of(media.cbegin(), media.cend(), [](const QString &m) { return m.startsWith(QLatin1String("floppy")); });

            Device d;
            d.path = it.key();
            d.drive = objectPath(block.value(QStringLiteral("Drive")));
            d.hasFilesystem = hasFs;
            d.encrypted = usage == QLatin1String("crypto");
            // Only volumes: filesystems, locked LUKS containers, and discs or floppies with media
            if (!hasFs && !d.encrypted && !((optical || floppy) && drive.value(QStringLiteral("MediaAvailable")).toBool()
                                            && block.value(QStringLiteral("Size")).toLongLong() > 0))
                continue;
            if (usage == QLatin1String("other") || block.value(QStringLiteral("IdType")).toString() == QLatin1String("swap"))
                continue;
            // An unlocked LUKS container is shown through its cleartext filesystem, not twice
            if (d.encrypted && unlocked.contains(d.path))
                continue;
            d.mountPoints = byteStrings(it.value().value(QLatin1String(kFilesystem)).value(QStringLiteral("MountPoints")));
            bool system = false;
            for (const QString &mp : std::as_const(d.mountPoints))
                system = system || isSystemMount(mp);
            if (system)
                continue; // /, /boot, /home…
            d.size = block.value(QStringLiteral("Size")).toLongLong();
            d.removable = drive.value(QStringLiteral("Removable")).toBool() || drive.value(QStringLiteral("MediaRemovable")).toBool();
            d.ejectable = drive.value(QStringLiteral("Ejectable")).toBool() || optical;
            d.canPowerOff = drive.value(QStringLiteral("CanPowerOff")).toBool();
            if (mode == QLatin1String("removable") && !d.removable && !d.ejectable && !d.canPowerOff)
                continue;
            // Internal partitions that the system marks as such (EFI, recovery) are skipped
            // unless they are mounted for the user
            if (block.value(QStringLiteral("HintSystem")).toBool() && !d.isMounted()) {
                const QString ptype = it.value().value(QLatin1String(kPartition)).value(QStringLiteral("Type")).toString().toLower();
                static const QStringList hidden = {
                    QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"), // EFI system
                    QStringLiteral("de94bba4-06d1-4d40-a16a-bfd50179d6ac"), // Windows recovery
                    QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae"), // Microsoft reserved
                    QStringLiteral("21686148-6449-6e6f-744e-656564454649"), // BIOS boot
                    QStringLiteral("0xef"), QStringLiteral("0x27"),
                };
                if (hidden.contains(ptype) || block.value(QStringLiteral("IdLabel")).toString().compare(QLatin1String("EFI"), Qt::CaseInsensitive) == 0)
                    continue;
            }

            QString label = block.value(QStringLiteral("HintName")).toString();
            if (label.isEmpty())
                label = block.value(QStringLiteral("IdLabel")).toString();
            if (label.isEmpty() && optical && !hasFs)
                label = tr("Audio Disc");
            if (label.isEmpty())
                label = d.encrypted ? tr("%1 Encrypted Volume").arg(formatSize(d.size)) : tr("%1 Volume").arg(formatSize(d.size));
            d.name = label;
            QByteArray devFile = block.value(QStringLiteral("Device")).toByteArray();
            if (devFile.endsWith('\0'))
                devFile.chop(1);
            d.devFile = QString::fromLocal8Bit(devFile);

            // Icon: what UDisks suggests, otherwise by the kind of drive
            d.iconName = block.value(QStringLiteral("HintIconName")).toString();
            if (d.iconName.isEmpty()) {
                const QString bus = drive.value(QStringLiteral("ConnectionBus")).toString();
                const bool card = std::any_of(media.cbegin(), media.cend(), [](const QString &m) { return m.startsWith(QLatin1String("flash")); });
                const int rpm = drive.value(QStringLiteral("RotationRate"), -1).toInt();
                if (optical)
                    d.iconName = QStringLiteral("media-optical");
                else if (floppy)
                    d.iconName = QStringLiteral("media-floppy");
                else if (card)
                    d.iconName = QStringLiteral("media-flash");
                else if (bus == QLatin1String("usb") && d.removable)
                    d.iconName = QStringLiteral("drive-removable-media-usb");
                else if (d.removable || bus == QLatin1String("usb"))
                    d.iconName = QStringLiteral("drive-removable-media");
                else if (rpm == 0)
                    d.iconName = QStringLiteral("drive-harddisk-solidstate");
                else
                    d.iconName = QStringLiteral("drive-harddisk");
            }
            out << d;
        }
    }
    // Stable order: removable media first (as they come and go), then by name
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(out.begin(), out.end(), [&](const Device &a, const Device &b) {
        const bool ra = a.removable || a.ejectable || a.canPowerOff, rb = b.removable || b.ejectable || b.canPowerOff;
        if (ra != rb)
            return rb; // internal drives first, removable ones next to the trash, as in Unity
        return collator.compare(a.name, b.name) < 0;
    });
    bool same = out.size() == m_devices.size();
    for (int i = 0; same && i < out.size(); ++i)
        same = out.at(i).path == m_devices.at(i).path && out.at(i).name == m_devices.at(i).name
               && out.at(i).mountPoints == m_devices.at(i).mountPoints && out.at(i).iconName == m_devices.at(i).iconName;
    if (same)
        return;
    m_devices = out;
    Q_EMIT changed();
}

void Devices::call(const QString &path, const QString &iface, const QString &method, const QVariantList &args,
                   const std::function<void(const QDBusMessage &)> &done)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(kService), path, iface, method);
    msg.setArguments(args);
    msg.setInteractiveAuthorizationAllowed(true); // polkit may ask for a password (internal drives)
    auto *w = new QDBusPendingCallWatcher(sysBus().asyncCall(msg, 120000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        done(w->reply());
    });
}

static bool isError(const QDBusMessage &r) { return r.type() == QDBusMessage::ErrorMessage; }

static void openFolder(const QString &dir)
{
    if (!QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty()
        && QProcess::startDetached(QStringLiteral("gio"), {QStringLiteral("open"), dir}))
        return;
    QProcess::startDetached(QStringLiteral("xdg-open"), {dir});
}

void Devices::open(const QString &path)
{
    const Device d = device(path);
    if (d.isMounted())
        openFolder(d.mountPoints.first());
    else if (d.hasFilesystem)
        mount(path, true);
    else if (d.encrypted) {
        // Unlocking needs a passphrase dialog: GNOME Disks has one; otherwise the file manager (gvfs)
        if (!QStandardPaths::findExecutable(QStringLiteral("gnome-disks")).isEmpty() && !d.devFile.isEmpty())
            QProcess::startDetached(QStringLiteral("gnome-disks"), {QStringLiteral("--block-device"), d.devFile});
        else
            Q_EMIT error(tr("%1 is encrypted").arg(d.name),
                         tr("Unlock it in the file manager (Thunar, Nautilus…) or install GNOME Disks (gnome-disk-utility)."));
    }
}

void Devices::mount(const QString &path, bool thenOpen)
{
    const QString name = device(path).name;
    call(path, QLatin1String(kFilesystem), QStringLiteral("Mount"), {QVariantMap{}}, [this, name, thenOpen](const QDBusMessage &r) {
        if (isError(r)) {
            Q_EMIT error(tr("Unable to mount %1").arg(name), r.errorMessage());
            return;
        }
        const QString mp = r.arguments().value(0).toString();
        qInfo().noquote() << "mounted" << name << "at" << mp;
        if (thenOpen && !mp.isEmpty())
            openFolder(mp);
    });
}

void Devices::unmount(const QString &path)
{
    const QString name = device(path).name;
    call(path, QLatin1String(kFilesystem), QStringLiteral("Unmount"), {QVariantMap{}}, [this, name](const QDBusMessage &r) {
        if (isError(r))
            Q_EMIT error(tr("Unable to unmount %1").arg(name), r.errorMessage());
    });
}

void Devices::eject(const QString &path)
{
    const Device d = device(path);
    if (d.drive.isEmpty() || d.drive == QLatin1String("/"))
        return;
    // Unmount every volume of this drive first, then eject the medium or power the drive off
    QStringList mounted;
    for (const Device &other : std::as_const(m_devices))
        if (other.drive == d.drive && other.isMounted())
            mounted << other.path;
    auto finish = [this, d]() {
        const QString method = d.canPowerOff && !d.ejectable ? QStringLiteral("PowerOff") : QStringLiteral("Eject");
        call(d.drive, QLatin1String(kDrive), method, {QVariantMap{}}, [this, d, method](const QDBusMessage &r) {
            // Some card readers can't power off: ejecting the medium is enough then
            if (isError(r) && method == QLatin1String("PowerOff"))
                call(d.drive, QLatin1String(kDrive), QStringLiteral("Eject"), {QVariantMap{}}, [](const QDBusMessage &) {});
            else if (isError(r))
                Q_EMIT error(tr("Unable to eject %1").arg(d.name), r.errorMessage());
            else
                qInfo().noquote() << method << d.name;
        });
    };
    auto remaining = std::make_shared<int>(mounted.size());
    auto failed = std::make_shared<bool>(false);
    if (mounted.isEmpty()) {
        finish();
        return;
    }
    for (const QString &p : std::as_const(mounted)) {
        call(p, QLatin1String(kFilesystem), QStringLiteral("Unmount"), {QVariantMap{}}, [=](const QDBusMessage &r) {
            if (isError(r) && !*failed) {
                *failed = true;
                Q_EMIT error(tr("Unable to eject %1").arg(d.name), r.errorMessage()); // usually "target is busy"
            }
            if (--*remaining == 0 && !*failed)
                finish();
        });
    }
}
