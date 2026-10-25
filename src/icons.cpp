#include "icons.h"

#include <QDebug>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QtMath>

void setupIconTheme()
{
    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    const QString current = QIcon::themeName();
    if (!current.isEmpty() && current != QLatin1String("hicolor"))
        return;

    QString name;
    // 1. GTK 3/4: Manjaro Xfce writes the theme there too
    const QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    for (const char *f : {"/gtk-3.0/settings.ini", "/gtk-4.0/settings.ini"}) {
        QSettings s(cfg + QLatin1String(f), QSettings::IniFormat);
        name = s.value(QStringLiteral("Settings/gtk-icon-theme-name")).toString();
        if (!name.isEmpty())
            break;
    }
    // 2. Xfce (xsettings) and GNOME
    auto run = [](const QString &prog, const QStringList &args) {
        if (QStandardPaths::findExecutable(prog).isEmpty())
            return QString();
        QProcess p;
        p.start(prog, args);
        if (!p.waitForFinished(1500))
            return QString();
        QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        out.remove('\'');
        return out;
    };
    if (name.isEmpty())
        name = run(QStringLiteral("xfconf-query"), {QStringLiteral("-c"), QStringLiteral("xsettings"),
                                                    QStringLiteral("-p"), QStringLiteral("/Net/IconThemeName")});
    if (name.isEmpty())
        name = run(QStringLiteral("gsettings"), {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                                                 QStringLiteral("icon-theme")});
    if (!name.isEmpty()) {
        QIcon::setThemeName(name);
        qInfo().noquote() << "icon theme:" << name;
    }
}

QColor tileColor(const QIcon &icon)
{
    static QHash<qint64, QColor> cache;
    const qint64 key = icon.cacheKey();
    const auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return it.value();

    const QImage img = icon.pixmap(32, 32).toImage().convertToFormat(QImage::Format_ARGB32);
    double r = 0, g = 0, b = 0, weight = 0;
    for (int y = 0; y < img.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = QColor::fromRgba(line[x]);
            if (c.alpha() < 128)
                continue;
            const double s = c.hsvSaturationF();
            const double w = s * s * c.valueF(); // colorful and bright pixels define the tile
            r += c.redF() * w;
            g += c.greenF() * w;
            b += c.blueF() * w;
            weight += w;
        }
    }
    QColor out;
    if (weight < 2.0) {
        out = QColor(0x5a, 0x5a, 0x62); // monochrome icon
    } else {
        out = QColor::fromRgbF(r / weight, g / weight, b / weight);
        // Unity's tiles are muted: limit saturation and brightness
        float h, s, v;
        out.getHsvF(&h, &s, &v);
        out = QColor::fromHsvF(h, qMin(s, 0.65f), qBound(0.45f, v, 0.75f));
    }
    cache.insert(key, out);
    return out;
}

QIcon dashButtonIcon()
{
    QIcon icon;
    for (int size : {32, 64, 128}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const double c = size / 2.0, R = size * 0.30, dot = size * 0.075;
        p.setPen(QPen(QColor(0xf2, 0xf2, 0xf4), size * 0.07));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(c, c), R, R);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xf2, 0xf2, 0xf4));
        for (int i = 0; i < 3; ++i) { // three "friends" on the circle, like a generic launcher logo
            const double a = qDegreesToRadians(-90.0 + i * 120.0);
            p.drawEllipse(QPointF(c + R * qCos(a), c + R * qSin(a)), dot * 1.6, dot * 1.6);
        }
        icon.addPixmap(pm);
    }
    return icon;
}

QIcon placeholderIcon(const QString &name)
{
    QIcon icon;
    for (int size : {32, 64}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xe8, 0xe8, 0xec));
        p.drawRoundedRect(QRectF(size * 0.1, size * 0.1, size * 0.8, size * 0.8), size * 0.15, size * 0.15);
        QFont f;
        f.setPixelSize(int(size * 0.5));
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(0x30, 0x30, 0x36));
        p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, name.left(1).toUpper());
        icon.addPixmap(pm);
    }
    return icon;
}
