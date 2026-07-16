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

The camera PiP is off by default. The app discovers V4L2 camera paths and persists the selected device. When enabled it uses a bounded two-frame path at 320×180/10 FPS in efficiency mode, 160×90/5 FPS in Battery Saver, or a 1280×720/30 FPS HQ profile. It is deliberately separate from the normal zero/low-copy Intel encode path, because compositing a camera frame costs power. **Live camera preview** is separate from PiP and is enabled by default when a camera is available: it opens a movable GTK window at launch, stops while recording, and returns when recording or a cancelled portal request ends. On AC it matches the Windows efficiency target at 640×360/20 FPS; on battery it uses 320×180/15 FPS, Battery Saver uses 160×90/5 FPS, and HQ uses 1280×720/30 FPS. Its one-buffer leaky queue always favors the newest frame. Closing the window turns the preview off persistently.

The preview is an independent movable and resizable window with explicit minimize, maximize/restore, and close controls. It fills its user-selected shape rather than showing letterbox bands. It also watches its own GStreamer errors: if a V4L2 device is disconnected or the preview ends, the app closes the preview cleanly and tells you how to reopen it after reconnecting the device.

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

### Build a native RPM

The repository includes a reproducible Fedora RPM recipe. On a Fedora build
machine with the dependencies below, run:

```bash
cd fedora-linux-app
sudo dnf install rpm-build
./packaging/build-rpm.sh
sudo dnf install packaging/rpmbuild/RPMS/$(uname -m)/fedora-screen-recorder-0.1.1-1.*.rpm
```

This packages the executable, GNOME desktop entry, AppStream metadata, and
icon. The generated RPMs are intentionally ignored by Git.

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

Available encoders are tried in a hardware-first order (VA low-power, Quick Sync, then VA-API). If one fails while the pipeline starts or before its first encoded frame, the app keeps the existing portal session and retries the next candidate, ending with OpenH264 rather than losing the recording attempt. Once a frame is encoded, the partial-file safety model takes precedence and the recording is preserved rather than replaced.

## Behavior and safeguards

- The main window is intentionally a compact recording dashboard. **Recording settings** opens native GNOME preference pages for audio, video and power, camera, and storage, so configuration does not obscure recording controls.
- Screen/window selection is always mediated by GNOME; no capture permission is retained.
- The red recording file is written as `*.partial.mp4` and renamed to `.mp4` only after GStreamer sends EOS and the MP4 is finalized.
- System-audio capture uses the PipeWire Pulse monitor (`@DEFAULT_MONITOR@`), not the microphone source.
- The microphone track is optional and passes through a light expander/noise gate before mixing.
- Pause/resume uses a monotonic clock with sub-second accounting, so repeated short pauses do not inflate the displayed recording duration.
- Every recording has a neighboring `.diagnostics.txt` file with selected encoder, power state, profile, audio/camera choices, completion status, PipeWire captured-frame count, encoded-frame count, audio-buffer count, and GStreamer QoS drops.
- The app checks free storage every 10 seconds and safely stops below 500 MB.
- Recording options and the selected output folder persist in `~/.config/fedora-screen-recorder/settings.ini`.
- The GTK title bar is draggable and includes explicit minimize, maximize/restore, and close controls.
- Existing `.partial.mp4` files are detected at startup and preserved. The explicit recovery action uses the same safe rename model as Windows and refuses to overwrite an existing final MP4.
- PipeWire, camera, audio, encoder, and output failures are classified in the UI and recorded as a `FAULT` block in diagnostics. The app never reopens a portal or device automatically; the user explicitly selects **Record** to retry.
- The default destination is `~/Videos/Screen Recordings`.

## Validation

```bash
./verify-local.sh
```

This is the required local gate before merging Fedora app changes: it builds the
app, runs every GoogleTest, verifies the GTK4 preview sink and desktop metadata,
checks the RPM helper syntax, and produces a native RPM. The repository-owned
Fedora and Windows GitHub Actions workflows are intentionally disabled; checks
are performed on the Fedora development laptop instead.

After a window-shell change, also run this graphical-session smoke test. It
starts the real application for four seconds with preview disabled, and fails
if GTK rejects the window configuration:

```bash
./verify-ui-runtime.sh
```

For this Intel Iris Xe Fedora laptop, run the optional hardware-path gate after
the portable check:

```bash
sudo dnf install ffmpeg-free
./verify-intel-va.sh
```

It uses the app's private refreshed GStreamer registry, records a one-second
VA low-power H.264 clip, and verifies its codec, 848×480 dimensions, and 30 FPS
with `ffprobe`. It is intentionally Intel-specific and is not required on
systems that use the OpenH264 fallback.
