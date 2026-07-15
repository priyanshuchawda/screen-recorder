#!/usr/bin/env sh
# Optional Intel Iris Xe hardware-path gate for this Fedora laptop. It uses the
# same private GStreamer registry refreshed by the app, then verifies a real
# VA low-power H.264 MP4 instead of merely checking plugin discovery.
set -eu

cache_root=${XDG_CACHE_HOME:-"$HOME/.cache"}
registry="$cache_root/fedora-screen-recorder/gstreamer-registry.bin"
output=$(mktemp --suffix=.mp4)
trap 'rm -f "$output"' EXIT HUP INT TERM

mkdir -p "$(dirname -- "$registry")"
GST_REGISTRY="$registry" gst-inspect-1.0 vah264lpenc >/dev/null
GST_REGISTRY="$registry" gst-launch-1.0 -q \
    videotestsrc num-buffers=30 ! videoconvert ! vapostproc ! \
    'video/x-raw(memory:VAMemory),format=NV12,width=848,height=480,framerate=30/1' ! \
    vah264lpenc bitrate=4000 key-int-max=60 ! h264parse ! mp4mux faststart=true ! \
    filesink location="$output"

codec=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$output")
width=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$output")
height=$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$output")
fps=$(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of csv=p=0 "$output")

test "$codec" = h264
test "$width" = 848
test "$height" = 480
test "$fps" = 30/1
printf 'Intel VA hardware encode verified: %sx%s H.264 at %s FPS\n' "$width" "$height" "$fps"
