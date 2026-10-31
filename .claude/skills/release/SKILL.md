---
name: release
description: Release a FutureBlitz version (a patch like 1.0.1 or a minor like 1.1.0) end to end - the version bump, the tag, release.yml, publishing the draft, publish.yml's channels, futureblitz.dev's production site, the launch check, and what depends on fb's version. Use whenever a FutureBlitz version is to be released, published, or tagged, or when asked what a release still needs.
---

# Releasing FutureBlitz

docs/release-setup.md's "A release, then" is the checklist; this is how each step is done.
Every step happens, in order, for every release: a release isn't done until the launch check
passes, futureblitz.dev included.

**Approval.** Tagging, publishing the release, publish.yml, and production's deploy are public.
Ask the user once for the release ("Tag and publish vX.Y.Z?") and go on only on their yes, in
this conversation. Pushing the version bump to master needs no more than CLAUDE.md's verified
step.

`V` below is the version, like `1.0.1`.

## 1. The version

- `CMakeLists.txt`: `project(FutureBlitz VERSION V ...)`.
- `editors/vscode/package.json`'s `"version"`, and `editors/vscode/package-lock.json`'s two (the
  top and `packages[""]`), as commit 096e6d92 did for 1.0.0.
- `docs/history.md`: a `## V` entry at the top, with what the release fixes or adds.
- CLAUDE.md's Current step, where it names the released version.
- Verify as CLAUDE.md says, then commit `version V [skip ci]` as the user and push master.

For a minor, also check that everything the release's history lists is on master, that no
lane's branch it needs is still out, and that the site documents it (step 4).

## 2. The tag and release.yml

```bash
git tag vV <the version commit's sha>
git push origin vV
gh workflow run release.yml --ref vV   # the bump's [skip ci] means the push started nothing
```

Wait for the run in the background, never with a foreground sleep:

```bash
while [ "$(gh run view <id> --json status -q .status)" != completed ]; do sleep 240; done
gh run view <id> --json conclusion,jobs -q '.conclusion, (.jobs[] | "\(.name): \(.conclusion)")'
```

It takes about 25 minutes. A failing job: read its log (`gh api repos/hlpdev/FutureBlitz/actions/jobs/<job>/logs`),
fix it on master as a verified step, move the tag to the fix (`git tag -f`, `git push -f origin
vV`, after the user's yes), and start release.yml again.

## 3. The draft, and publish.yml

- The draft has the same files as the last release, with the version changed:
  ```bash
  diff <(gh release view vPREV --json assets -q '.assets[].name' | sed 's/PREV/X/g' | sort) \
       <(gh release view vV --json assets -q '.assets[].name' | sed 's/V/X/g' | sort)
  ```
- Publish it: `gh release edit vV --draft=false --latest`.
- Every channel:
  ```bash
  gh workflow run publish.yml --ref master -f version=V -f winget=true -f homebrew=true -f linux=true -f vscode=true
  ```
  and wait for it as above. Its winget job opens a pull request to microsoft/winget-pkgs; note
  its link (`gh pr list -R microsoft/winget-pkgs --author hlpdev`).

## 4. futureblitz.dev

The site is hlpdev/futureblitz-web, checked out beside this repo. Its `production` branch is
what futureblitz.dev serves: the newest release's docs. `master` is staging's and follows
FutureBlitz's master, so it can document features no release has yet. Production is never
deployed from master unless master documents exactly the release. The `site` skill has the
commands for building, checking, and deploying.

- **Every release:** `packages/ui/src/facts.js`'s `version` is `V`, on both branches. Every page
  reads it there, and the copy check fails on a page that says another.
- **A patch:** on `production`, set facts.js's version, then record the outputs with the
  release's own fb, downloaded from the release (never the user's installed fb or a dev build):
  ```bash
  gh release download vV -p 'futureblitz-V-windows-x64.zip' -p SHA256SUMS -D <scratch>
  FB=<scratch>/futureblitz-V-windows-x64/bin/fb.exe FUTUREBLITZ_ROOT=/c/nonexistent npx pnpm@9.15.0 record
  ```
- **A minor:** `production` moves to master (merge master into it), with the site synced to the
  tag: `FUTUREBLITZ_ROOT=<a checkout at vV> FB=<the release's fb> npx pnpm@9.15.0 sync`, then
  `record`.
- Test, build for production (no `SITE_ENV`), the copy check, the page tests; commit and push
  the branch; then deploy production (`npx wrangler deploy` in apps/site) and staging from
  master.

## 5. The launch check

```bash
python tools/release/launch_check.py V
```

Every line passes but winget's while its pull request waits for Microsoft's moderators; run it
again once it's merged. It checks the release's files, SHA256SUMS, the installers' signatures,
winget, Homebrew, futureblitz.dev's download page, and apt's and dnf's repositories.

## 6. What depends on fb's version

- hlpdev/fbengine's `FB_VERSION` in `.github/workflows/ci.yml` and `publish.yml`, and anything
  it moved off a target until this release (as its bindings check went Windows-only for 1.0.1).
  The engine's lanes own fbengine; tell the session that lands their steps.
- The VS Code extension went out with publish.yml's vscode job.

## 7. The report

The release's link, each channel's state, the winget pull request, the launch check's output,
and anything left for the user.
