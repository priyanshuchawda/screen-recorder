#!/usr/bin/env sh
# Build and install for the current user. No root access is required.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${PREFIX:-"$HOME/.local"}
cmake -S "$root" -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$root/build"
cmake --install "$root/build"
printf 'Installed Fedora Screen Recorder to %s/bin/fedora-screen-recorder\n' "$prefix"
