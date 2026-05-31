// Helper to format time as HH:MM:SS
const formatTime = (secs) => {
  const h = String(Math.floor(secs / 3600)).padStart(2, "0");
  const m = String(Math.floor((secs % 3600) / 60)).padStart(2, "0");
  const s = String(secs % 60).padStart(2, "0");
  return `${h}:${m}:${s}`;
};

/**
 * Initialize the interactive screen recorder mock widget
 */
export const initSimulator = () => {
  const btnRec = document.getElementById("sim-btn-rec");
  if (!btnRec) return;

  const btnText = document.getElementById("sim-btn-text");
  const statusPill = document.getElementById("sim-status-pill");
  const statusText = document.getElementById("sim-status-text");
  const screenPreview = document.getElementById("sim-screen-preview");
  const recTimeEl = document.getElementById("sim-recording-time");
  const recFpsEl = document.getElementById("sim-recording-fps");
  
  // Toggles
  const toggleCam = document.getElementById("sim-toggle-cam");
  const toggleMic = document.getElementById("sim-toggle-mic");
  const toggleHq = document.getElementById("sim-toggle-hq");
  const camOverlay = document.getElementById("sim-cam-overlay");
  const waveContainer = document.querySelector(".wave-container");

  let isRecording = false;
  let secondsElapsed = 0;
  let timerInterval = null;

  // Toggle camera visibility
  const updateCamVisibility = () => {
    if (toggleCam) {
      if (toggleCam.checked) {
        camOverlay.style.display = "flex";
      } else {
        camOverlay.style.display = "none";
      }
    }
  };

  if (toggleCam) {
    toggleCam.addEventListener("change", updateCamVisibility);
    updateCamVisibility(); // Initial check
  }

  // Toggle microphone state (fades wave bar visual if muted)
  const updateMicState = () => {
    if (waveContainer && toggleMic) {
      waveContainer.style.opacity = toggleMic.checked ? "1" : "0.15";
    }
  };
  if (toggleMic) {
    toggleMic.addEventListener("change", updateMicState);
  }

  // Toggle HQ mode (changes display to 60 FPS)
  const updateHqState = () => {
    if (recFpsEl && toggleHq) {
      recFpsEl.textContent = toggleHq.checked ? "60 FPS" : "30 FPS";
    }
  };
  if (toggleHq) {
    toggleHq.addEventListener("change", updateHqState);
  }

  // Action Button Click
  btnRec.addEventListener("click", () => {
    const recorderContainer = btnRec.closest(".sim-recorder");
    isRecording = !isRecording;

    if (isRecording) {
      // Start recording
      secondsElapsed = 0;
      recTimeEl.textContent = formatTime(0);
      
      screenPreview.classList.add("recording");
      if (recorderContainer) recorderContainer.classList.add("active-rec");
      
      if (btnText) btnText.textContent = "Stop Recording";
      
      if (statusText) statusText.textContent = "Recording...";
      if (statusPill) statusPill.className = "status-pill status-recording";

      // Lock configuration switches during recording
      if (toggleHq) toggleHq.disabled = true;
      
      timerInterval = setInterval(() => {
        secondsElapsed++;
        if (recTimeEl) recTimeEl.textContent = formatTime(secondsElapsed);
      }, 1000);
    } else {
      // Stop recording
      clearInterval(timerInterval);
      
      screenPreview.classList.remove("recording");
      if (recorderContainer) recorderContainer.classList.remove("active-rec");
      
      if (btnText) btnText.textContent = "Start Recording";
      
      if (statusText) statusText.textContent = "Ready";
      if (statusPill) statusPill.className = "status-pill status-ready";

      if (toggleHq) toggleHq.disabled = false;
    }
  });
};
