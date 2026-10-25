# futureblitz.dev

M9's design, planned with the user on 2026-10-24: the site, the documentation, the playground,
the package registry, and accounts, all on Cloudflare, done at the end of phase 1. Nothing is
hosted yet; this is the plan.

## Decided

1. **Its own milestone, M9**, after M8's tooling (whose `fb add` and `fb publish` it serves) and
   before the engine, which moves to M10 to M17.
2. **A retro Blitz nod.** A modern site in one dark look, with no theme switch, and Blitz3D-era
   touches: the classic
   IDE's blue, a CRT and pixel accent kept to the hero and headings, and the story of where
   FutureBlitz comes from up front.
3. **Accounts are GitHub or email with a passkey.** Publishing uses scoped API tokens, and CI
   publishes through GitHub's OIDC trusted publishing, with no stored secret.
4. **A playground that runs code on the server**: a Run button on the site and on every docs
   example, compiled and run in a sandboxed container, with share links.
5. **Everything on Cloudflare**: Workers for the site and APIs, R2 for files, D1 for data,
   Queues and Containers for background work.

## Addresses

| Address | What |
|---|---|
| `futureblitz.dev` | The landing page, `/learn`, `/docs`, `/std`, `/play`, `/download`, `/blog`, `/community` |
| `pkg.futureblitz.dev` | The package site: search, package pages, accounts, the dashboard, and `/api/v1` |
| `index.futureblitz.dev` | The sparse index fb reads: one file per package, cached at the edge |
| `dl.futureblitz.dev` | Package archives and fb's own releases, immutable, from R2 |
| `play.futureblitz.dev` | The playground's run API (its editor is futureblitz.dev/play) |

## Look and feel

- **Palette:** "Blitz blue" (the old IDE's deep blue, about `#10258A`), a warm amber accent for
  calls to action and the cursor, and phosphor green for terminal output, on near-black
  (`#070A14`). The site has one theme, dark, with no switch, and its text meets WCAG AA contrast.
- **Type:** a pixel display face (Silkscreen) for the wordmark, the hero's headline, and section
  heads; IBM Plex Sans for text; JetBrains Mono for code. Fonts are self-hosted, so no third party sees a visit.
- **Retro, kept to its place:** the hero is a CRT monitor (curvature, scanlines, a soft glow, all
  off for `prefers-reduced-motion`); headings carry a small pixel bullet; the docs and package
  pages are calm and modern, with the blue in their chrome.
- **Code is the star:** FutureBlitz highlighting from one TextMate grammar shared with the VS Code
  extension (M8), in a theme drawn from the Blitz IDE's colors.

## The landing page

1. **Hero:** a CRT window where a Blitz3D program types itself out, then morphs into the
   FutureBlitz program that does the same, and the window runs it: a spinning lit cube in WebGL2
   (the engine's real demos replace it once the engine exists). Headline: "Blitz3D, grown up."
   Buttons: Download, Learn, Try it in the browser.
2. **What it is**, in one line each: native executables through LLVM; one file to ship; safe
   threads (Sendable, Tasks, Parallel For); a standard library with batteries; C libraries through
   bindgen; a game engine on the way.
3. **From Blitz3D:** the same small game in Blitz3D and in FutureBlitz, side by side, and what
   changed (types, classes, modules, errors, async), with a link to the migration guide.
4. **Code tabs:** Hello, a game loop with coroutines, JSON over HTTPS, a Parallel For, calling C:
   each with a Run button.
5. **One executable:** a build's real output, an .exe and its size, with "no runtime to install".
6. **Packages:** a few from the registry, with `fb add` lines.
7. **Get started:** install commands per OS (winget, Homebrew, a shell script, a zip), then the
   footer: docs, std, packages, GitHub, community, the license.

Budget: the page is static, under 100 KB of JavaScript besides the hero's canvas, which loads
after the first paint; Largest Contentful Paint under 1.5 s on a mid phone.

## Documentation

- **Learn:** install; a tour of the language in short pages, each with runnable examples; and a
  tutorial that builds a small game, then a CLI tool, then a web service.
- **Guides** by topic: types and nullability, Classes and Structs, errors, generics and
  Interfaces, concurrency and async, modules and packages, C libraries and bindgen, testing,
  release builds and shipping, and coming from Blitz3D (docs/migration.md).
- **Reference:** the language spec, rendered from docs/spec/; std's reference, generated from
  std's sources; the CLI and project.toml references.
- **Generated from the code:** `fb doc` (M8, with `///` doc comments) writes a package's public
  API as JSON: modules, types, members, signatures, docs, and source links. The site renders std's
  from it, and the package site renders every package's.
- **Every example runs**: CI compiles and runs each docs example (doc tests), so none goes stale,
  and each has Run and Copy buttons.
- **Versioned:** `/docs/latest/` and `/docs/<version>/` for each release; search with Pagefind,
  static and on the client, over the version being read; "Edit this page" links to GitHub.

## The site's build

- Astro builds the site as static files: prose in MDX, Shiki for highlighting with the
  FutureBlitz grammar, Pagefind for search, and small Svelte 5 islands for the hero and the
  playground. Styles are plain CSS over design tokens (custom properties). The docs' content
  lives in this repo; the site's code in its own repo, `futureblitz-web`.
- `futureblitz-web` is a pnpm monorepo: `apps/site` (futureblitz.dev), `apps/pkg` (the package
  site, Astro rendered on Workers through its Cloudflare adapter), `workers/api`, `workers/play`,
  `packages/ui` (the tokens and components both sites share), and `packages/grammar` (the
  FutureBlitz grammars).
- Served by Workers static assets. GitHub Actions builds on each push to master and deploys with
  Wrangler; each pull request gets a preview.
- Cloudflare Web Analytics (cookieless) only; no tracking, no cookie banner.

## The registry

- **Index:** a sparse index, as Cargo's: `index.futureblitz.dev/<name>` holds one JSON line per
  version (version, dependencies with ranges, the fb version it needs, its targets, each archive's
  SHA-256, yanked or not), and `config.json` says where downloads and the API are. fb fetches only
  the packages it needs, with ETags; the edge caches each file, and a publish purges it.
- **Archives:** `dl.futureblitz.dev/<name>/<version>/` holds `<name>-<version>.fbpkg` (project.toml,
  license, readme, source or .fbi) and, for prebuilt packages, one `<name>-<version>-<target>.fbpkg`
  per target with its .fblib builds and natives, so a game's build downloads only its targets.
  Archives are immutable and served from R2 with no egress cost.
- **Names:** what `Import` names, lowercase ASCII letters, digits, and `_`, starting with a letter,
  at most 64; flat, as crates.io's; `std`, `fb`, `futureblitz`, and names a `_` apart from another
  package's are refused.
- **Versions are immutable.** A version can be yanked (new resolutions skip it; locked ones still
  download), never replaced. A publisher can delete a version within 72 hours of publishing if
  nothing depends on it; after that, only an admin can, for legal or security reasons.
- **Limits:** source archives up to 10 MB and each target's up to 200 MB (more by request, as the
  engine will need), 30 publishes an hour per account.
- **Checks at publish:** the archive's paths stay inside it; project.toml parses and matches the
  upload; the version is new and is semver; the license is an SPDX expression; the readme
  renders; the .fbi's ABI hash matches each target's builds.

## The API and its storage

- **Workers:** `pkg.futureblitz.dev/api/v1` is one Worker in TypeScript (Hono, with Drizzle over
  D1, Zod for validation, SimpleWebAuthn for passkeys, and Arctic for GitHub sign-in): publish,
  yank, owners and teams, tokens, trusted publishers, search, package and version metadata, and
  download counts.
- **D1** (SQLite) holds users, their GitHub identities and passkeys, sessions, API tokens (hashed),
  packages, versions, owners, teams, trusted publishers, an audit log of every change, and reserved
  names; search is D1's FTS5 over names, descriptions, and keywords.
- **R2** holds the archives, the index's files, readmes, and generated docs.
- **A Durable Object per package** serializes its publishes, so two can't race on its index file.
- **Queues** run what follows a publish: render the readme, build its docs (`fb doc` in a
  Container), update search, and purge the index's cache.
- **Download counts** go to Workers Analytics Engine, and a daily job rolls them into D1.
- **Email** (verification, new-publish and ownership notices) through Cloudflare's Email Service.
- **Abuse:** Turnstile on sign-up and on heavy playground use; rate limits on the API.

## Accounts

- **Sign in with GitHub** (OAuth), or **create an account with an email and a passkey**
  (WebAuthn), its email verified by a link; an account can have both, and several passkeys.
- **Sessions** are secure, HttpOnly cookies on pkg.futureblitz.dev; the dashboard asks for a passkey
  or GitHub again before sensitive changes (tokens, owners, deleting).
- **API tokens** are scoped (publish new packages, publish to named packages, yank, manage
  owners), expire (90 days by default), and are shown once and stored hashed.
- **Trusted publishing:** a package's owner names a GitHub repository and workflow, and that
  workflow's OIDC token publishes it, with no secret stored in CI.
- **Teams:** an organization owns packages, and its members get roles (owner, publisher).
- **Recovery:** the verified email and recovery codes; deleting an account leaves its packages
  published, owned by their other owners, or marked unowned.

## The package site

- **Search** with filters (targets, keywords, recently updated), and category pages.
- **A package's page:** its readme, `fb add` line, versions, dependencies and dependents, targets
  supported, license, links, downloads over time, and its generated docs, versioned.
- **People and teams:** their packages; the dashboard for tokens, trusted publishers, owned
  packages, invites, and the audit log.
- **Policies:** the terms, privacy, package policy (names, squatting, malware, yanking), the code
  of conduct, and how to report a package.

## The command line (built in M8)

- `fb login` opens the browser, signs in, and stores a token in the OS's credential store
  (Windows Credential Manager, the macOS keychain, libsecret), or reads `FB_REGISTRY_TOKEN` in CI.
- `fb publish` builds the archives, runs the publish checks locally first, and `--dry-run` stops
  before uploading; `fb yank`, `fb owner`, and `fb search`.
- `fb add`, `fb update`, and builds read the sparse index with HTTP caching, verify each archive's
  SHA-256 against `fb.lock`, and work offline from the cache; `[registries]` in project.toml names
  another registry, a mirror or a company's own.

## The playground

- **The editor** is CodeMirror 6, on futureblitz.dev/play and inline in the docs, with examples to
  pick and a share button. Not Monaco: CodeMirror is about a tenth the size, works on phones, and
  is cheap to have many of on one page; it takes a small Lezer grammar of its own beside the
  TextMate one that VS Code and Shiki share, and shows the LSP's errors and hovers.
- **Running:** the editor posts the code to `play.futureblitz.dev`, a Worker that starts a
  Cloudflare Container from an image of fb and std for Linux x64, which compiles and runs it, and
  streams its output back over a WebSocket.
- **Sandboxed:** no network, a temporary folder only, 10 s of CPU, 256 MB of memory, 64 KB of
  output, the same limits in release mode, and one run at a time per visitor; std.net and processes
  fail as the OS refuses them.
- **Cached and shared:** a run is cached by its code's hash and fb's version; a share link stores
  the code in R2 under a short id.
- Graphics (the engine) come later, streamed as frames or through a WebAssembly build.

## Running it

- **Two environments:** staging and production, each with its own D1, R2, and Workers; a
  release promotes staging's build.
- **Backups:** D1's Time Travel, and a nightly export of D1 to R2; R2 keeps every archive, which is
  never deleted but for the policy's reasons.
- **Monitoring:** Workers logs and Analytics Engine, alerts on errors, and a status page.
- **Cost** to start: Workers' paid plan ($5 a month) covers the site and APIs; R2 storage is about
  $0.015 per GB a month with free egress; D1, Queues, and Containers are billed by use.

## M9's build order

| Step | What | Done when |
|---|---|---|
| 0 | The design system: palette, type, the CRT hero, and components, in the one dark theme | a page of every component passes WCAG AA |
| 1 | The site's skeleton: Astro, the FutureBlitz grammar in Shiki, deploys to staging | futureblitz.dev's staging serves the landing page and a docs page |
| 2 | The landing page, with its hero and code tabs | it meets its budget on a mid phone |
| 3 | The docs: Learn, guides, the spec, std's reference from `fb doc`, versions, search | every page renders, and every example compiles and runs in CI |
| 4 | Accounts: GitHub sign-in, email with passkeys, sessions, recovery | a person signs up each way and signs in again |
| 5 | The registry: index, archives, the publish API, tokens, trusted publishing, yanking | `fb publish` and a GitHub workflow each publish a package that `fb add` then builds with |
| 6 | The package site: search, package pages, generated docs, the dashboard | a published package's page shows its readme, versions, and docs |
| 7 | The playground: the editor, the run Worker, the container, sharing | the docs' examples run from the browser within the limits |
| 8 | Operations: production, backups, monitoring, the status page, the policies | a restore from backup works, and alerts fire on errors |
| 9 | Loose ends, and the milestone's review | M9's "done when": the site, docs, registry, accounts, and playground are live |
