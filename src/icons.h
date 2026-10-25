#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

// If Qt has no icon theme (under labwc there is usually no platform plugin), use the same
// one as GTK/Xfce, otherwise application icons won't be found by name.
void setupIconTheme();

// Background color of a launcher tile, taken from the icon like Unity does: the average
// of its saturated pixels, or a neutral grey for monochrome icons.
QColor tileColor(const QIcon &icon);

// Drawn icons for when the theme has none
QIcon dashButtonIcon();   // the button at the top: a circle of dots
QIcon placeholderIcon(const QString &name); // the first letter of the name
