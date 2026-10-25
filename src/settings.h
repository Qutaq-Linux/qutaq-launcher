#pragma once

#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

// ~/.config/qutaq-launcher/launcher.conf; key is "group/key", e.g. "launcher/iconSize".
// The file is re-read on every access, so edits apply without a restart.
inline QSettings &launcherSettings()
{
    static QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                           + QStringLiteral("/qutaq-launcher/launcher.conf"),
                       QSettings::IniFormat);
    return s;
}

inline QVariant launcherSetting(const QString &key, const QVariant &def = QVariant())
{
    launcherSettings().sync();
    return launcherSettings().value(key, def);
}

inline void launcherSettingSet(const QString &key, const QVariant &value)
{
    launcherSettings().setValue(key, value);
    launcherSettings().sync();
}
