#!/usr/bin/env sh
# Starts the real GTK application briefly and fails if it exits during startup.
# Preview is disabled only for this smoke test, so it does not reserve the
# user's V4L2 device while validating the window shell.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app=${1:-"$root/build/fedora-screen-recorder"}

if [ -z "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
    printf '%s\n' 'Skipping UI runtime smoke test: no graphical session is available.'
    exit 0
fi

config_dir=$(mktemp -d)
log_file=$(mktemp)
cleanup() {
    rm -rf "$config_dir"
    rm -f "$log_file"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$config_dir/fedora-screen-recorder"
printf '%s\n' '[Camera]' 'preview=false' > "$config_dir/fedora-screen-recorder/settings.ini"

set +e
XDG_CONFIG_HOME="$config_dir" timeout --signal=TERM --kill-after=2 4 "$app" >"$log_file" 2>&1
status=$?
set -e

case "$status" in
    124|137|143)
        printf '%s\n' 'UI runtime smoke test passed.'
        ;;
    *)
        cat "$log_file" >&2
        printf 'UI runtime smoke test failed: application exited with status %s.\n' "$status" >&2
        exit "$status"
        ;;
esac
