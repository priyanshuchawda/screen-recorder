import { defineConfig } from "astro/config";
import sitemap from "@astrojs/sitemap";

export default defineConfig({
  site: "https://screen-recorder-windows.netlify.app",
  integrations: [sitemap()],
  output: "static",
});
