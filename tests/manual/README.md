# Local Windows recording probe

The opt-in `recording_probe` exercises the production capture, microphone,
loopback, encoder, and muxer on the current Windows device. Its small animated
window provides a visible motion reference. It records the real desktop and
microphone, so use a private disposable directory and do not publish the output.
It never uploads a recording. The probe is not part of the default build or
CTest run.

From the repository root in PowerShell:

```powershell
cmake --build build --config Release --target recording_probe
$probeOutput = Join-Path $env:TEMP ('screen-recorder-probe-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $probeOutput | Out-Null
.\build\Release\recording_probe.exe $probeOutput 150 15
```

For hardware pause-path checks, add `--pause-resume` to pause for three seconds
and then resume, or `--stop-paused` to request stop while paused. Use at least
six seconds for either scenario; the probe requires a `complete` result.

```powershell
.\build\Release\recording_probe.exe $probeOutput 30 15 --pause-resume
.\build\Release\recording_probe.exe $probeOutput 30 15 --stop-paused
```

Keep the animated window visible and wait for the process to stop. The output
path and counters are printed to the console; the diagnostics file is under
`_diagnostics` in the supplied output directory. Check the entire video for
changing marker motion, listen to both audio sources, and compare stream
durations and packet timestamps with `ffprobe`. Check logs for stalled writes,
queue drops, and the stop/finalize summary. A run without audible input does
not establish microphone intelligibility or loopback quality.

To exercise the watchdog with an injected audio-write delay, build
the same probe and set the following variable only in the test shell before
running it:

```powershell
$env:SR_PROBE_STALL_AUDIO = '1'
$env:SR_PROBE_STALL_AUDIO_MS = '18000' # optional; 1–120000 ms, default 18000
$env:SR_PROBE_STALL_AUDIO_AFTER_WRITES = '1' # optional; default first audio write
.\build\Release\recording_probe.exe $probeOutput 60
Remove-Item Env:SR_PROBE_STALL_AUDIO
Remove-Item Env:SR_PROBE_STALL_AUDIO_MS -ErrorAction SilentlyContinue
Remove-Item Env:SR_PROBE_STALL_AUDIO_AFTER_WRITES -ErrorAction SilentlyContinue
```

Pass `15` as the optional third argument to use the 360p/15 fps profile;
omit it to use 1080p/60 fps. The watchdog should report `event=mux_write_stalled`, request a stop, and
finalize after the injected delay returns. The delay is compiled only into a
separate probe-only copy of the recording core, never the production app. The
probe exits with a failure if the requested injection was not actually triggered
or if the finalized outcome is not `incomplete` for the injected stall (the
bounded capture queue is expected to report the frames it could not retain).
A normal run must report `complete`.
A permanently blocked operating-system call cannot be safely
interrupted by this watchdog; the partial file may still require recovery.
