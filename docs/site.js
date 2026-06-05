import { loadRepoStats, loadLatestRelease } from './modules/github-api.js';
import { initSimulator } from './modules/simulator.js';

(() => {
  "use strict";

  const setFooterYear = () => {
    const year = document.getElementById("year");
    if (year) year.textContent = String(new Date().getFullYear());
  };

  const init = () => {
    // Basic setup
    setFooterYear();
    
    // GitHub API integrations
    loadRepoStats();
    loadLatestRelease().catch((err) => {
      console.warn("GitHub release load failed, using fallback release URL", err);
      // fallback details
      const setDownload = (href) => {
        for (const id of ["primary-download", "asset-download"]) {
          const node = document.getElementById(id);
          if (node) node.setAttribute("href", href);
        }
      };
      const setText = (id, text) => {
        const node = document.getElementById(id);
        if (node && text !== undefined) node.textContent = text;
      };
      setDownload("https://github.com/priyanshuchawda/screen-recorder/releases/latest");
      setText("release-version", "v0.3.10");
      setText("release-asset", "ScreenRecorder-0.3.10-windows-x64.zip");
      setText("release-meta", "Open the latest release and choose the Windows x64 ZIP asset.");
    });

    // Mock Simulator setup
    initSimulator();
  };

  // Launch on load
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
