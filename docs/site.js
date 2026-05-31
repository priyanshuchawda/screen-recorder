(() => {
      "use strict";

      const repository = "priyanshuchawda/screen-recorder";
      const latestReleaseUrl = `https://github.com/${repository}/releases/latest`;
      const releaseApiUrl = `https://api.github.com/repos/${repository}/releases/latest`;
      const assetNamePattern = /windows-x64\.zip$/i;

      const formatBytes = (bytes) => {
        if (!Number.isFinite(bytes) || bytes <= 0) return "";
        return new Intl.NumberFormat(undefined, {
          maximumFractionDigits: 1
        }).format(bytes / (1024 * 1024)) + " MB";
      };

      const formatDate = (value) => {
        const date = new Date(value);
        if (Number.isNaN(date.getTime())) return "";
        return new Intl.DateTimeFormat(undefined, {
          year: "numeric",
          month: "short",
          day: "numeric"
        }).format(date);
      };

      const setText = (id, text) => {
        const node = document.getElementById(id);
        if (node && text) node.textContent = text;
      };

      const setDownload = (href) => {
        for (const id of ["primary-download", "asset-download"]) {
          const node = document.getElementById(id);
          if (node) node.setAttribute("href", href);
        }
      };

      const loadLatestRelease = async () => {
        const response = await fetch(releaseApiUrl, {
          headers: { "Accept": "application/vnd.github+json" }
        });
        if (!response.ok) throw new Error(`GitHub release request failed: ${response.status}`);
        const release = await response.json();
        const assets = Array.isArray(release.assets) ? release.assets : [];
        const asset = assets.find((item) => assetNamePattern.test(String(item.name || "")));

        setText("release-version", release.tag_name || "Latest Release");
        if (asset && asset.browser_download_url) {
          setDownload(asset.browser_download_url);
          setText("release-asset", asset.name);
          const size = formatBytes(asset.size);
          const published = formatDate(release.published_at);
          const meta = [size, published && `published ${published}`].filter(Boolean).join(" - ");
          setText("release-meta", meta || "Direct Windows package link loaded from GitHub Releases.");
          return;
        }

        setDownload(latestReleaseUrl);
        setText("release-meta", "Open the latest release and choose the Windows x64 ZIP asset.");
      };

      const setFooterYear = () => {
        const year = document.getElementById("year");
        if (year) year.textContent = String(new Date().getFullYear());
      };

      setFooterYear();
      loadLatestRelease().catch(() => {
        setDownload(latestReleaseUrl);
        setText("release-meta", "Open the latest release and choose the Windows x64 ZIP asset.");
      });
    })();

