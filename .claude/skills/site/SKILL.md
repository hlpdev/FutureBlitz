---
name: site
description: Update, check, and deploy futureblitz.dev (hlpdev/futureblitz-web) - syncing the spec and std's reference after a language or std change, recording examples' outputs, the tests and copy check, and which branch staging and production deploy from. Use whenever the site, its docs, Learn, the download page, or facts.js change, or when deploying staging or production.
---

# futureblitz.dev

The site is hlpdev/futureblitz-web, at `../futureblitz-web` beside this repo (private), in
`apps/site/`; its README has the details. Every language, std, or tooling change lands with the
site's pages it touches, on staging.

## Which branch is which

- `master` serves **staging.futureblitz.dev** and follows FutureBlitz's master.
- `production` serves **futureblitz.dev**: the newest release's docs, at its version. It moves
  only at a release (the `release` skill), never with an unreleased feature.

A fix to the site itself (a typo, a broken page) goes to master, and to `production` too
(cherry-pick) when production has the page.

## Bringing a change in (master)

From `apps/site/`, with this repo built (cmake-build-debug's fb) or `FB` set:

```bash
npx pnpm@9.15.0 sync          # the spec and std's reference, from FUTUREBLITZ_ROOT (default ../FutureBlitz)
npx pnpm@9.15.0 record        # each example's output, with that fb
npx pnpm@9.15.0 check:examples
```

Learn's tutorials, guides, and the reference are edited by hand under `src/content/`. Versions,
targets, commands, hosts, and requirements come only from `packages/ui/src/facts.js`.

## Checks, before every push

```bash
npx pnpm@9.15.0 install --frozen-lockfile
npx pnpm@9.15.0 --filter @futureblitz/grammar --filter @futureblitz/ui --filter @futureblitz/deepmerge --filter @futureblitz/site test
npx pnpm@9.15.0 build          # in apps/site; SITE_ENV=staging for staging's
npx pnpm@9.15.0 check:copy
npx pnpm@9.15.0 test:e2e
```

`build` runs `record --if-fb`, which re-records every output when the fb it finds isn't the one
they were recorded with. To build without re-recording, as CI does, set
`FUTUREBLITZ_ROOT=/c/nonexistent` and leave `~/.futureblitz/bin` off PATH. Commit as the user,
as `site: ...`, and push.

## Deploying

From `apps/site/`, signed in with `wrangler login`:

```bash
SITE_ENV=staging npx pnpm@9.15.0 build && npx wrangler deploy --env staging   # master
npx pnpm@9.15.0 build && npx wrangler deploy                                  # production, from the production branch
```

Production's deploy is public: only with the user's yes, from a checkout of `production`
(`git worktree add ../futureblitz-web-production production`). Afterwards,
`curl -fsS https://futureblitz.dev/download/ | grep -o "FutureBlitz [0-9.]*"` shows its version,
which `tools/release/launch_check.py` checks too.
