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

`/fb/` on dl is never a package's, since `fb` is a reserved name, so fb's own releases can live
there.

## Names and versions

- **A package's name** is lowercase ASCII letters, digits, and `_`, starting with a letter, at most
  64: `^[a-z][a-z0-9_]{0,63}$`. Two names that are the same without their `_`s are one name to the
  registry, so `foo_bar` can't be published beside `foobar`; `std`, `fb`, and `futureblitz`, and
  any name that's one of them without its `_`s, are reserved.
- **A username** is lowercase letters, digits, and `-`, 1 to 39 long, not starting or ending with
  `-`. **A team's name** is the same.
- **A version** is semver 2.0: `MAJOR.MINOR.PATCH`, with an optional `-prerelease`, no leading
  zeros. Build metadata (`+...`) is refused at publish, so two versions never differ by it alone.
  Versions order as semver says.
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
  fb sends `If-None-Match` and gets a `304` when nothing changed. A name with no package is a
  `404`. The edge caches each file; a publish, yank, or delete purges it.

## Downloads

`GET https://dl.futureblitz.dev/<name>/<version>/<file>` is the archive, `application/gzip`,
`Cache-Control: public, max-age=31536000, immutable`, with `ETag: "<sha256>"`. fb checks the
SHA-256 against the index line or fb.lock. A yanked version's archives still download; a deleted
one's are a `404`. Each `GET` counts a download.

## Archives

A `.fbpkg` is a gzip-compressed tar (POSIX ustar, or pax for long paths), with its files at its
root, not under a folder.

- **The source archive**, `<name>-<version>.fbpkg`: `project.toml`, the license, the readme, and
  either the package's `.fb` source or, for a prebuilt package, no `.fb` files at all.
- **A target's archive**, `<name>-<version>-<target>.fbpkg`: only `lib/<target>/` and
  `native/<target>/`. A prebuilt package's has `lib/<target>/debug/<name>.fblib` and `.fbi` and
  the same in `release/`; a package with natives has its `native/<target>/` files.
- A target is `windows-x64`, `linux-x64`, `linux-arm64`, `macos-arm64`, or `macos-x64`.

### Checks at publish

The API refuses an archive, with an error naming the file and why, when:

- **Paths:** an entry is anything but a file or a folder (a link, a device, a FIFO); a path is
  absolute, has a drive letter, `\`, a NUL, a control character, `<>:"|?*`, an empty part, or a
  `.` or `..` part, or a part that ends in `.` or a space, or is a name Windows reserves (`CON`,
  `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, with any extension); a path is longer than
  255 bytes, or isn't UTF-8; two paths are the same ignoring letter case; there are more than
  20,000 entries.
- **Sizes:** the source archive is over 10 MB, a target's over 200 MB (more by request, per
  package), or one unpacks to more than 8 times its limit; docs JSON is over 25 MB.
- **The SHA-256 or the size** isn't the one the publish request names.
- **project.toml:** it isn't at the root or doesn't parse as TOML; `[project] name` or `version`
  isn't the upload's; `type` is `"executable"` (or it has `main` or a root `main.fb` and no
  `type`), since only libraries are dependencies; `license` isn't a valid SPDX expression of
  known licenses (`LicenseRef-...` is allowed); `fb` isn't a valid range; a dependency's name
  isn't a valid package name, it is the package itself, its range isn't valid, or it has `path`,
  `git`, or `registry`.
- **The license and readme:** there is no license file at the root (`LICENSE`, `LICENSE.md`,
  `LICENSE.txt`, `COPYING`, or what `[project] license_file` names) or no readme (`README.md`,
  `README`, `README.txt`, or what `[project] readme` names); the readme is over 1 MB, isn't
  UTF-8, or doesn't render.
- **Prebuilt:** a source archive with `.fb` files has a `lib/` folder, or one without them has no
  target archives; a target archive has a path outside `lib/<target>/` and `native/<target>/`,
  or is empty; a target with `lib/` lacks either `debug` or `release`'s `<name>.fblib` and
  `<name>.fbi`; a `.fblib`'s first line, `// fblib <name> <version> <hash>`, and its `.fbi`'s,
  `// fbi <name> <version> <hash>`, don't name this package, this version, and one hash.
- **Docs:** the docs JSON doesn't parse, or isn't fb doc's format 1 for this package and version
  (`format`, `package`, `version`, and `modules`).

project.toml's `[project]` keys the registry reads: `name`, `version`, `type`, `license`,
`license_file`, `readme`, `description` (at most 300 characters), `keywords` (at most 5, each a
name), `repository`, `homepage`, `documentation` (https URLs), and `fb`.

## The API

`https://pkg.futureblitz.dev/api/v1`, JSON in and out (`Content-Type: application/json`) but for
uploads, readmes, and redirects.

### Authentication

- **A token**: `Authorization: Bearer fbr_<43 characters>`. fb's commands use this.
- **A session**: the `__Host-fb_session` cookie the sign-in endpoints set (Secure, HttpOnly,
  SameSite=Lax, 30 days), for the package site. A request with a session that changes something
  needs an `Origin` header that's the package site's.
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
`last_owner`, `conflict`, `expired`, and `internal`.

### Tokens and scopes

A token has scopes and, optionally, the packages it's for:

| Scope | Allows |
|---|---|
| `publish-new` | publishing a package that doesn't exist yet |
| `publish-update` | publishing a new version of a package the account can publish |
| `yank` | yanking, unyanking, and deleting a version within 72 hours |
| `owners` | adding and removing owners and trusted publishers |

`packages` is a list of names, each of which may end with `*` (`engine_*`), or `null` for every
package the account can act on; a token with a list can't touch others, new ones included. A
token expires (90 days by default, at most 365); it's shown once, and stored as its SHA-256.

### Sign-in for fb (`fb login`)

1. `POST /cli-logins` `{ "name": "fb on hunter-pc" }` →
   `201 { "id", "secret", "code": "WDJB-MJHT", "url": "https://pkg.futureblitz.dev/cli-login/<id>", "expires_at", "interval": 3 }`.
   fb opens `url` and prints `code`.
2. The package site's page shows the code and, with a session, `GET /cli-logins/<id>` →
   `{ "id", "name", "code", "created_at", "expires_at", "status" }`; the person approves with
   `POST /cli-logins/<id>/approve` `{ "scopes", "packages", "expires_in_days" }` (re-auth), or
   denies with `POST /cli-logins/<id>/deny`.
3. fb polls `POST /cli-logins/<id>/poll` `{ "secret" }` every `interval` seconds:
   `202 { "status": "pending" }`; then once `200 { "status": "approved", "token", "expires_at", "username" }`;
   or `410` `expired` when denied, expired (10 minutes), or already taken.

In CI, fb reads `FB_REGISTRY_TOKEN` instead.

### Uploads

A publish names archives that were uploaded first, each by its SHA-256, so a retry re-sends only
what's missing. Uploads need a token with a publish scope.

- `HEAD /uploads/<sha256>`: `200` with `Content-Length` when this account has it staged, else
  `404`.
- `PUT /uploads/<sha256>` with `Content-Length` and the bytes, up to 64 MB: `201 { "sha256", "size" }`.
- Bigger, in parts: `POST /uploads/<sha256>/parts` `{ "size" }` → `201 { "upload_id", "part_size" }`;
  then `PUT /uploads/<sha256>/parts/<upload_id>/<n>`, for n from 1, each `part_size` bytes but
  the last → `200 { "part": n, "etag" }`; then `POST /uploads/<sha256>/parts/<upload_id>/complete`
  `{ "parts": [{ "part": 1, "etag": "…" }, …] }` → `201 { "sha256", "size" }`.

The SHA-256 is checked at publish. Staged uploads are deleted a day after they're made.

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
an owner's rights; 30 publishes an hour per account. On success,
`201 { "name", "version", "warnings": [] }`: the version is in the index at once, and its
readme, docs, and search follow within a minute. Two publishes of one package are serialized; of
one version, the second is a `409` `version_exists`.

### Packages and versions

- `GET /packages/<name>` → `{ "name", "description", "keywords", "repository", "homepage", "documentation", "license", "max_version", "downloads", "created_at", "updated_at", "owners": [<owner>], "versions": [<version>] }`, versions newest first.
- `GET /packages/<name>/versions/<version>` → `<version>`:
  `{ "version", "yanked", "license", "fb", "deps", "targets", "archives", "published_at", "published_by": "<username> or null", "trusted_publisher": "<owner/repo> or null", "downloads", "readme": bool, "docs": bool }`.
- `GET /packages/<name>/versions/<version>/readme` → the rendered readme, `text/html`; raw HTML in
  the Markdown is escaped, and links are only http, https, and mailto.
- `GET /packages/<name>/versions/<version>/docs` → the docs JSON.
- `GET /packages/<name>/dependents?page=` → `{ "dependents": [{ "name", "version", "req" }], "total" }`:
  the newest version of each package that depends on it.
- `GET /packages/<name>/downloads` → `{ "total", "days": [{ "day": "2026-10-29", "version", "count" }] }`, the last 90 days.
- `POST /packages/<name>/versions/<version>/yank` and `/unyank` → `200 <version>`; needs `yank`.
- `DELETE /packages/<name>/versions/<version>` → `204`; needs `yank`, within 72 hours of
  publishing (`too_late` after), and no other package's version whose range takes it
  (`has_dependents`); an admin can at any time.

### Owners

An owner is `{ "kind": "user", "name": "alice" }` or `{ "kind": "team", "name": "acme" }`.

- `GET /packages/<name>/owners` → `{ "owners": [<owner>], "invites": [<invite>] }`.
- `POST /packages/<name>/owners` `{ "user": "alice" }` → `202 <invite>`, which alice accepts; or
  `{ "team": "acme" }` → `201 <owner>`, added at once if the caller is one of acme's owners.
- `DELETE /packages/<name>/owners` `{ "user": "alice" }` or `{ "team": "acme" }` → `204`; the last
  owner is a `409` `last_owner`.
- They need the `owners` scope, or a session with re-auth, and an owner's rights: a user owner,
  or an owner of an owning team. A team's publishers can publish and yank its packages, but not
  change their owners.

An invite is `{ "id", "kind": "owner" | "team", "package" or "team", "role", "from", "created_at", "expires_at" }`;
invites expire after 14 days.

- `GET /me/invites` → `{ "invites": [<invite>] }`.
- `POST /me/invites/<id>/accept` and `/decline` → `204`.

### Teams

- `POST /teams` `{ "name": "acme" }` → `201 <team>`; the caller is its owner.
- `GET /teams/<name>` → `{ "name", "created_at", "members": [{ "username", "role" }], "packages": ["…"] }`.
- `POST /teams/<name>/members` `{ "user": "bob", "role": "owner" | "publisher" }` → `202 <invite>`.
- `PATCH /teams/<name>/members/<username>` `{ "role" }` → `200`.
- `DELETE /teams/<name>/members/<username>` → `204`; the last owner is a `409` `last_owner`.

Changing a team needs a session with re-auth, or a token with `owners`, and the team's owner role.

### Trusted publishing

A package's owner names a GitHub repository and workflow:

- `GET /packages/<name>/trusted-publishers` → `{ "trusted_publishers": [{ "id", "repository": "owner/repo", "workflow": "release.yml", "environment": "release" or null, "created_at" }] }`.
- `POST /packages/<name>/trusted-publishers` `{ "repository", "workflow", "environment" }` → `201`;
  the registry looks the repository up and keeps its and its owner's GitHub ids, so a repository
  deleted and made again by someone else doesn't match.
- `DELETE /packages/<name>/trusted-publishers/<id>` → `204`.

In the workflow (`permissions: id-token: write`), fb asks GitHub for an OIDC token with
`audience` config.json's `oidc_audience`, and exchanges it:

- `POST /trusted-publishing/token` `{ "token": "<the JWT>" }` →
  `201 { "token": "fbr_…", "expires_at", "packages": ["physics"] }`: a token for 30 minutes,
  with `publish-update` and `yank` scopes for the packages whose trusted publishers match the
  JWT's `repository_id`, `repository_owner_id`, workflow (from `job_workflow_ref`), and, if one is
  named, `environment`. A JWT is used once.
- fb then publishes with it as with any token, and `DELETE /tokens/current` revokes it at the end.

### The account

- `GET /me` → `{ "username", "email", "email_verified", "github_login", "passkeys", "has_recovery_codes", "created_at" }`; with a token too, for `fb whoami`.
- `DELETE /me` (re-auth) → `204`: sessions, tokens, and passkeys go, and its packages stay, owned
  by their other owners or by no one.
- `GET /me/tokens` → `{ "tokens": [{ "id", "name", "prefix": "fbr_ab12", "scopes", "packages", "created_at", "expires_at", "last_used_at" }] }`.
- `POST /me/tokens` `{ "name", "scopes", "packages", "expires_in_days" }` (re-auth) → `201` that and `"token"`, shown once.
- `DELETE /me/tokens/<id>` → `204`; `DELETE /tokens/current` revokes the token sending it.
- `GET /me/audit?before=` → `{ "events": [<event>] }`; `GET /packages/<name>/audit` the same for
  its owners. An event is `{ "id", "at", "action", "actor", "package", "version", "detail" }`.
- `GET /users/<username>` → `{ "username", "github_login", "created_at", "packages": ["…"] }`.

### Signing in (the package site)

All of these set the session cookie when they sign someone in, and take `"next"`, a path on the
package site, where the browser goes after.

- **GitHub:** `GET /auth/github?next=/dashboard&purpose=signin|reauth` → `302` to GitHub;
  GitHub comes back to `GET /auth/github/callback`, which signs in (making the account the first
  time, with the GitHub login as its username, or `<login>-<4 digits>` if that's taken) and
  `302`s to `next`. With `purpose=reauth` the signed-in account's GitHub has to match.
- **Email and a passkey, signing up:** `POST /auth/register/options`
  `{ "email", "username", "turnstile" }` → `{ "challenge_id", "options" }` (WebAuthn creation
  options); `POST /auth/register/verify` `{ "challenge_id", "credential" }` → `201 <me>`. The
  email gets a verification link to `https://pkg.futureblitz.dev/verify-email?token=…`, whose
  page calls `POST /auth/email/verify` `{ "token" }` → `200 <me>`. Links last 24 hours;
  `POST /me/email/resend` sends another.
- **A passkey, signing in:** `POST /auth/passkey/options` `{}` → `{ "challenge_id", "options" }`;
  `POST /auth/passkey/verify` `{ "challenge_id", "credential" }` → `200 <me>`.
- **Re-auth by passkey:** `POST /auth/reauth/options` and `/auth/reauth/verify`, the same shapes.
- **Passkeys:** `GET /me/passkeys` → `{ "passkeys": [{ "id", "name", "created_at", "last_used_at" }] }`;
  `POST /me/passkeys/options` → `{ "challenge_id", "options" }` and `POST /me/passkeys`
  `{ "challenge_id", "credential", "name" }` → `201` (re-auth); `DELETE /me/passkeys/<id>` →
  `204` (re-auth), but not an account's last way to sign in (`conflict`).
- **Recovery:** `POST /auth/email/link` `{ "email" }` → `202` always, and a verified email gets a
  sign-in link to `/signin-link?token=…`, whose page calls `POST /auth/email/link/verify`
  `{ "token" }` → `200 <me>`; `POST /me/recovery-codes` (re-auth) → `201 { "codes": [10 codes] }`,
  replacing the old ones; `POST /auth/recovery` `{ "username", "code" }` → `200 <me>`, using the
  code up. A recovery sign-in counts as re-auth, so a new passkey can be added.
- `POST /auth/signout` → `204`.

### Search

`GET /search?q=physics&keyword=&target=&sort=relevance|downloads|recent&page=1&per_page=20` →
`{ "packages": [{ "name", "max_version", "description", "keywords", "targets", "downloads", "updated_at" }], "total" }`.
`q` searches names, descriptions, and keywords; `per_page` is at most 100.

### Rate limits

Beyond 30 publishes an hour per account, sign-in and sign-up endpoints take 10 requests a minute
from an address, and the rest of the API 300 a minute; over a limit is a `429` `rate_limited`
with `Retry-After`.
