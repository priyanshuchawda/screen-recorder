#!/usr/bin/env sh
# Build an RPM from the Fedora app tree without needing a git archive or a
# system-wide rpmbuild configuration. Output is kept under packaging/rpmbuild.
set -eu

app_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$app_root/.." && pwd)
version=$(sed -n 's/^project(FedoraScreenRecorder VERSION \([^ ]*\).*/\1/p' "$app_root/CMakeLists.txt")

if [ -z "$version" ]; then
    printf '%s\n' 'Could not determine the version from CMakeLists.txt.' >&2
    exit 1
fi
if ! command -v rpmbuild >/dev/null 2>&1; then
    printf '%s\n' 'Install rpm-build first: sudo dnf install rpm-build' >&2
    exit 1
fi

topdir="$app_root/packaging/rpmbuild"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
source_root="$stage/fedora-screen-recorder-$version"
mkdir -p "$source_root" "$topdir/SOURCES" "$topdir/SPECS"

cp -a "$app_root/CMakeLists.txt" "$app_root/README.md" "$app_root/install.sh" \
    "$app_root/io.github.screenrecorder.Fedora.desktop" \
    "$app_root/io.github.screenrecorder.Fedora.metainfo.xml" \
    "$app_root/io.github.screenrecorder.Fedora.svg" \
    "$app_root/src" "$app_root/tests" "$source_root/"
cp "$repo_root/LICENSE" "$source_root/LICENSE"

tar -C "$stage" -czf "$topdir/SOURCES/fedora-screen-recorder-$version.tar.gz" \
    "fedora-screen-recorder-$version"
cp "$app_root/packaging/fedora-screen-recorder.spec" "$topdir/SPECS/"
rpmbuild -ba --define "_topdir $topdir" "$topdir/SPECS/fedora-screen-recorder.spec"

printf 'RPMs are available under %s/RPMS\n' "$topdir"
