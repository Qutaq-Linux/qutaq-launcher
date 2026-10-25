# qutaq-launcher

## Overview

`qutaq-launcher` is left taskbar panel for Qutaq Desktop.

## Build

To build the program, be sure to install the libraries

**Arch Linux**

```sh
sudo pacman -S --needed base-devel git cmake qt6-base qt6-wayland qt6-tools qt6-svg layer-shell-qt wayland udisks2
```

**Debian / Ubuntu**

```sh
sudo apt install build-essential git cmake pkg-config libwayland-dev \
  qt6-base-dev qt6-base-private-dev qt6-wayland-dev qt6-wayland-private-dev \
  qt6-wayland-dev-tools qt6-tools-dev qt6-l10n-tools liblayershellqtinterface-dev \
  qt6-svg-plugins udisks2
```

**Fedora**

```sh
sudo dnf install gcc-c++ git cmake wayland-devel \
  qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtwayland-devel qt6-qttools-devel qt6-linguist \
  layer-shell-qt-devel qt6-qtsvg udisks2
```

Building:

```sh
git clone https://github.com/Qutaq-Linux/qutaq-launcher.git
cd qutaq-launcher
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Using

`~/.config/labwc/autostart` — the panel first, then the launcher:

```sh
~/.local/bin/qutaq-panel >/tmp/qutaq-panel.log 2>&1 &
~/.local/bin/qutaq-launcher >/tmp/qutaq-launcher.log 2>&1 &
sleep 1
```
