# ScreenRecorder website

The public ScreenRecorder website is a static Astro site isolated from the native Windows and Fedora applications.

## Local development

Requires Node.js 20.3+ and pnpm.

```sh
pnpm install
pnpm dev
pnpm build
```

## Deployment

Cloudflare Pages project: `screenrecording` at `https://screenrecording.pages.dev`. Build with `pnpm build`, then deploy with `pnpm deploy:pages`. The canonical URL is centralized in `astro.config.mjs`. Preview URLs should not be indexed.

Release links and confirmed product details live in `src/data/site.ts`. Windows v0.3.16 is linked directly; Fedora links to the release page because the RPM asset/version must be selected there.
