export const repository = "https://github.com/priyanshuchawda/screen-recorder";

export const releases = {
  windows: {
    version: "0.3.16",
    name: "ScreenRecorder-0.3.16-windows-x64.zip",
    url: `${repository}/releases/download/v0.3.16/ScreenRecorder-0.3.16-windows-x64.zip`,
    size: "129,117 bytes",
    sha256: "443c24a51ae57fe7a404ecd87e1e4ee9b66d42b4889ba39cef8766f4f9cfef7e",
    platform: "Windows 10/11 · x64",
  },
  fedora: {
    version: "0.1.5",
    name: "fedora-screen-recorder-0.1.5-1.fc44.x86_64.rpm",
    url: `${repository}/releases/download/fedora-v0.1.5/fedora-screen-recorder-0.1.5-1.fc44.x86_64.rpm`,
    size: "120,775 bytes",
    sha256: "d17819f2d01e5819802f524426145aaa8f605b83f10f5d902b3f0549d265c137",
    platform: "Fedora 44 · GNOME / Wayland",
    note: "x86_64 RPM · SHA-256 available below",
  },
};

export const features = [
  {
    number: "01",
    title: "Hardware first",
    text: "Prefers Intel Quick Sync H.264 when available, with a graceful software fallback.",
    icon: "◈",
  },
  {
    number: "02",
    title: "Desktop + mic audio",
    text: "Captures system output and microphone, aligns them by timestamps, and mixes to one stereo track.",
    icon: "〰",
  },
  {
    number: "03",
    title: "Your quality, your call",
    text: "Choose 360p, 480p, 720p or 1080p profiles with frame rate aware bitrates.",
    icon: "⌗",
  },
  {
    number: "04",
    title: "Camera when you need it",
    text: "Add an optional camera overlay with a deliberately efficient default profile.",
    icon: "▣",
  },
  {
    number: "05",
    title: "Recording diagnostics",
    text: "Every session can leave a timestamped sidecar to help explain pipeline errors and stalls.",
    icon: "⌁",
  },
  {
    number: "06",
    title: "No account, no upload",
    text: "Recordings are saved locally. No cloud account or upload service sits in the way.",
    icon: "↗",
  },
];

export const questions = [
  {
    q: "Is ScreenRecorder free?",
    a: "Yes. It is open source under the MIT License, with no subscription or account required.",
  },
  {
    q: "Which platforms are supported?",
    a: "The Windows app supports Windows 10/11 x64. A separate native build targets Fedora 44 with GNOME and Wayland.",
  },
  {
    q: "Does it record system audio?",
    a: "On Windows it can capture desktop output through WASAPI Loopback and the default microphone, then mix them into one stereo audio track.",
  },
  {
    q: "Where are recordings stored?",
    a: "Recordings are written to your chosen local output folder. The website does not receive your recordings.",
  },
  {
    q: "Can I change microphone and desktop levels afterward?",
    a: "The current Windows app mixes both sources into one stereo track, so their relative levels cannot be changed independently in a standard player.",
  },
];
