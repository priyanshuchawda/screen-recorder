# Fedora Screen Recorder

A native C++20 Fedora 44 GNOME/Wayland application. It uses the desktop portal to let the user select a screen or a window, receives the permitted video through PipeWire, and writes H.264/AAC MP4s with GStreamer. There is no scripting runtime in the recording path.

## Native stack

- C++20 + CMake/Ninja
- GTK4/libadwaita UI and `xdg-desktop-portal`/libportal source selection
- PipeWire DMA-BUF transport and GStreamer muxing
- Intel Iris Xe `vah264lpenc` low-power H.264 first; Quick Sync/VA-API/OpenH264 fallbacks in that order
- V4L2 camera and PipeWire Pulse monitor audio, both opt-in

## Power and quality policy

The Fedora version intentionally mirrors the Windows project’s laptop policy.

| Mode | Resolution | FPS | H.264 bitrate | Power behavior |
|---|---:|---:|---:|---|
| Efficiency on AC | 848×480 | 30 / 60 | 4 / 6 Mbps | Default; hardware-first |
| Efficiency on battery | 848×480 | 15 | 1.5 Mbps | Automatically clamped |
| Battery Saver | 640×360 | 15 | 1 Mbps | Explicit low-cost mode; HQ takes precedence |
| High quality | 1920×1080 | 30 / 60 | 8 / 10 Mbps | Explicit opt-in; unchanged on battery |

The camera PiP is off by default. The app discovers V4L2 camera paths and persists the selected device. When enabled it uses a bounded two-frame path at 320×180/10 FPS in efficiency mode, 160×90/5 FPS in Battery Saver, or a 1280×720/30 FPS HQ profile. It is deliberately separate from the normal zero/low-copy Intel encode path, because compositing a camera frame costs power. The separate **Preview selected camera** action uses the same policy, opens a GTK4 preview window, and always stops before screen recording begins or when its window closes.

UPower is checked every ten seconds during a recording. A transition to battery immediately lowers the mutable encoder bitrate in efficiency mode; the next recording also receives the full 15 FPS battery profile. The active stream is not renegotiated in place, avoiding MP4 timestamp/resolution discontinuities.

## Install and run

This laptop already has the required runtime packages. Install the app for the current user:

```bash
cd fedora-linux-app
./install.sh
fedora-screen-recorder
```

Or build and run it directly during development:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/fedora-screen-recorder
```

The launcher is also installed as **Fedora Screen Recorder** in the GNOME app grid.

## Fedora dependencies

The app checks these at recording time and gives a precise error if one is unavailable:

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
  gtk4-devel libadwaita-devel libportal-devel libportal-gtk4-devel \
  gstreamer1-devel gstreamer1-plugins-base-devel pipewire-devel \
  gstreamer1-plugins-good gstreamer1-plugin-gtk4 gstreamer1-plugins-bad-free \
  gstreamer1-plugin-openh264 gstreamer1-plugin-libav pipewire libva-utils \
  xdg-desktop-portal xdg-desktop-portal-gnome
```

For this Intel Iris Xe laptop, the RPM Fusion packages `intel-media-driver` and `gstreamer1-plugins-bad-freeworld` enable the verified H.264 VA low-power encoder. The app refreshes the GStreamer registry at launch, then selects it automatically and falls back safely if unavailable.

## Behavior and safeguards

- Screen/window selection is always mediated by GNOME; no capture permission is retained.
- The red recording file is written as `*.partial.mp4` and renamed to `.mp4` only after GStreamer sends EOS and the MP4 is finalized.
- System-audio capture uses the PipeWire Pulse monitor (`@DEFAULT_MONITOR@`), not the microphone source.
- The microphone track is optional and passes through a light expander/noise gate before mixing.
- Every recording has a neighboring `.diagnostics.txt` file with selected encoder, power state, profile, audio/camera choices, completion status, encoded-frame count, audio-buffer count, and GStreamer QoS drops.
- The app checks free storage every 10 seconds and safely stops below 500 MB.
- Recording options and the selected output folder persist in `~/.config/fedora-screen-recorder/settings.ini`.
- Existing `.partial.mp4` files are detected at startup and preserved. The explicit recovery action uses the same safe rename model as Windows and refuses to overwrite an existing final MP4.
- The default destination is `~/Videos/Screen Recordings`.

## Validation

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
desktop-file-validate io.github.screenrecorder.Fedora.desktop
appstreamcli validate io.github.screenrecorder.Fedora.metainfo.xml
```
