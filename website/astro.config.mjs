import { defineConfig } from "astro/config";
import sitemap from "@astrojs/sitemap";

export default defineConfig({
  site: "https://screenrecording.pages.dev",
  integrations: [sitemap()],
  output: "static",
});
