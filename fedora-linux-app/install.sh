#!/usr/bin/env sh
# Build and install for the current user. No root access is required.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${PREFIX:-"$HOME/.local"}
cmake -S "$root" -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$root/build"
cmake --install "$root/build"

# A source install belongs in the same desktop locations as a packaged app.
# These refreshes make the entry and scalable icon visible immediately on
# GNOME, KDE, and other XDG-compliant Fedora desktops.
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$prefix/share/applications" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$prefix/share/icons/hicolor" >/dev/null 2>&1 || true
fi

printf 'Installed Fedora Screen Recorder to %s/bin/fedora-screen-recorder\n' "$prefix"
printf 'Open it from the app grid as "Fedora Screen Recorder", or run: gtk-launch io.github.screenrecorder.Fedora\n'
