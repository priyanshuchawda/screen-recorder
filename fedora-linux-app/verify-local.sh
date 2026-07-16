#!/usr/bin/env sh
# Complete local quality gate for Fedora app changes. This deliberately runs on
# the developer's Fedora machine instead of relying on remote GitHub Actions.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$root/build"

cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure
"$root/verify-ui-runtime.sh" "$build_dir/fedora-screen-recorder"
gst-inspect-1.0 gtk4paintablesink >/dev/null
desktop-file-validate "$root/io.github.screenrecorder.Fedora.desktop"
appstreamcli validate --no-net "$root/io.github.screenrecorder.Fedora.metainfo.xml"
sh -n "$root/packaging/build-rpm.sh"
sh -n "$root/install.sh"
sh -n "$root/install-rpm.sh"
sh -n "$root/verify-ui-runtime.sh"
"$root/packaging/build-rpm.sh"
