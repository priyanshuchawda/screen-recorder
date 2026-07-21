Name:           fedora-screen-recorder
Version:        0.1.5
Release:        1%{?dist}
Summary:        Power-aware GNOME screen recorder for Fedora
License:        MIT
URL:            https://github.com/priyanshuchawda/screen-recorder
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(gstreamer-1.0)
BuildRequires:  pkgconfig(libadwaita-1)
BuildRequires:  pkgconfig(libportal)
BuildRequires:  pkgconfig(libportal-gtk4)

Requires:       gstreamer1-plugin-gtk4
Requires:       gstreamer1-plugin-libav
Requires:       gstreamer1-plugin-openh264
Requires:       gstreamer1-plugins-bad-free
Requires:       gstreamer1-plugins-good
Requires:       pipewire
Requires:       xdg-desktop-portal
Requires:       xdg-desktop-portal-gnome

%description
Fedora Screen Recorder is a native C++20 GNOME/Wayland screen recorder. It
uses the desktop portal and PipeWire for consented capture and GStreamer for
power-aware H.264/AAC MP4 recording.

%prep
%autosetup

%build
%cmake -G Ninja -DBUILD_TESTING=OFF
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%{_bindir}/fedora-screen-recorder
%{_datadir}/applications/io.github.screenrecorder.Fedora.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.screenrecorder.Fedora.svg
%{_metainfodir}/io.github.screenrecorder.Fedora.metainfo.xml

%changelog
* Tue Jul 21 2026 ScreenRecorder contributors <contributors@screenrecorder.invalid> - 0.1.5-1
- Keep the live camera preview at 720p and 30 FPS on every power profile.

* Tue Jul 21 2026 ScreenRecorder contributors <contributors@screenrecorder.invalid> - 0.1.4-1
- Keep the live camera preview visible during a camera PiP recording.

* Thu Jul 16 2026 ScreenRecorder contributors <contributors@screenrecorder.invalid> - 0.1.3-1
- Use the approved compact rectangular camera preview as the default.

* Thu Jul 16 2026 ScreenRecorder contributors <contributors@screenrecorder.invalid> - 0.1.2-1
- Add a DNF-based RPM installer helper and reliable GNOME launcher guidance.
- Refresh user-local desktop and icon caches after source installation.

* Wed Jul 15 2026 ScreenRecorder contributors <contributors@screenrecorder.invalid> - 0.1.1-1
- Add draggable window controls and default live camera preview.
- Add a graphical startup smoke test for the custom window shell.

* Wed Jul 15 2026 ScreenRecorder contributors <contributors@screenrecorder.invalid> - 0.1.0-1
- Initial Fedora package
