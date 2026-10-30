# The registry's wire format

What fb and the registry say to each other: the sparse index, config.json, the archives the API
checks, and every request and response of `pkg.futureblitz.dev/api/v1`. The server is
`workers/api`; fb's side is FutureBlitz's M10 (the `m10-client` lane). Settled 2026-10-29 for M10
step 0, from FutureBlitz's docs/website.md and docs/libraries.md; this file is mirrored to
FutureBlitz's docs/registry-api.md, and a change goes in both.

Every JSON key is snake_case. Every time is an RFC 3339 string in UTC, like
`"2026-10-29T18:00:00Z"`. Sizes are bytes. A SHA-256 is 64 lowercase hex digits.

## Addresses

| Address | What |
|---|---|
| `https://index.futureblitz.dev/config.json` | where downloads and the API are |
| `https://index.futureblitz.dev/<name>` | the package's index file |
| `https://dl.futureblitz.dev/<name>/<version>/<file>` | an archive |
| `https://pkg.futureblitz.dev/api/v1/...` | the API |

Staging is `staging-index.futureblitz.dev`, `staging-dl.futureblitz.dev`, and
`staging-pkg.futureblitz.dev`. Locally (`wrangler dev`) it's `index.localhost:8787`,
`dl.localhost:8787`, and `pkg.localhost:8787`, over http: one Worker answers all three by host.
On staging and production, a request over plain http is a `308` to the same address over https,
which keeps its method and body; nothing is answered over http.

dl.futureblitz.dev's `/apt/` and `/rpm/` are FutureBlitz's apt and dnf repositories, and fb's own
releases are there too, under `/fb/`, `/futureblitz/`, or `/releases/`. None of those is ever a
package's, since each is a reserved name.

## Names and versions

- **A package's name** is lowercase ASCII letters, digits, and `_`, starting with a letter, at most
  64: `^[a-z][a-z0-9_]{0,63}$`. Two names that are the same without their `_`s are one name to the
  registry, so `foo_bar` can't be published beside `foobar`. `std`, `fb`, `futureblitz`, `apt`,
  `rpm`, and `releases` are reserved, and so is any name that's one of them without its `_`s, so
  no package's folder on dl.futureblitz.dev is one of FutureBlitz's own (see Addresses).
- **A username** is lowercase letters, digits, and `-`, 1 to 39 long, not starting or ending with
  `-`. **A team's name** is the same.
- **A version** is semver 2.0: `MAJOR.MINOR.PATCH`, with an optional `-prerelease`, no leading
  zeros, and each number, a numeric prerelease part's too, at most 2^53 - 1. Build metadata
  (`+...`) is refused at publish, so two versions never differ by it alone. Versions order as
  semver says.
- **A version range** (a dependency's `req`, and project.toml's `fb`) is one or more comparators
  joined by `,`, all of which a version has to meet:
  - `1.2.3`, `1.2`, `1`, or with `^`: caret, the same leftmost nonzero part. `1.2` is
    `>=1.2.0, <2.0.0`; `0.4` is `>=0.4.0, <0.5.0`; `0.0.3` is `>=0.0.3, <0.0.4`; `0` is
    `>=0.0.0, <1.0.0`.
  - `~1.2.3` and `~1.2`: `>=1.2.3, <1.3.0`, `>=1.2.0, <1.3.0`; `~1` is `>=1.0.0, <2.0.0`.
  - `=1.2.3`: that version; `=1.2` is `>=1.2.0, <1.3.0`.
  - `>1.2.3`, `>=1.2`, `<2`, `<=1.4.0`: a missing part is 0, except that `>1.2` is `>=1.3.0`
    and `<=1.2` is `<1.3.0`.
  - `*`, `1.*`, `1.2.*`: any, `>=1.0.0, <2.0.0`, and `>=1.2.0, <1.3.0`.
  - A prerelease version meets a range only if one of its comparators names a prerelease of the
    same `MAJOR.MINOR.PATCH`, as Cargo's do: `^1.2.0-beta.1` takes `1.2.0-beta.2` and `1.2.1`,
    but `^1.2.0` takes no prerelease.
  - Spaces around `,` and after an operator are allowed. `||` and hyphen ranges aren't.

## config.json

```json
{
  "v": 1,
  "dl": "https://dl.futureblitz.dev/{name}/{version}/{file}",
  "api": "https://pkg.futureblitz.dev/api/v1",
  "oidc_audience": "pkg.futureblitz.dev"
}
```

- `dl` is a template: fb puts in the package's name, the version, and the archive's file name.
- `oidc_audience` is the `aud` a GitHub workflow asks its OIDC token for (see Trusted
  publishing).
- A key fb doesn't know is ignored, so more can come.

## The index

`GET https://index.futureblitz.dev/<name>` is the package's index file: one JSON object per line,
each a version, in the order they were published, with a `\n` after each.

```json
{"v":1,"name":"physics","version":"1.2.0","deps":[{"name":"math3d","req":"0.4"}],"fb":"0.9","targets":["linux-x64","windows-x64"],"archives":{"source":{"sha256":"…","size":48211},"linux-x64":{"sha256":"…","size":5120334},"windows-x64":{"sha256":"…","size":5023117}},"yanked":false}
```

| Key | What |
|---|---|
| `v` | the line's format, 1 |
| `name` | the package's name |
| `version` | its version |
| `deps` | its `[dependencies]`, each `{ "name", "req" }`, sorted by name; `req` as project.toml writes it |
| `fb` | the range of fb versions it needs, project.toml's `[project] fb`, or `null` |
| `targets` | the targets it has an archive of its own for, sorted; `[]` for a package whose source archive is all of it, which builds on any target |
| `archives` | each archive's SHA-256 and size: `source` always, and one per target in `targets` |
| `yanked` | whether new resolutions skip it; a locked one still downloads |

- A dependency from another registry has `"registry": "<that registry's index URL>"` too. The
  public registry refuses those at publish, so its own lines never have one, but a company's
  registry might.
- A key fb doesn't know is ignored. A line whose `v` is higher than fb knows is skipped, as if
  the version weren't there.
- An archive's file name follows from its key: `source` is `<name>-<version>.fbpkg`, and a
  target's is `<name>-<version>-<target>.fbpkg`.
- A version deleted (within 72 hours, or by an admin) is gone from the file, and its number is
  never published again.
- The response is `text/plain; charset=utf-8`, with an `ETag`, and `Cache-Control: no-cache`, so
  fb sends `If-None-Match` and gets a `304` when nothing changed. The edge can make the ETag weak
  (`W/"…"`) as it compresses; If-None-Match compares weakly, takes a list, and `*`. A name with no package is a
  `404`. The edge caches each file; a publish, yank, or delete purges it.

## Downloads

`GET https://dl.futureblitz.dev/<name>/<version>/<file>` is the archive, `application/gzip`,
`Cache-Control: public, max-age=31536000, immutable`, with `ETag: "<sha256>"`; If-None-Match
gives a `304` as on the index. fb checks the SHA-256 against the index line or fb.lock. A yanked
version's archives still download; a deleted one's are a `404`, and so is any path that isn't an
archive's, escaped or not. Each `GET` that sends the archive counts a download.

## Archives

A `.fbpkg` is a gzip-compressed tar with its files at its root, not under a folder. The registry
takes only the part of tar that every extractor reads alike, so the paths it checks are the paths
that get extracted:

- every header is POSIX ustar (magic `ustar\0`, version `00`), with a valid checksum; GNU's
  (`ustar  \0`) and old tars' are refused, and so are GNU long names (`L`, `K`);
- an entry is a file (`0`) or a folder (`5`), and a folder's size is 0;
- an entry has at most one pax header (`x`) before it, whose keys are only `path`, `size`,
  `mtime`, `atime`, `ctime`, `uid`, `gid`, `uname`, `gname`, `hdrcharset`, and `comment`, each
  once; a global header (`g`), `linkpath`, and `GNU.sparse.*` are refused;
- a header's size and checksum are octal digits, after any spaces, and then only NULs or
  spaces; a base-256 or empty field, or one with a NUL first, is refused (a pax `size` takes a
  bigger file);
- after the end marker, only zeros.

- **The source archive**, `<name>-<version>.fbpkg`: `project.toml`, the license, the readme, and
  either the package's `.fb` source or, for a prebuilt package, no `.fb` files at all. Its
  `lib/<target>/` and `native/<target>/` go in the target's archive instead.
- **A target's archive**, `<name>-<version>-<target>.fbpkg`: only `lib/<target>/` and
  `native/<target>/`. A prebuilt package's has `lib/<target>/debug/<name>.fblib` and `.fbi` and
  the same in `release/`; a package with natives has its `native/<target>/` files.
- A target is `windows-x64`, `linux-x64`, `linux-arm64`, `macos-arm64`, or `macos-x64`.

### Checks at publish

The API refuses an archive, with an error naming the file and why, when:

- **Paths:** an entry is anything but a file or a folder (a link, a device, a FIFO); a path is
  absolute, has a drive letter, `\`, a NUL, a control character, `<>:"|?*`, an empty part, or a
  `.` or `..` part, or a part that ends in `.` or a space, or is a name Windows reserves (`CON`,
  `PRN`, `AUX`, `NUL`, `CONIN$`, `CONOUT$`, `COM0`–`COM9`, `LPT0`–`LPT9`, and `COM`/`LPT` with
  `¹`, `²`, or `³`, with any extension); a path is longer than 255 bytes, or isn't UTF-8; two
  paths are the same to a disk that ignores letter case (compared after Unicode's NFKC and case
  folding, so `ſ` is `s`); a file is inside another file's path; there are more than 20,000
  entries; the tar isn't the subset above.
- **Sizes:** the source archive is over 10 MB, a target's over 200 MB (more by request, per
  package), or one unpacks to more than 8 times its limit; docs JSON is over 25 MB.
- **The SHA-256 or the size** isn't the one the publish request names.
- **project.toml:** it isn't at the root, is over 1 MB, or doesn't parse as TOML; `[project] name` or `version`
  isn't the upload's; `type` is `"executable"` (or it has `main` or a root `main.fb` and no
  `type`; `type = "library"` with a `main.fb` is a library), since only libraries are dependencies; `license` isn't a valid SPDX expression of
  known licenses (`LicenseRef-...` is allowed); there's no `description`, or it's empty; `fb`
  isn't a valid range; a dependency's name isn't a valid package name, it is the package itself,
  its range isn't valid, or it has `path`, `git`, or `registry`.
- **Dependencies:** a dependency isn't a package on the registry (by its exact name), or no version
  of it that's published and not yanked meets its range, so the version could never be added.
- **The license and readme:** there is no license file at the root (`LICENSE`, `LICENSE.md`,
  `LICENSE.txt`, `COPYING`, or what `[project] license_file` names) or no readme (`README.md`,
  `README`, `README.txt`, or what `[project] readme` names); the readme is over 1 MB, isn't
  UTF-8, or doesn't render.
- **Prebuilt:** the source archive has a `lib/<target>/` or `native/<target>/` path; it has no
  `.fb` files and there are no target archives; a source package's target archive has `lib/`; a
  target archive has a path outside `lib/<target>/` and `native/<target>/`, or is empty; a
  prebuilt package's target archive lacks either `debug` or `release`'s `<name>.fblib` and
  `<name>.fbi`; a `.fblib`'s first line, `// fblib <name> <version> <hash>`, and its `.fbi`'s,
  `// fbi <name> <version> <hash>`, don't name this package, this version, and one hash.
- **Docs:** the docs JSON doesn't parse, or isn't fb doc's format 1 for this package and version
  (`format`, `package`, `version`, and `modules`).

project.toml's `[project]` keys the registry reads: `name`, `version`, `type`, `license`,
`license_file`, `readme`, `description` (needed, at most 300 characters), `keywords` (at most 5,
each up to 32 lowercase letters, digits, and `-`), `repository`, `homepage`, `documentation`
(https URLs), and `fb`; and `[dependencies]`, each a range or `{ version = "…" }`, whose `link`
the registry ignores (the program decides it).

## The API

`https://pkg.futureblitz.dev/api/v1`, JSON in and out (`Content-Type: application/json`) but for
uploads, readmes, and redirects.

### Authentication

- **A token**: `Authorization: Bearer fbr_<43 characters>`. fb's commands use this.
- **A session**: the `__Host-fb_session` cookie the sign-in endpoints set (Secure, HttpOnly,
  SameSite=Lax, 30 days), for the package site. A request with a session that changes something
  needs an `Origin` header that's the package site's, and any change with another `Origin` is
  refused; fb sends none.
- **Re-auth:** a session's sensitive changes (tokens, owners, trusted publishers, passkeys,
  recovery codes, deleting) need a passkey or GitHub sign-in in the last 10 minutes, or they're a
  `403` with `reauth_required`. A token's scopes stand in for it.

### Errors

Any `4xx` or `5xx` has

```json
{ "errors": [{ "code": "version_exists", "detail": "physics 1.2.0 is already published" }] }
```

`detail` is a sentence for a person, which fb prints; `code` is for programs. The codes:
`bad_request`, `invalid_name`, `invalid_version`, `invalid_range`, `unauthorized`, `forbidden`,
`reauth_required`, `scope`, `not_found`, `version_exists`, `name_taken`, `name_reserved`,
`too_large`, `checksum`, `archive`, `manifest`, `license`, `readme`, `prebuilt`, `docs`,
`email_unverified`, `rate_limited` (with `Retry-After`), `has_dependents`, `too_late`,
`last_owner`, `conflict`, `expired`, `unavailable` (a `503`, with `Retry-After`, when a service the
registry asks, such as GitHub, couldn't be, or while the registry is restored, when it takes no
change but signing in and an admin's), and `internal`.

### Tokens and scopes

A token has scopes and, optionally, the packages it's for:

| Scope | Allows |
|---|---|
| `publish-new` | publishing a package that doesn't exist yet |
| `publish-update` | publishing a new version of a package the account can publish |
| `yank` | yanking, unyanking, and deleting a version within 72 hours |
| `owners` | adding and removing owners and trusted publishers |

`packages` is a list of up to 50 names, each of which may end with `*` (`engine_*`), or `null`
for every package the account can act on; a token with a list can't touch others, new ones
included. A
token expires (90 days by default, at most 365); it's shown once, and stored as its SHA-256.

### Sign-in for fb (`fb login`)

1. `POST /cli-logins` `{ "name": "fb on hunter-pc" }` →
   `201 { "id", "secret", "code": "WDJB-MJHT", "url": "https://pkg.futureblitz.dev/cli-login/<id>", "expires_at", "interval": 3 }`.
   fb opens `url` and prints `code`.
2. The package site's page shows the code and, with a session, `GET /cli-logins/<id>` →
   `{ "id", "name", "code", "created_at", "expires_at", "status" }`, where `status` is
   `pending`, `approved`, `denied`, or `taken` (fb has its token), or a `404` once it has expired
   or if it never was; the person approves with
   `POST /cli-logins/<id>/approve` `{ "scopes", "packages", "expires_in_days" }` (re-auth), or
   denies with `POST /cli-logins/<id>/deny`.
3. fb polls `POST /cli-logins/<id>/poll` `{ "secret" }` every `interval` seconds:
   `202 { "status": "pending" }`; then once `200 { "status": "approved", "token", "expires_at", "username" }`;
   or `410` `expired` when denied, expired (10 minutes), already taken, or its account was
   deleted. Approving or denying one already answered, before or after fb polls, is a `409`
   `conflict`, until it expires.

In CI, fb reads `FB_REGISTRY_TOKEN` instead.

### Uploads

A publish names archives that were uploaded first, each by its SHA-256, so a retry re-sends only
what's missing. Uploads need a token with a publish scope, and an account whose email is
verified. Each account's uploads are its own.

- `GET /uploads/<sha256>` (or `HEAD`): `200 { "sha256", "size" }` when this account has it
  staged, else `404`.
- `PUT /uploads/<sha256>` with `Content-Length` and the bytes, up to 64 MB:
  `201 { "sha256", "size" }`; bytes that aren't the SHA-256 are a `422` `checksum`.
- Bigger, in parts: `POST /uploads/<sha256>/parts` `{ "size" }` (up to 2000 MB) →
  `201 { "upload_id", "part_size" }`; then `PUT /uploads/<sha256>/parts/<upload_id>/<n>`, for n
  from 1 to the size over `part_size`, rounded up, each exactly `part_size` bytes but the last,
  which is the rest (another length or number is a `400`) → `200 { "part": n, "etag" }`; then
  `POST /uploads/<sha256>/parts/<upload_id>/complete` `{ "parts": [{ "part": 1, "etag": "…" }, …] }`
  → `201 { "sha256", "size" }`. Put together, it has to be the size it said and the SHA-256 it's
  named by, or it's deleted and the answer is a `422` `checksum`.
- An account has at most 4000 MB staged, in uploads and uploads in parts begun (a `413`
  `too_large` past it).

The SHA-256 is checked again at publish. Staged uploads, and uploads in parts never put
together, are deleted a day after they're made. What a publish checked is what's stored: its
archives are copied with their SHA-256s, and its docs from the bytes it checked.

### Publishing

`POST /packages/<name>/versions`:

```json
{
  "version": "1.2.0",
  "archives": {
    "source": { "sha256": "…", "size": 48211 },
    "linux-x64": { "sha256": "…", "size": 5120334 }
  },
  "docs": { "sha256": "…", "size": 90122 }
}
```

`docs` is the upload of `fb doc`'s JSON, or `null`. It needs a token with `publish-new` (a new
package) or `publish-update`, an account whose email is verified, and, for an existing package,
an owner's rights; 30 publish attempts an hour per account. On success,
`201 { "name", "version", "warnings": [] }`: the version is in the index at once (the edge
elsewhere within seconds, by the purge), and its readme, docs, and search follow within a minute;
the other owners get a mail. Two publishes of one package are serialized; of
one version, the second is a `409` `version_exists`, and so is a deleted version's number, whose
`detail` says it was deleted.

A trusted publishing token publishes only new versions of packages that trust its workflow; a
package not on the registry yet is a `403` `scope` saying its first version comes from
`fb login` or `FB_REGISTRY_TOKEN`.

### Packages and versions

- `GET /packages/<name>` → `{ "name", "description", "keywords", "repository", "homepage", "documentation", "license", "max_version", "targets", "downloads", "created_at", "updated_at", "owners": [<owner>], "versions": [<version>] }`, versions newest first.
- `GET /packages/<name>/versions/<version>` → `<version>`:
  `{ "version", "yanked", "license", "fb", "deps", "targets", "archives", "published_at", "published_by": "<username> or null", "trusted_publisher": "<owner/repo> or null", "downloads", "readme": bool, "docs": bool }`.
- `GET /packages/<name>/versions/<version>/readme` → the rendered readme, `text/html`; raw HTML in
  the Markdown is escaped, and links are only http, https, mailto, and relative ones (not
  `//host`); a `404`
  until it's rendered.
- `GET /packages/<name>/versions/<version>/docs` → the docs JSON, with `ETag: "<sha256>"` of it;
  `HEAD` gives the ETag and `Content-Length` alone.
- `GET /packages/<name>/dependents?page=` → `{ "dependents": [{ "name", "version", "req" }], "total" }`:
  the newest version of each package that depends on it.
- `GET /packages/<name>/downloads` → `{ "total", "days": [{ "day": "2026-10-29", "version", "count" }] }`, the last 90 days.
- `POST /packages/<name>/versions/<version>/yank` and `/unyank` → `200 <version>`; needs `yank`.
- `DELETE /packages/<name>/versions/<version>` → `204`; needs `yank`, within 72 hours of
  publishing (`too_late` after), and no other package's version whose range takes it
  (`has_dependents`). An admin can delete any package's version at any time, from a session
  with re-auth, with `?reason=` (up to 500 characters), which the audit log keeps.

### Owners

An owner is `{ "kind": "user", "name": "alice" }` or `{ "kind": "team", "name": "acme" }`.

- `GET /packages/<name>/owners` → `{ "owners": [<owner>], "invites": [<invite>] }`, the pending
  invites to own it; anyone can ask, without a token, as `fb owner <name>` does.
- `POST /packages/<name>/owners` `{ "user": "alice" }` → `202 <invite>`, mailed to alice, who
  accepts it; or
  `{ "team": "acme" }` → `201 <owner>`, added at once if the caller is one of acme's owners.
- `DELETE /packages/<name>/owners` `{ "user": "alice" }` or `{ "team": "acme" }` → `204`; the last
  owner is a `409` `last_owner`, even when two removals race. A removed user's invites for the
  package go with them: those they sent, and those sent to them.
- `DELETE /packages/<name>/owners/invites/<id>` → `204` revokes a pending invite.
- The changes need the `owners` scope, or a session with re-auth, and an owner's rights: a user
  owner, or an owner of an owning team. A team's publishers can publish and yank its packages, but
  not change their owners.

An invite is `{ "id", "kind": "owner" | "team", "package" or "team", "role", "from", "invitee", "created_at", "expires_at" }`;
invites expire after 14 days. One person has one pending invite to a package or a team at a
time (another is a `409` `conflict`), and an account sends at most 20 invites an hour (`429`).

- `GET /me/invites` → `{ "invites": [<invite>] }`, the newest 100; with any token but a trusted
  publishing token.
- `POST /me/invites/<id>/accept` and `/decline` → `204`. Accepting checks that the inviter can
  still give it (a `410` `expired` if not), and a team's invite to someone already in it is a
  `409` `conflict`: only the team's owners change a member's role.

### Teams

- `POST /teams` `{ "name": "acme" }` → `201 <team>`; the caller is its owner.
- `GET /teams/<name>` → `{ "name", "created_at", "members": [{ "username", "role" }], "packages": ["…"] }`.
- `DELETE /teams/<name>` → `204`: its packages keep their other owners, or have none.
- `POST /teams/<name>/members` `{ "user": "bob", "role": "owner" | "publisher" }` → `202 <invite>`.
- `GET /teams/<name>/invites` (its owners, signed in) → `{ "invites": [<invite>] }`;
  `DELETE /teams/<name>/invites/<id>` → `204` revokes one.
- `PATCH /teams/<name>/members/<username>` `{ "role" }` → `200`.
- `DELETE /teams/<name>/members/<username>` → `204`; the last owner is a `409` `last_owner`.
  Demoting or removing a member drops the team's invites they sent, and those sent to them.
- `GET /teams/<name>/audit?before=` (its owners, signed in) → `{ "events": [<event>] }`.

Changing a team needs a session with re-auth, or a token with `owners` and no `packages` list
(a token for some packages can't touch teams, a `403` `scope`), and the team's owner role.

### Trusted publishing

A package's owner names a GitHub repository and workflow:

- `GET /packages/<name>/trusted-publishers` (signed in) → `{ "trusted_publishers": [{ "id", "repository": "owner/repo", "workflow": "release.yml", "environment": "release" or null, "pending": bool, "created_at" }] }`.
- `POST /packages/<name>/trusted-publishers` `{ "repository", "workflow", "environment" }` →
  `201` that trusted publisher. The registry looks the repository up on GitHub and keeps its and
  its owner's GitHub ids, so a repository deleted and made again by someone else doesn't match.
  A repository GitHub doesn't show the registry, as a private one, is kept by its name, with
  `"pending": true`, and the first OIDC token from that repository's workflow (in its
  environment, if one is named) gives it the ids, which every later token has to match. If GitHub
  can't be asked (it refuses, limits, or fails), it's a `503` `unavailable`. The same repository
  (letter case aside), workflow, and environment twice is a `409` `conflict`.
- `DELETE /packages/<name>/trusted-publishers/<id>` → `204`.

In the workflow (`permissions: id-token: write`), fb asks GitHub for an OIDC token with
`audience` config.json's `oidc_audience`, and exchanges it:

- `POST /trusted-publishing/token` `{ "token": "<the JWT>" }` →
  `201 { "token": "fbr_…", "expires_at", "packages": ["physics"] }`: a token for 30 minutes,
  with `publish-update` and `yank` scopes for every package whose trusted publishers match the
  JWT's `repository_id`, `repository_owner_id`, workflow file (from `workflow_ref`, which has to
  be in the JWT's own `repository`), and, if one is named, `environment`. The JWT is RS256,
  from `https://token.actions.githubusercontent.com`, for `oidc_audience`, unexpired, and used
  once. Each publish with it acts with its package's rights: the trusted publisher's maker's
  while they can publish the package, else its longest-standing user owner's. Deleting any of
  the token's trusted publishers revokes it. A JWT no package trusts is a `403` `forbidden`
  whose `detail` names the repository and workflow, or, when packages trust the workflow only in
  another environment, those packages and environments and the one the job ran in (or none).
- fb then publishes with it as with any token, and `DELETE /tokens/current` revokes it at the end.
  It can't read the account it acts for: `GET /me`, `/me/teams`, and `/me/invites` are a `403`
  `forbidden` to it.

### The account

- `GET /me` → `{ "username", "email", "email_verified", "github_login", "passkeys", "has_recovery_codes", "created_at" }`; with any token but a trusted publishing token,
  which `fb login --token` asks before it keeps one, to refuse a token the registry doesn't take
  and to say whose it is.
- `DELETE /me` (re-auth) → `204`: sessions, tokens, passkeys, `fb login`s, invites, and its
  email and GitHub login and id go, and its packages stay, owned by their other owners or by no one. The only owner of a team is
  refused, a `409` `last_owner` naming the teams, until they name another owner or delete it.
- `GET /me/teams` → `{ "teams": [{ "name", "role", "members", "packages": ["…"] }] }`: the teams
  the account is in, by name, with its role in each, how many members each has, and the packages
  each owns; with any token but a trusted publishing token.
- `GET /me/tokens` → `{ "tokens": [{ "id", "name", "prefix": "fbr_ab12", "scopes", "packages", "created_at", "expires_at", "last_used_at" }] }`.
- `POST /me/tokens` `{ "name", "scopes", "packages", "expires_in_days" }` (re-auth) → `201` that and `"token"`, shown once.
- `DELETE /me/tokens/<id>` → `204`; `DELETE /tokens/current` revokes the token sending it.
- `GET /me/audit?before=` → `{ "events": [<event>] }`, the account's own, 100 at a time;
  `GET /packages/<name>/audit` the same for its owners (not a team's publishers). `before` is an
  event's id, a positive whole number (else a `400`). An event is
  `{ "id", "at", "action", "actor", "package", "version", "detail" }`.
- `GET /users/<username>` → `{ "username", "github_login", "created_at", "packages": ["…"] }`.

### Signing in (the package site)

All of these set the session cookie when they sign someone in. Only the GitHub flow takes
`next`, a path on the package site (printable ASCII, one `/` first, no `\`; anything else is
`/`), and redirects there; the others answer JSON and leave where to go to the page. A passkey
has to check a PIN or a biometric (WebAuthn's user verification is required). An address is
held only once it's verified: an unverified claim doesn't stop another sign-up, GitHub, or a
change taking it, and verifying it, by link or by GitHub, takes it from accounts that only
claimed it.

- **GitHub:** `GET /auth/github?next=/dashboard&purpose=signin|reauth|link` → `302` to GitHub;
  GitHub comes back to `GET /auth/github/callback`, which signs in (making the account the first
  time, with the GitHub login as its username, or `<login>-<4 digits>` if that's taken) and
  `302`s to `next`. With `purpose=reauth` the signed-in account's GitHub has to match;
  `purpose=link` (re-auth) adds GitHub to a signed-in email account. A failure `302`s to
  `/signin?error=<state|expired|denied|github|session>` (`denied` when it was cancelled on
  GitHub), or to `next` with `error=github_mismatch` or `github_taken`. A deleted account's
  GitHub identity can sign up again, as a new account.
- **Email and a passkey, signing up:** `POST /auth/register/options`
  `{ "email", "username", "turnstile" }` → `{ "challenge_id", "options" }` (WebAuthn creation
  options); `POST /auth/register/verify` `{ "challenge_id", "credential" }` → `201 <me>`. The
  email gets a verification link to `https://pkg.futureblitz.dev/verify-email?token=…`, whose
  page calls `POST /auth/email/verify` `{ "token" }` → `200 <me>` (a `409` `conflict` if another
  account verified it first). Links last 24 hours; `POST /me/email/resend` sends another, and an
  account has at most 5 live verification links (another is a `429`).
- **Changing the address:** `POST /me/email` `{ "email" }` (re-auth) → `200 <me>`, unverified
  until its link is followed, and the account's other sessions are signed out; the same address
  again changes nothing. It counts against the sign-in limit.
- **A passkey, signing in:** `POST /auth/passkey/options` `{}` → `{ "challenge_id", "options" }`;
  `POST /auth/passkey/verify` `{ "challenge_id", "credential" }` → `200 <me>`.
- **Re-auth by passkey:** `POST /auth/reauth/options` and `/auth/reauth/verify`, the same shapes.
- **Passkeys:** `GET /me/passkeys` → `{ "passkeys": [{ "id", "name", "created_at", "last_used_at" }] }`;
  `POST /me/passkeys/options` → `{ "challenge_id", "options" }` and `POST /me/passkeys`
  `{ "challenge_id", "credential", "name" }` → `201` (re-auth); `DELETE /me/passkeys/<id>` →
  `204` (re-auth), but not an account's last way to sign in (`conflict`), and it signs the
  account's other sessions out. A passkey already registered is a `409` `conflict`.
- **Sessions:** `POST /me/sessions/revoke` (re-auth) → `204` signs out every other session.
- **Recovery:** `POST /auth/email/link` `{ "email" }` → `202` always, and a verified email gets a
  sign-in link (at most 3 live at once) to `/signin-link?token=…`, whose page calls `POST /auth/email/link/verify`
  `{ "token" }` → `200 <me>`; `POST /me/recovery-codes` (re-auth) → `201 { "codes": [10 codes] }`,
  replacing the old ones; `POST /auth/recovery` `{ "username", "code" }` → `200 <me>`, using the
  code up. A recovery sign-in counts as re-auth, so a new passkey can be added, and it signs the
  account's other sessions out.
- `POST /auth/signout` → `204`.

### Search

`GET /search?q=physics&keyword=&target=&user=&team=&sort=relevance|downloads|recent&page=1&per_page=20` →
`{ "packages": [{ "name", "max_version", "description", "keywords", "targets", "downloads", "updated_at" }], "total" }`.
`q` searches names, descriptions, and keywords, by their words in any script, accents aside. A
package is found only if it matches every word of `q` (up to its first 10), each as the start of
one of its words, so `color palette` finds a package with both and not one with only one; a `q`
with no letters or digits finds nothing. fb's search of a registry on disk, which has only names,
needs every word in a name. An empty `keyword`, `target`, or `sort` is none, as
a form sends it. `user` and `team` keep the packages that person or team owns, as their pages
list them (a name that isn't a username or a team's is a `400`). `page` is from 1 to 1000 and `per_page` from 1 to 100 (20 by default, and a
larger or smaller one is taken as the nearest); a page or per_page that isn't a whole number, or
a page out of range, is a `400`.

### Health

`GET /health` → `200 { "status": "ok", "checks": { "d1": "ok", "r2": "ok", "queue": "ok" }, "backup": { "last", "fresh" } }`,
or a `503` with `"status": "down"` and each check that didn't answer in 5 seconds `"down"`: D1,
both R2 buckets, and the queue, which takes a message the jobs drop. `backup` is when the last
nightly backup of D1 was made (RFC 3339, or `null`), and whether that was in the last 26 hours;
it doesn't make the registry down. It needs no sign-in, isn't cached, and is what the package
site's status page and the health checks read.

### Admin

The registry's admins' own endpoints, from an admin's session, never a token: with no sign-in
they're a `401` `unauthorized`, and from a token or anyone else's session a `403` `forbidden`.
The `POST`s need re-auth, or they're a `403` `reauth_required`.

- `POST /admin/reindex` → `202 { "run", "report": "/api/v1/admin/reindex/<run>" }`: rebuilds the
  index from D1, after a restore, which the audit log keeps as `registry.reindexed`. It goes on
  in the background: the name of a package D1 hasn't whose archives are on dl is reserved, each
  package's index file is made what D1 says, an index file of a package D1 hasn't is emptied, and
  archives are compared with versions', none deleted. Running it again changes nothing. A first
  publish of a name whose folder on dl has archives, from a package D1 lost, is a `403`
  `name_reserved`, and reserves the name; a version whose archive is on dl though D1 has no row
  of it can't be published again, a `409` `version_exists`.
- `GET /admin/reindex/<run>` → `{ "run", "started", "started_by", "finished", "phase", "steps", "packages", "rewritten", "unknown_archives", "missing_archives", "reserved", "undone_yanks", "next", "sent" }`:
  the rebuild's report so far; `finished` is `null` until it's done. `rewritten` is the packages
  whose index files changed, `unknown_archives` the archives' paths no version names,
  `missing_archives` those a listed version names that aren't there, `reserved` the names
  reserved, and `undone_yanks` the yanks and unyanks the index files had and D1 hasn't, as
  `"<name> <version> was yanked"`; `next` and `sent` are its next step's. A run there isn't is a
  `404`.
- `POST /admin/reserved/<name>/owner` `{ "user": "<username>" }` → `204`: gives a name the
  rebuild reserved back to the account that owned its package, which is made, owned by them, as
  the reservation is dropped. A name not reserved for a lost package, or an account there isn't,
  is a `404`; a package of that name already, a `409` `name_taken`.

### Rate limits

An account tries at most 30 publishes an hour, counted as each starts, failed ones too, so
publishes sent at once can't pass the limit. Sign-in and sign-up endpoints take 10 requests a
minute from an address, and the rest of the API 300 a minute; an IPv6 address counts by its
/64. Over a limit is a `429` `rate_limited` with `Retry-After`.
