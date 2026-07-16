#!/usr/bin/env sh
# Install a released Fedora Screen Recorder RPM with DNF, including its runtime
# dependencies. Pass the downloaded RPM path, or run beside a locally built RPM.
set -eu

if ! command -v dnf >/dev/null 2>&1; then
    printf '%s\n' 'This installer requires Fedora DNF.' >&2
    exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
package=${1:-}

if [ -z "$package" ]; then
    for candidate in "$root"/packaging/rpmbuild/RPMS/*/fedora-screen-recorder-*.rpm; do
        if [ -f "$candidate" ]; then
            package=$candidate
            break
        fi
    done
fi

if [ -z "$package" ] || [ ! -f "$package" ]; then
    printf '%s\n' 'Pass the downloaded fedora-screen-recorder-*.rpm file as the first argument.' >&2
    exit 2
fi

package=$(CDPATH= cd -- "$(dirname -- "$package")" && pwd)/$(basename -- "$package")
sudo dnf install --assumeyes "$package"

printf '%s\n' 'Installed. Open "Fedora Screen Recorder" from the app grid, then right-click it and choose "Pin to Dash" to keep it on the taskbar.'
