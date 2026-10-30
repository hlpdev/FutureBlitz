# futureblitz.dev

The design of futureblitz.dev, planned with the user on 2026-10-24, replanned on 2026-10-29, and
its look settled on 2026-10-30: M9 builds the site and the documentation, and M10 the package
registry, its API, accounts, and the package site, with the package manager. All of it is on
Cloudflare. Nothing is hosted yet; this is the plan.

## Decided

1. **Two milestones, M9 and M10**, after M8's tooling: M9 the site and docs, which
   need M8's releases, `fb doc`, the grammar, and the LSP; and M10 packages online, the registry,
   accounts, and the package site with the package manager, built together, on M9's design
   system. Phase 1 ends with M10; the engine is a phase of its own (the user, 2026-10-29).
2. **Terminal noir with a Blitz nod** (the user, 2026-10-30): a developer's site, beautiful and
   restrained, in one dark look with no theme switch: near-black with a faint grid, monospace
   where it helps, the classic IDE's blue for links and highlights and amber for the one main
   button, and light retro touches throughout (the pixel wordmark and section labels, scanline
   dividers, the CRT in the hero).
3. **Accounts are GitHub or email with a passkey.** Publishing uses scoped API tokens, and CI
   publishes through GitHub's OIDC trusted publishing, with no stored secret.
4. **No playground** (the user, 2026-10-30): running visitors' code on servers costs compute, and
   people install fb to try it. Examples are code with a Copy button, and the site is static.
5. **Everything on Cloudflare**: Workers for the site and APIs, R2 for files, D1 for data, and
   Queues for background work; no Containers, since nothing runs fb on a server: `fb publish`
   writes a package's docs JSON on the publisher's machine.

## Addresses

| Address | What |
|---|---|
| `futureblitz.dev` | The landing page, `/learn`, `/docs`, `/std`, `/download` |
| `pkg.futureblitz.dev` | The package site: search, package pages, accounts, the dashboard, and `/api/v1` |
| `index.futureblitz.dev` | The sparse index fb reads: one file per package, cached at the edge |
| `dl.futureblitz.dev` | Package archives and fb's own releases, immutable, from R2 |

## Look and feel

Settled with the user on 2026-10-30, from mockups: direction B, terminal noir, with direction A's
retro touches.

- **Palette:** near-black page (`#08090D`) under a faint 24 px grid (`#11131A`) in the hero, cards
  a step up (`#0C0E14`, borders `#1C1F28`); "Blitz blue" (`#6E8BFF` on dark, from the old IDE's
  `#10258A`) for links, highlights, section labels, and the active nav item; amber (`#F2A53A`) for
  the one main button on a page and the cursor; text `#F2F4FA`, secondary `#9AA1B5`, muted
  `#5D6478`. One theme, dark, with no switch; text meets WCAG AA.
- **Type:** Space Grotesk for headings and the hero's headline; IBM Plex Sans for text; JetBrains
  Mono for code, commands, and small labels; Silkscreen, a pixel face, only for the wordmark and
  section labels ("▸ WHAT YOU GET"). Fonts are self-hosted, so no third party sees a visit.
- **Logo** (the user, 2026-10-30): an F whose upright is an amber lightning bolt, leaning
  forward, with white arms, every edge at one angle; on near-black for icons (the favicon, the
  Discord server, the VS Code extension). The wordmark is that F and then "uture" in white and
  "Blitz" in the bolt's amber, in Space Grotesk Bold leaning at the mark's angle. The files are in
  FutureBlitz's assets/brand/, made by tools/brand/make_brand.py.
- **Retro, lightly, throughout:** the pixel wordmark and section labels, thin scanline dividers
  between sections, and a CRT monitor in the hero (rounded bezel, faint scanlines, a cursor); no
  curvature, glow, or noise elsewhere, and nothing moves with `prefers-reduced-motion`.
- **Density:** the landing page is airy, with few words; the docs and package pages are dense
  and scannable.
- **Nav:** Learn, Docs, Std, Packages, and Discord, with the amber Install button at the right.
- **Code is the star:** FutureBlitz highlighting from the one TextMate grammar shared with the VS
  Code extension (M8), in a theme of the palette: keywords blue, strings and numbers amber,
  functions pale blue, comments `#7C8399` (lighter than muted text, so they meet AA).

## The landing page

The page is about the language. The engine is a separate library, shown nowhere on it, and the
project's milestones are GitHub's business, not the page's.

- **Who reads it**, first to last: people who made games in Blitz3D or BlitzBasic and want that
  feeling back; hobby and indie developers who want something simpler than C++ and faster than a
  scripting language; programmers curious about a new language.
- **Its one job:** get a visitor to install FutureBlitz and run `fb new` within a few minutes.
- **How it talks:** plainly and specifically, about making things, not about features. No
  "blazing", "seamless", "powerful", or "modern". Every claim comes with code, a real number, or
  a real name (a std module, a bundled library, a Blitz3D command). Nothing is invented: no
  testimonials, no made-up packages, no benchmark it can't show.
- **Its sections, in order:**
  1. **Hero.** On the grid, `$ fb new game && fb run game` in mono, then the headline "Write it
     like BASIC. Ship it like C." (its second sentence in blue) and one line under it, beside a
     CRT monitor where a short program types itself and then its real output appears, recorded
     when the site is built, not run on a server. It never changes size, and it's still with
     `prefers-reduced-motion`. Buttons: "Install FutureBlitz" (amber) and "Take the tour".
  2. **What you get.** Six tiles, each a short claim with its technical line: native code through
     LLVM, one file to ship, threads the compiler checks, coroutines, the standard library, any C
     library.
  3. **From Blitz3D.** One small program in both languages side by side, and four notes on what
     changed, with a link to the guide for Blitz3D users.
  4. **The samples folder.** Blitz3D came with a folder of samples that taught a generation;
     FutureBlitz has one too. Five samples, each a small story from making a game: a boss fight as
     a coroutine, a save file in JSON, heavy work on another thread, particles over every core,
     and a window from a C library. Picking one shows its code in an IDE window, and its
     recorded output below it.
  5. **In the box.** The standard library as a shelf of its 21 modules, each with what it's for,
     and the C libraries std links in (SQLite, libcurl, mbedTLS, zlib-ng, zstd, PCRE2, and the
     rest), so a program that uses them is still one file.
  6. **Get started.** The install line for each OS, then `fb new`, `fb run`, and the three places
     to go next: the tour, the docs, and the guide for Blitz3D users.
- **Layout:** each section has its own shape rather than the same title, paragraph, and grid: the
  hero is split, the comparison is two windows, the samples are a list beside a window, the std is
  a dense shelf, and getting started is a terminal. Section heads are short and say what's there.
- **Budget:** the page is static, under 100 KB of JavaScript, the hero's animation starting after
  the first paint; Largest Contentful Paint under 1.5 s on a mid phone; with
  `prefers-reduced-motion`, nothing moves and every section is still complete.

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
- **Every example runs** in CI, compiled and run as doc tests, so none goes stale; each has a
  Copy button.
- **The docs' layout:** a left nav of the section's pages, the content, and an "On this page"
  column; a version picker beside the wordmark and search on ⌘K; a std page gives a type's
  module, its summary, an example, and each member's signature and doc.
- **Versioned:** `/docs/latest/` and `/docs/<version>/` for each release; search with Pagefind,
  static and on the client, over the version being read; no "Edit this page" (the docs live in the
  private web repo), but a "Report an issue" link to FutureBlitz's issues. The policies' contact is
  the issues and the Discord, with no email address.

## The site's build

- Astro builds the site as static files: prose in MDX, Shiki for highlighting with the
  FutureBlitz grammar, Pagefind for search, and a small Svelte 5 island for the hero. Styles are plain CSS over design tokens (custom properties). The docs' content
  lives in this repo; the site's code in its own private repo, `hlpdev/futureblitz-web`, with the
  package site and the API, so a bug in them isn't on show (the user, 2026-10-30).
- `futureblitz-web` is a pnpm monorepo: `apps/site` (futureblitz.dev), `apps/pkg` (the package
  site, Astro rendered on Workers through its Cloudflare adapter), `workers/api`,
  `packages/ui` (the tokens and components both sites share), and `packages/grammar` (the
  FutureBlitz grammars).
- Served by Workers static assets. Deploys run with Wrangler from the developer's machine, signed
  in to the user's Cloudflare account with `wrangler login`, so no API token is stored anywhere;
  GitHub Actions builds and tests each push.
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

The index's lines, config.json, the archives' checks, and the API's requests and responses are in
[registry-api.md](registry-api.md), mirrored from futureblitz-web's docs/registry-api.md.

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
- **Queues** run what follows a publish: render the readme, store the docs JSON `fb publish`
  uploads (`fb doc`'s, made on the publisher's machine), update search, and purge the index's
  cache.
- **Download counts** go to Workers Analytics Engine, and a daily job rolls them into D1.
- **Email** (verification, new-publish and ownership notices) through Cloudflare's Email Service.
- **Abuse:** Turnstile on sign-up; rate limits on the API.

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
- **A package's page:** a header with its name, version, summary, and `fb add` line with Copy;
  tabs for its readme, docs (from its uploaded `fb doc` JSON, versioned), versions, and
  dependents; and a facts column of its license, targets, versions, links, and downloads.
- **People and teams:** their packages; the dashboard for tokens, trusted publishers, owned
  packages, invites, and the audit log.
- **Policies:** the terms, privacy, package policy (names, squatting, malware, yanking), the code
  of conduct, and how to report a package.

## The command line (built in M10)

- `fb login` opens the browser, signs in, and stores a token in the OS's credential store
  (Windows Credential Manager, the macOS keychain, libsecret), or reads `FB_REGISTRY_TOKEN` in CI.
- `fb publish` builds the archives and the docs JSON, runs the publish checks locally first,
  and `--dry-run` stops before uploading; `fb yank`, `fb owner`, and `fb search`.
- `fb add`, `fb update`, and builds read the sparse index with HTTP caching, verify each archive's
  SHA-256 against `fb.lock`, and work offline from the cache; `[registries]` in project.toml names
  another registry, a mirror or a company's own.

## Running it

- **Two environments:** staging (`staging.futureblitz.dev`, `staging-pkg.futureblitz.dev`) and
  production, each with its own D1, R2, Workers, and GitHub OAuth app; a release promotes
  staging's build.
- **Backups:** D1's Time Travel, and a nightly export of D1 to R2; R2 keeps every archive, which is
  never deleted but for the policy's reasons.
- **Monitoring:** Workers logs and Analytics Engine, alerts on errors, and a status page.
- **Cost** to start: the static site fits Workers' free plan; the registry's API, D1, R2, and
  Queues fit their free tiers at first, and Workers' paid plan ($5 a month) when they don't; R2
  storage is about $0.015 per GB a month with free egress. Nothing runs fb on a server.

## M9's build order

| Step | What | Done when |
|---|---|---|
| 0 | The design system: palette, type, the logo, the CRT hero, and components, in the one dark theme | a page of every component passes WCAG AA |
| 1 | The site's skeleton: Astro, the FutureBlitz grammar in Shiki, deploys to staging | futureblitz.dev's staging serves the landing page and a docs page |
| 2 | The landing page, with its hero, samples, and recorded outputs, and the download page over M8's releases | it meets its budget on a mid phone |
| 3 | The docs: Learn, guides, the spec, std's reference from `fb doc`, versions, search | every page renders, and every example compiles and runs in CI |
| 4 | Operations: production, monitoring, the status page, and the policies | alerts fire on errors |
| 5 | Loose ends, and the milestone's review | M9's "done when": the site and its docs are live |

M10's build order, the registry, accounts, the package site, and the package manager, is in
[milestones.md](milestones.md).
