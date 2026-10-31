# History

What each milestone and step added, by name, and each review's fixes, as CLAUDE.md kept
them while each was built. Newest first.

## M11, the IDE

Step 0, measuring (2026-10-31): tests/lsp/bench_tests.cpp, fb-lsp's benchmark, hidden from ctest
(`fb_lsp_tests "[bench]"`), on a generated program of 300 modules in 10 layers and on std's own
folder, with the editor's own delays. It times a syntax error's and a type error's diagnostics
from the didChange to their publication, completion after `.` and on a word and signature help
after `(` from the didChange to the response, and hover, the quick fixes at an error,
references, and a rename with every check done, `FB_LSP_BENCH_SAMPLES` times each (10), and
writes lsp-bench.md and lsp-bench.json to `FB_LSP_BENCH_REPORT`'s folder: each metric's 50th and
95th percentiles against tooling.md's budgets, by nearest rank, and the edited file's own checks
from the workspace's history. A metric's `enforced` flag makes a miss fail it; hover's and the
quick fixes' are on, as they met their budgets. `client_t` has `published()`, which waits for the
diagnostics of the document's current version, from the messages after its last change, and a
`dependents_delay` of its own. ctest runs it once on a program of 2 layers of 2 modules. CI's LSP
benchmark job builds fb_lsp_tests in release on Linux, runs it, puts the report in the run's
summary, and keeps it as the lsp-bench artifact. tooling.md has where the time goes: each check
checks everything its file imports again, std's too, the 150 ms wait, and the workspace's work
around a check, which canonicalizes paths at 0.45 ms each on Windows. driver.cpp's two locals
that shadowed others in `build_program`, which -Wshadow reported, are `beside_object` and
`written`.

A string's escapes in their own color (2026-10-31, the user): fb-lsp's semantic tokens gave each
string literal one `string` token, which VS Code colors over the grammar's
`constant.character.escape`, so `\n` looked like the rest of the string. `string_pieces()` in
semantic.cpp gives a string's or a character's tokens only its text, without its escapes, as
`decode_escape()` reads them, `{{` and `}}`, or its interpolations' `{` and `}`, and a raw string
whole; a test checks each piece of a string with escapes and an interpolation, a raw string's,
and a character's. interface.cpp's interface text, which keeps a doc comment's line break,
adds one to the end with an If, not `+= ... ? 1 : 0`, which -Wsign-conversion reported.

Brackets in brackets break once (2026-10-31, the user): a line with no comma or operator to break
at broke after its outermost opening bracket, then the next line after its own, so
`JsonReader((Try client.Get("...")).Bytes)` came out a staircase with a `(` alone on a line.
`layout_from` tries a break after an opening bracket that all that follows fits after, on the
next line at the bracket's own indentation, before one it doesn't, the outermost first; std
formats as before. Tests check the user's line, the outermost of two that fit, and the innermost
when only it does.

## 1.1.0

(2026-10-31.) Dependencies with both a path and a version, `fbengine = { path = "../fbengine",
version = "0.1" }`, as Cargo has them: a build uses the folder, which has to be at a version the
range takes, and isn't locked, though its own registry dependencies are; a package from the
registry uses the range; and `fb publish` writes the archive's project.toml without the path
(`published_manifest()` takes out its key and value, in an inline table with its comma, or its
line, a table's or a dotted key's, and reads the result back as the same manifest), refusing a
dependency with only a path as before, with how to add a version. `dependency_t` has `range_span`
for where the range is written, apart from `path_span`, and `path_entry_span`. A path with a
version can name a registry, as a path alone can't.

A built-in `Object`, what every Class's object is: `builtin_interfaces_t::object`, an Interface
without members that sema declares with its built-ins and resolves by the name when the module
declares and imports none, which every Class implements (`implements()` and conformance_query_t)
and a Class type parameter converts to; lowering adds it to every Class's table but a closure's
or cell's, and codegen names its fb_interface_t `fb.(Object).interface`, apart from any
program's. `As?` and `As` convert to and from a type parameter that's a Class's, as `Is` did;
`Weak<Object>` and `[Weak x]` of one; and errors for listing it, extending it, a constraint of
it, making one, and showing one. Struct values aren't Objects, and an Interface's value becomes
one only with `As?`, since it can be an array's. Interface files write it as `Object`, and
fb-lsp completes it. tests/programs/objects.fb keeps any Class's object in a holder and gets it
back typed; panic_object_cast.fb panics on the wrong Class.

A package's tests see its Package members: the tests/ folder's package, `<name>.tests`, and the
doc examples', `<name>.doctests`, are its tests in the module table (`add_tests()` and
`sees_package()`), which sema's `sees()` and `can_use()` and fb-lsp's completion use, while
Private names stay their files' and Public what other packages see. The tested package's
tests/ folder calls a Package Function.

std.math's `Mat4.Perspective` and `Mat4.Orthographic` panic, with their values, for values
whose matrix isn't finite, which they gave as Inf before: a field of view, an aspect, or a
height so small that its reciprocal is Inf, a near and far whose `near * depth` is, and sides
whose sum is (fbengine's review found it). Their panics' messages say the matrix is finite;
five more panic_math_ programs.

std.io's `Vfs` agrees with itself where a later mount sits at or under a place an earlier mount
has a file, or a later mount has a folder there: its reads, Async too, throw IS_A_DIRECTORY, as
`KindOf` and `Entries` say it's a folder, where they read the earlier file before (`Holds()` and
`FolderError()`; fbengine's review found it). tests/programs/io_vfs_shadowed.fb.

## 1.0.1

(2026-10-30.) Linux's fb-bindgen read no header that includes `<stdint.h>`: release.yml gathered
lib/linux-<arch>/include/ from glibc-headers and kernel-headers, and AlmaLinux 8 has
gnu/stubs-64.h and gnu/lib-names-64.h (arm64's -lp64.h) in glibc-devel, which fbengine's CI found.
The runtime job gathers glibc-devel's headers too, installing the three packages where they
aren't, and fails if any of the container's gnu/ headers isn't among them; each target's test job
runs the installed release's fb-bindgen on tools/release/libc.h, a header of `<stdbool.h>`,
`<stddef.h>`, `<stdint.h>`, and `<string.h>`. Windows' MinGW-w64 and winpthreads headers, and
macOS's SDK, from `xcrun`, already had every header theirs include. A cli test whose name began
with `--`, which Catch2 took as an option, so ctest never ran it, is "a --registry folder ...".

## E4, the scene

Steps 1 to 12, with E5 step 1's CPU half (2026-10-31, the scene lane, landed at a74f435). Worlds
hold a tree of entities, found by name, shown and hidden, copied with their children and shared
geometry, and freed, each a game's object attached with `SetData`; transforms are local and world,
moved, turned, pointed, and converted between entities. Cameras are perspective (vertical field of
view) or orthographic, with viewports, render order, projecting points both ways, and a draw
projection of reversed depth in Vulkan's clip space. Lights, the ambient light, and fog are data.
Geometry is built in code with two UV sets, welded, merged, and flipped, with the primitive shapes;
materials are data with texture layers, blends, lightmaps, and order. Visibility gives in view and
line of sight; collisions sweep ellipsoids against meshes, boxes, spheres, ground planes, and
terrains, with stop, slide, and step-up, a max slope, and a skin a mover never gets 0.001 past over
200 long seeded walks, never frozen with a way out; picks are lines and spheres by pick mode, from a
World, an entity, or a camera. Sprites face the camera as billboards, fixed, or upright;
`UpdateWorld`, `CaptureWorld` and tweened matrices, and `FixedStep` give logic at a fixed rate drawn
between its steps; ground planes, mirrors, fading by distance, mesh intersection, and terrains of
heights made and changed in code. What each camera draws is extracted, culled, faded, tweened, and
sorted, so drawing never reads the live scene. Eight reviews' findings were all fixed with tests,
among them the leaving root of a sweep that started inside an edge or corner, edges skipped moving
away from a plane, a squeezed mover pushed through a wall, a tall mover launched over a ledge,
movers frozen at exactly their radius from an edge, picks through a mesh's seams, collisions
depending on a mesh's edit history, and the camera and sprites untweened in what cameras draw.

## E9, fbengine_audio

Steps 1 to 3, 5, and 6, and step 4 on positions (2026-10-31, the audio lane, landed at 5d7e461).
Step 1 added fbengine_audio, a package over its own DLL, `fbengine_audio_native`, with miniaudio
linked in: the mix and its devices (`Audio.Start` on DEFAULT, SILENT, or MANUAL, an output by name
and a period, `Audio.Outputs`, `Audio.Output`, `Audio.Lost`), STA COM on Windows, a clipped mix, and
an exit with the audio started. Step 2 added sounds and channels, each Channel with volume, pan
under a pan law, pitch, loops, positions, seeks, and fades. Step 3 added streams of WAV, FLAC, and
OGG. Step 4 added the listener and emitters, positional sound with Doppler worked out by the engine
and held with the pitch to 1/16 to 16, moved along with what they follow from one late Frame.OnFlip
hook (`Frame.OnFlip(hook, late)`, which runs after every other); its entity overloads wait for the
scene's entities. Step 5 added buses with low-passes and reverbs. Step 6 added stops and pauses that
fade out over 5 ms, and a lost output that falls back to the default at the next Flip, following the
default as it changes. A Channel's volume and pan are one pair of gains, published to the mixing
thread at once and ramped. Four reviews' findings were all fixed with tests, among them a heap
corruption, a leak, and an undefined shift in stb_vorbis, patched at configure time, decode buffers
sized by a file's claimed length, sample rates and Doppler past their limits, a pan heard half
applied, a race on bus low-passes, the WASAPI reroute against the fallback, and out of memory caught
at every C entry that allocates, swept by a test that fails each allocation in turn. 64 channels mix
in 0.18 ms.

## E1, app and input

Steps 3 to 7 (2026-10-31, the input lane, landed at 3698cf3; steps 1 and 2 came with E0). Step 3
added the keyboard: `Key` by position, `KeyDown`, `KeyHit`, and `KeyUp` counted per Flip,
`FlushKeys`, and text input with IME composition (`TextInput()`, `StartTextInput`, `StopTextInput`),
with Simulate's keys and text for tests. Step 4 added the mouse: position in window units, the
wheel, speeds since the last Flip, buttons, `MoveMouse`, the cursor, and relative mode for mouse
look, with no warp emulation. Step 5 added gamepads and joysticks: buttons, axes past round dead
zones, rumble, connections, GUIDs, serials, and player indices, raw joysticks, and Simulate's
virtual devices; OS devices go by the OS's own IDs. Step 6 added input actions: an `ActionSet` of
`Action`s bound to keys, buttons, and axes, one- and two-axis, read at each Flip (`Down`, `Hit`,
`Up`, `Value`, `Vector`), saved and loaded as TOML, rebound from the next input pressed
(`InputPressed`), with `Binding.Reads` and `ActionSet.BoundTo` for a bindings menu;
examples/controls shows them. Step 7 added `ShowMessage` and `OpenURL` in a new app area, which open
nothing headless (`OpenedURLs()` for tests), and the done-when, every kind of input read in one
Flip. Six reviews' findings were all fixed with tests, among them presses counted for a key held
when a set was turned on or rebound, Ups lost to a rebind, a handover between bindings counted as a
press, two pads on one button counted twice, scales that weren't finite, links Windows' shell opens
as other files for non-ASCII and UNC paths, a closed window's mouse position kept, and the headless
guard misreading SDL's driver list. A seeded fuzz of presses and releases over several bindings and
pads checks that every Flip's Hits and Ups are what some order of its events would give.

## E10, fbengine_physics

Steps 1 and 2 (2026-10-31, the physics lane, landed at 8720580). Step 1 added fbengine_physics, a
package over its own DLL, `fbengine_physics_native`, with Jolt v5.6.0 linked in (its debug symbols
only in a debug build): worlds as handles (`fbe_physics_new`, `_free`, `_step`, `_gravity`,
`_set_gravity`, `_steps`, `_overflowed`, `_threads`, `_version`, `_error`), stepped on one shared
Jolt thread pool of a thread per core less one, with x86 held to SSE4.2 and no fused multiply-add.
`PhysicsWorld` has `Gravity` and `SetGravity`, `Step` for a given time (a request of many collision
steps split over several of Jolt's updates, so its 2048 jobs are never exhausted), `Update` at a
fixed `StepRate` up to `MaxSteps` a frame with `Alpha` for drawing between steps, `Steps`,
`Overflowed`, and `Free`; a world dropped without Free frees itself. Step 2 added bodies and shapes:
`Shape` makes boxes, spheres, capsules, cylinders, convex hulls, triangle meshes, and compounds
(`fbe_shape_*`), each a refcounted Jolt shape any bodies of any worlds share, and freed when
dropped; `Body(world, shape, motion, position, rotation)` puts one in a world as static, dynamic,
or kinematic (`fbe_body_*`), with its velocities, forces, torques, impulses, mass, friction,
restitution, damping, gravity factor, and sleep. A shape that must be static (a mesh, or a compound
with one) is refused as dynamic; the broad phase is sorted as bodies come and go; Jolt starts with
the first shape or world. Handles carry their pool's kind (96 to 98), so one pool refuses another's.
examples/falling_boxes drops 200 boxes that come to rest; 1000 mixed bodies on a 20000-triangle mesh
step in 0.8 ms. Two reviews found 13 problems, all fixed with tests: a crash for a shape made
before any world, a dynamic compound with a mesh part, Jolt's job limit at many collision steps on
32 threads, the broad phase's nodes running out between steps, 45 MB of debug symbols, handles
without a kind, unnormalized rotations, and leaks of dropped worlds and shapes among them.

## E0, fbengine's setup

(2026-10-31, in hlpdev/fbengine.) The repository, public under Apache-2.0, with FutureBlitz's
.clang-format and .clang-tidy; CMake reading the version from packages/fbengine/project.toml, and
configuring again when it changes; cmake/native.cmake's `fbengine_add_native`, a package's DLL
behind a C API that exports only its prefix's functions, with its C++ runtime linked in, copied
into the package's `native/<target>/`; the fbengine package, `Version()` over `fbe_version()` in
fbengine_native, through fb-bindgen's bindings in c/; examples/hello; and ctest running `fb test`
and the example. CI: native.yml builds each target's DLLs, Linux's in manylinux_2_28 for glibc
2.28; ci.yml runs the package's tests and the examples on fb 1.0.0's release on all five targets,
clang-format 19.1.5 and `fb fmt --check`, and the GPU tests; publish.yml, run by hand on a
v<version> tag, publishes every package through trusted publishing and then adds and runs it from
the registry on all five targets. fbengine 0.1.0 (from the user's `fb login`) and 0.1.1 (trusted
publishing) are on staging. The GPU test makes a Vulkan instance and device through volk, with
no window, preferring a CPU device, clears an image, and reads it back: on lavapipe on Linux
(apt's) and Windows (mesa-dist-win's MSVC build, registered in the registry, since the loader
ignores VK_DRIVER_FILES for an administrator, and MSYS2's MinGW build corrupts its heap freeing
host-visible memory), and on MoltenVK on Apple silicon; Intel Macs' runners have no Metal and
Homebrew's Mesa no lavapipe, so their GPU tests are run by hand.

## M10, packages online

fb's side of steps 1, 2, 5, and 6 (2026-10-30, the m10-client lane), over the design in
libraries.md and registry-api.md's wire format, which futureblitz-web's workers/api serves: semver
versions and Cargo's ranges (`fb/semver.hpp`), with pre-releases only where a range names one;
project.toml's registry dependencies, `[registries]` (https, or http on this machine, or a folder,
a registry on disk), and `[project]`'s description, license (an SPDX expression), license_file,
readme, repository, homepage, documentation, keywords, and fb; a backtracking resolver, newest
first but fb.lock's versions first, that skips yanked versions fb.lock lacks and versions whose fb
range this fb isn't in, and names who asks for what in a conflict; fb.lock, flat, by name, with
each archive's SHA-256 and every target's; the cache in FB_HOME or ~/.futureblitz, of index files
with ETags, archives, and unpacked packages with markers, written whole or not at all; `.fbpkg`
archives, gzip'd ustar with pax for long paths, a package's own and one per target of its
`lib/<target>/` and `native/<target>/`, unpacked only after the registry's own path and size
checks; the driver finding registry packages through fb.lock alone, offline, with messages saying
what to run; libcurl from the compiler's library, with the runtime's OS roots; `fb add`, `update`,
`search`, `publish` (with `--dry-run`, uploads in parts, docs JSON, and trusted publishing from a
GitHub workflow), `yank`, `owner`, `login` (the registry's sign-in page and polling, or `--token`),
and `logout`, with tokens in Windows' Credential Manager, the macOS keychain, libsecret, or
FB_CREDENTIALS_FILE; and `--offline` for the commands that build. Tests run against registries on
disk and a registry the tests serve over HTTP on 127.0.0.1.

After it, the same lane: a test against workers/api itself under wrangler dev, given its URL
(FB_TEST_REGISTRY), which logs in with a token, publishes, searches, and adds and builds on a
second FB_HOME; downloads into a file as they come, and archives unpacked a piece at a time, at
the registry's own limits, in decimal megabytes; and the review's 35 fixes. The resolver jumps
back to the choice that conflicts (resolve-1), and names the newest version in a conflict
(resolve-5); fb.lock pins a registry on disk as `path+` its folder (resolve-2, archive-11), quotes
target keys and records sizes (resolve-7), and fb update drops a yanked version (resolve-3); an
index line's registry is canonical and mapped through a futureblitz mirror (resolve-8); schemes
are http, https, or file in any case, and a folder without config.json is no registry
(resolve-6); a range of an operator alone is refused (resolve-4); a description is one line of
300 UTF-16 units (resolve-9). Archives keep to registry-api.md's tar subset, with a pax size
honored and its keys checked (archive-1), kinds and markers compared folded (archive-2), paths
folded after NFKC (archive-3), Windows' device names and C1 controls refused (archive-9), and
limits said as they are (archive-10); fb publish checks unpacked sizes and file counts
(archive-6); a source archive never joins a folder that's there (archive-7), stale partial folders
go by the time in their names (archive-8), and a path too long for Windows names itself
(archive-4). fb login shows its code, then opens only an https or local page with a detached
opener (commands-1, -10), checks its store first and never loses a token (commands-3), and polls
through 429s, 5xx's, and the network failing (commands-4); tokens go only to an API over TLS or on
this machine, and config.json is fetched once a session (commands-2, -13); a network registry
decides a target's size (commands-6); the credentials file takes an empty file, trims and checks
tokens, and is made owner-only from the start (commands-7, -8); fb yank and owner check names
(commands-9); fb publish --offline needs --dry-run (commands-11); search on disk takes any word
in any case (commands-12); and `apt`, `rpm`, and `releases` are reserved as workers/api has them.

M9's and M10's milestone reviews, fb's share (2026-10-30, 18 findings and two of the site's).
Search on disk needs every word, as the API's does (m10-journey-1). fb owner lists without a token,
with the pending invites (m10-edges-5), says "Invited", "Added", or "Removed" and the rest
(m10-edges-11, m10-journey-11), and a token without a scope says how to get one with it
(m10-edges-6). Every command says a folder without config.json is no registry, and suggests
https:// for a host's name (m10-edges-7). fb login --token asks GET /me before it keeps a token,
and both logins say the account (m10-edges-9, m10-journey-3). The no-fit error lists the newest
five by version, and says a yanked fit or an fb range only when one ruled a version out
(m10-edges-10, m10-journey-10); fb add says "Added" once it's locked (m10-journey-10). Sizes are
the registry's decimal units, Uploading's too (m10-edges-12). A download fb can't write names the
file, and MAX_PATH, not the network (m10-journey-6). fb help add's --registry is a name, and
publish's --offline is with --dry-run (m10-journey-2, -12). registry-api.md has the targets of
GET /packages/<name>, and no `fb whoami` (m10-journey-14); spec 11's fb range is ">=1.2" and links
the package manager's docs (m9-site-16); spec 12 says a Buffer's slice, unlike an array's, is a
view (m9-site-6). install.sh and install.ps1 check the release's SHA256SUMS, find the latest by
releases/latest/download/ with no API call, and install.sh runs from main on its last line
(m9-ops-5, -6). A tag's release fails without the signing secrets, and release-setup.md has a
launch checklist with tools/release/launch_check.py (m9-ops-7). With futureblitz-web's API fixes:
registry-api.md is its copy, with fb's changes; trusted publishing refuses a package its token
isn't for, as a new one, before uploading (m10-journey-9); a 429 or 503 `unavailable` is asked
again after Retry-After, and the sign-in's polls wait on a 503 as on a 429; a target's download
says its target (m10-journey-16). Spec 12 is "C Interop", spec 10's loops are a UI toolkit's, a
terminal UI's, or a package's own, and Humanize's examples are files and people.

The registry's side, in futureblitz-web (2026-10-30, private): workers/api, one Worker in TypeScript
(Hono, Drizzle over D1, R2, Queues, a Durable Object per package), serving registry-api.md's wire
format: accounts by GitHub or by email with passkeys, sessions, recovery codes, scoped tokens,
trusted publishing from GitHub Actions' OIDC (private repositories kept by name until their first
token), teams and invites, publishing in parts within the strict tar subset, the sparse index and
downloads, search, yanks, the audit log, and the daily cron; its review's 50 fixes (the tar
reader refusing what other extractors read differently, scoped tokens kept out of teams, invites
revocable) and two more rounds'. apps/pkg, the package site (Astro on Workers, the API behind a
service binding): search, package pages with the readme, docs from `fb doc`'s JSON, versions, and
dependents, people and teams, the dashboard (tokens, owners, trusted publishers, teams, the audit
log, passkeys), fb login's page, the policies, /status, and a strict CSP; its review's 49 fixes.
Step 8: Workers Logs, GET /health and /status, plain HTTP redirected, a nightly D1 export to R2
from one snapshot kept 35 days, scripts/restore.mjs, an admin's rebuild of the index from D1
after a restore (lost names reserved, a lost version's number never published again), a
read-only mode, and the restore runbook; its review's 21 fixes, and a race's 500 made a 409.
staging-pkg.futureblitz.dev serves it all. The milestone's review, with M9's, found 62 across fb,
the API, and the sites, all fixed with tests: search ANDs its words everywhere, the install
scripts check SHA256SUMS and don't ask GitHub's API, `fb login --token` asks GET /me first,
tools/release/launch_check.py checks a release has every promised install method, and a version
whose dependencies don't exist, or with no description, is refused. M10 is done on staging;
production goes up with v1.0.0, after docs/release-setup.md's steps.

## M9, futureblitz.dev

futureblitz.dev, in futureblitz-web (2026-10-29 to 30): the design (terminal noir with a Blitz
nod, the bolt-stem logo, dark only, no playground) and docs/site-plan.md, the user's plan of every
page; apps/site in Astro with the FutureBlitz grammar in Shiki and Pagefind, its first build
replaced after the user's review by the planned one: the landing page and /download (the OS
detected, every install method), Learn's 13 areas of tutorials and three projects (every example
run, shown with its real output, and fb fmt-clean), Docs (the language reference from docs/spec,
each fb command, project.toml, the guides, packages, the changelog), std's reference with a page
per item from `fb doc`, the policies, and packages/ui's facts.js as the one place for every fact;
a copy check that fails a page with a date, a milestone, provenance, an unfinished word, an
engine name, an undocumented std item, or a fact stated outside facts.js; a strict CSP and HSTS;
Lighthouse's budget on every page (JavaScript, LCP under 1.5 s, CLS under 0.1); and axe on every
page at phone and desktop widths. Each lane was reviewed (Learn's 55 and 40, Docs' 37), and the
milestone's review, with M10's, fixed staging's indexing, the analytics beacon under the CSP,
LCP, 301s for moved pages, a sitemap, tables at phone width, the side nav's layout shift, and
pages that disagreed with fb or each other. staging.futureblitz.dev serves it; production goes up
with v1.0.0.

## M8, tooling

The engine ruling (2026-10-30): the engine is packages on top of FutureBlitz, so nothing in the
language names it. `Flip` left sema's names that later milestones declare. The main scheduler runs
at a top-level Await, Wait(), and the end of top-level code, and `Scheduler.Main`, which sema checks
as main-thread code (`check_main_scheduler()`) and lowering gets from `fb_scheduler_main`, an object
the runtime keeps outside the leak report, gives a loop of a program's own `Step()`
(`fb_scheduler_step`, a Scheduler's Tick of no time); `Scheduler.Main.Tick(seconds)` is a compile
error, and `fb_scheduler_tick` panics at its call for the main scheduler. `NextFrame()` became
`NextTick()`, WAIT_FRAME WAIT_TICK, and `fb_wait_frame` `fb_wait_tick`. tests/programs'
scheduler_main, scheduler_no_step, and panic_scheduler_main_tick show it. std.term's `Screen.Run`
steps it after each event, and while a coroutine waits polls for keys 10 ms at most, which a
Terminal's Package `IsAtEnd` tells from the input's end (term_screen_coroutines). docs/spec was
rewritten as a reference at 1.0, with no dates, provenance, or game or engine examples: its
examples are accounts, files, orders, shapes, a UI, and a server, and spec 08's engine resources
are native resources. Its review's fixes: only a top-level Await, Wait(), and Step run the main
scheduler, and a wait for a Task that's done doesn't (scheduler_done_wait); once the program's code
has ended, a tick of the main scheduler destroys its coroutines rather than resuming them, and a
Wait for a destroyed coroutine's Task panics (scheduler_end, panic_scheduler_end_wait); overloads
that fit but for their ByRef pick the one that isn't generic, for the ByRef hint; and
Scheduler?.Main and Scheduler.Main<Int> say what's wrong.

The installers, after the milestone (2026-10-30): a release is versioned from its tag
(`-DFB_VERSION`, refused unless three numbers) and has, beside each target's archive, setup.exe (a
WiX Burn bundle around the MSI), .deb and .rpm (nfpm, in /usr/lib/futureblitz with links in
/usr/bin), and the .vsix; Windows' executables, the MSI, the bundle's engine, and setup.exe are
signed with Azure Trusted Signing when its secrets are set. tools/release/packages.py writes winget's
manifests and the Homebrew formula from SHA256SUMS, and tools/release/repo.py the apt (by-hash) and
dnf repositories, signed with REPO_SIGNING_KEY; publish.yml, dispatched by hand, sends each where it
goes, packages first and signatures last. The review's 12 findings were fixed with tests, and
docs/release-setup.md has the user's one-time steps.

Step 9, loose ends and the milestone's review (2026-10-30, 5f908f5c to 31b7784c): a syntax error
right after a miscased keyword (`if`, `End function`) gets a note naming the keyword and a fix-it
that recases it, from the parser's `suggest_keyword_case` and token.cpp's `miscased_keyword()`,
only at or just before the error or first on its line and never for a name the code declares, its
LSP quick fix titled for the keyword. The milestone's review (docs/review/m8-step9.md) found 20, 1
high, and CI 4 more, all fixed with tests: fb-lsp and check_files() take std from
`toolchain().std`, so a release's std isn't checked or renamed as user code; a prebuilt package's
`fb doc` lines are its source's, and `fb test` on one links; nlohmann/json's notice ships with
fb-lsp; rename and references reach doc-comment examples; `fb fmt` keeps a doc comment's trailing
spaces; go to definition opens a prebuilt package's .fbi; the grammar knows `//!`; Import
completion leaves out a package's tests/ modules from its own files; install.ps1 keeps the
session's variables under `irm | iex`; a failing doc test's trace names the test; `fb doc`'s
warnings follow FB_COLUMNS; `fb new` names a keyword folder as a keyword; a rename to a name the
code already sees (Print, Int) is refused on Linux too (a range-for over a temporary's member in
`visible_names()`); a no-folder build task's problems reach Problems (`FB_PATHS=absolute` and an
owner of their own); the VS Code client waits up to 2 seconds for a server it stops as it comes
up, so its features don't outlive it; and macOS executables that run the compiler get a 64 MiB
stack, as Apple clang's unoptimized frames are larger than GCC's. M8 is done.

Step 8, `///` doc comments and `fb doc` (2026-10-29, 05ddb713): the lexer records the doc comments
outside the branches an #If leaves out, and the parser attaches the `///` lines right before a
declaration and its attributes to it in the AST, by its name's span, as `ast_t::doc()` gives them to
the LSP's hover: members of Structs, Classes, Interfaces, Extends, and Extern blocks, and Enum
members too, and a file's top `//!` lines to its module. `read_doc()` reads their Markdown Rust's
way: the first sentence is the summary, headings start sections, `[Name]`, `[Type.Member]`, and
module-qualified links name declarations, found as code where the doc is finds names, and the fences
under `# Examples` are examples, `fb no_run` ones only compiled, with `# ` lines hidden. `fb doc
[package | std]` writes the package's public API as JSON, format 1 in tooling.md: modules, Public
declarations, Operators and Overrides among them, a Class's Protected members, Extends of what users
see, signatures as written, attributes, source files and lines, docs, and each link's target, a
member without a doc taking the one it implements or overrides; it warns about a link that names
nothing and a doc comment on nothing. `fb test` runs a package's examples, a lone file's, and std's
(`fb test std`), each a module of `<name>.doctests` that uses the package from outside, with
std.core, std.test, and its declaration's module imported, named after its declaration, whose lines
a file's line origin maps back to the doc comment, so its errors, failures, panics, and traces show
the comment's lines. A prebuilt library's interface file keeps its docs, which its ABI hash leaves
out. std's comments on its declarations became `///` ones and its files' first comments `//!` ones;
every Public declaration, Enum member, and Operator has a summary, a test checks they're all in `fb
doc std`'s JSON, and std's docs warn about nothing. The LSP's hover shows a declaration's `///` doc.
Its review's 26 fixes are in docs/review/m8-step8.md.

Step 2, releases (2026-10-29, 19d46557): `fb --version` prints the version, the commit, from git
when CMake configured the build, only for fb's own checkout, or "unknown", and the target, `fb 0.1.0
(abc1234, windows-x64)`; `fb new <folder>` makes a package there, named by the folder's own name,
`fb new .` too, checked as project.toml's names are (package_name_error()), with project.toml, a
main.fb that imports std.core and prints "Hello, world!", and a .gitignore of build/, and refuses a
folder that isn't empty and Windows' devices' names. `cmake --install <build> --prefix <folder>`
writes a release (cmake/install.cmake): bin/ with fb, fb-bindgen, fb-lsp, lld, and what they load,
lib/<target>/ with the target's C headers in its include/, lib/clang/ with clang's, notices/, std/,
and lldb/futureblitz.py; fb-bindgen finds both through toolchain()'s clang_headers and c_headers, or
on macOS the SDK's from xcrun. fb's executables run with nothing else installed: MinGW's runtime
linked in where its winpthread allows, libstdc++ and libgcc on Linux, LLVM's static libraries on
Linux (FB_LLVM_STATIC), and the shared libraries they and lld load, but the system's, copied beside
them by cmake/bundle.cmake with a RUNPATH of $ORIGIN or @rpath names, signed again on macOS, each
one's license in notices/shared/; the release's own notices, LLVM's, MinGW-w64's and winpthreads',
GCC's, glibc's, the Linux headers', libxml2's, and toml++'s, are in cmake/notices/. FB_RUNTIME_ONLY
builds only lib/<target>/, which the Linux release builds in a manylinux_2_28 container against
glibc 2.28, while fb is built on Ubuntu 22.04; on macOS fb needs macOS 15, as Homebrew's LLVM does.
tools/install/install.sh and install.ps1 install the latest release, a version, or an archive under
~/.futureblitz, check it runs before replacing anything, and put its bin on PATH, keeping the user's
PATH as it was; the MSI (WiX 5) installs the same folder per user, and the .pkg
/usr/local/futureblitz with /etc/paths.d. .github/workflows/release.yml builds windows-x64,
linux-x64, linux-arm64, macos-arm64, and macos-x64, installs each with its script, the MSI, and the
.pkg on a fresh runner or a plain ubuntu:22.04 container, runs `fb new hello` and `fb run`, runs a
program built there in almalinux:8, and for a pushed v<version> tag makes a draft GitHub Release.
What it found: a Mach-O trace entry is a symbol of its own, so programs link on Intel Macs; the
runtime is position-independent where the compiler doesn't default to it; and programs link glibc
2.28's libdl and librt. Its review's 16 fixes are in docs/review/m8-step2.md.

Step 4, the LSP (2026-10-29, 672b4377): fb-lsp, an executable beside fb that an editor talks to in
JSON-RPC over stdin and stdout, read and written with nlohmann/json 3.12.0, in UTF-16 or UTF-8
positions: diagnostics as you type, hover with a declaration's signature, constraints included, and
the comments above it, or an expression's type, go to definition, find references, rename of a
symbol's whole family of overrides and implementations in the workspace's files, refused for a name
that's taken or seen there, completion of members after a '.', Super's included, names in scope,
types and type parameters where a type goes, modules after Import and From, and keywords, document
and workspace symbols, signature help over every overload, semantic tokens, code actions for
diagnostic_t::fixes, and formatting through format_source with the package's width; on a front end
that re-checks only what an edit touches: the driver's check_files() checks some files of a package
(with its tests/ folder), of std, or a loose file, with what they import and the open documents'
unsaved texts (source_reader_t, compilation_t::on_disk), each workspace package is checked whole
once in the background for every file's diagnostics and the import graph, an edit checks its file
after 150 ms and then, in the background, the files that import it, and checks share what std's
unloaded modules declare (std_names_cache_t). Sema records each use of a symbol (analysis_t::uses):
assignments' targets, Enum values, the types after Extends and Extend and before a Static member,
Aliases, module qualifiers, named arguments, and a From line's names, which a test of every kind of
use checks; and works out what's before the '.' of "x." being typed, which the parser keeps as a
member with an empty name. tests/lsp drives the server through its messages in process and fb-lsp
over pipes. Its review's 35 fixes are in docs/review/m8-step4.md.

Step 5, the VS Code extension (2026-10-29, 6267d7dc): editors/vscode/, in TypeScript with a pinned
package-lock.json. The TextMate grammar, syntaxes/futureblitz.tmLanguage.json
(`source.futureblitz`), is the one M9's Shiki will load: keywords in their case only, the contextual
ones where they're keywords, attributes, strings with escapes, interpolation, and format specs, raw
and triple-quoted strings, Chars, numbers, nested and `///` comments with their Markdown, #If lines,
declarations' names, parameters, and types, type arguments, calls, members, constants, and
operators, a single-line string and its interpolation ending with their line. The language
configuration toggles comments, closes brackets and quotes, carries `///` on, folds by indentation
and #If, and indents the blocks. vscode-languageclient starts fb-lsp, from futureblitz.lspPath,
beside fb, a build folder's lsp/, or PATH; missing or failing, it says so once and the rest works
without it, and saving formats once, through the editor's format on save. Tasks run fb build, run,
test, fmt, and check on the folder's package or the open file, with problem matchers for fb's
diagnostics, notes, panics, uncaught errors, and failed Expects; they set `FB_COLUMNS=utf-16`, which
makes fb count its diagnostics' columns in UTF-16 code units, as editors do. A `futureblitz` launch
builds in debug as a task and hands CodeLLDB, an extension dependency on every OS, an lldb
configuration that loads lldb/futureblitz.py, the one beside fb or the extension's copy; F5 with no
launch.json debugs the open file's package. Snippets for the blocks, and `npm run package` makes the
.vsix; publishing waits for the accounts. `npm test` runs the grammar's assertion files and
snapshots, a sweep of every .fb in the repo, every keyword in token_kinds.def, the indentation rules
over std, each snippet through `fb fmt`, and the problem matchers over fb's output, then the
extension in VS Code through @vscode/test-electron, with stand-ins for CodeLLDB and the server, and
against fb-lsp itself. Its review's 23 fixes (docs/review/m8-step5.md): the grammar's type arguments
end with their line and hold no `&&`, a parameter list's brackets balance, Set is an accessor only
in a Property, Step a keyword only after a bound, Attribute declarations, any type after a lambda's
`:` and an Operator's type parameters, and a colon the ternary's only after its `?`; the indentation
rules don't read Then or => in strings and comments, and a number is one word; starts that overlap
leave one server, and one that doesn't answer fails after 20 seconds; $fb-runtime takes an uncaught
error with no message, Fail, and thrown errors; a bad futureblitz.path is named; the Operator
snippet compiles; debugging takes the program's own folder, works with none open, builds a package's
file into its build/, refuses a library, and takes CodeLLDB's settings; the README's link; and
diagnostics' columns in UTF-16.

Step 3's Linux and macOS fixes (2026-10-29, db810ec8), found by the user's Mac and narrow CI runs:
on ELF the trace table is added from .init_array, since lld doesn't run the .ctors that LLVM's C
API's target machine writes, so Linux printed no trace at all; the runtime's parts have unwind
tables without exceptions, which Apple's clang leaves out on arm64, so macOS's unwinder gave no
frames; and FB functions on Apple's targets keep a frame pointer, as its ABI has. A Function's entry
jumps to its code at its first statement's line, so stepping in stops there; a String?, a Class?,
and a T[]? are a struct of their Value, so lldb shows Null; and tools/lldb/futureblitz.py hides
addresses, finds std's Map by its full name, and shows an object's Fields through its pointer.

Step 1, the self-contained toolchain (2026-10-29, e50d0fe6): fb links with its own lld, run directly
as `lld -flavor gnu` or `-flavor darwin` through a response file, quoted as Windows' command lines
are on Windows and as a POSIX shell's elsewhere, with each target's startup files and libraries from
its toolchain, and no C++ driver: MinGW-w64's crt2.o, crtbegin.o, and the rest, libstdc++, libgcc,
UCRT's and the system's import libraries on Windows; glibc's and GCC's startup files, glibc's
libraries, their linker scripts written again to name their files beside them, and libstdc++ and
libgcc static on Linux; and on macOS the SDK, from `xcrun`, which fb says how to install when it's
missing, with libSystem, libc++, and the compiler's builtins. fb/toolchain.hpp's toolchain() finds
lld, lib/<target>/, notices/, and std/ beside fb in a release, or in the build folder, where
cmake/toolchain.cmake copies the C runtime's pieces from the C++ compiler that builds the runtime
and the build copies the runtime's archives and std's bundled libraries, so the tests link as a
release does; build_config names them by their files. Windows' code page workarounds are gone, as
lld reads its arguments as UTF-8. On Linux a package's system library is found by its development
link or else by the file the system loads, like libGL.so.1. A Windows debug build writes a PDB
beside each binary, and std's files' debug info points at the toolchain's std. A program builds and
runs with no C++ compiler on PATH, on Windows, Linux (the whole of ctest), and macOS (its tests, on
the user's Mac and in CI). Building the Linux release against glibc 2.28 is step 2's.

Step 6, the formatter (2026-10-29, c7a74326): `fb fmt [files and folders]`, which formats .fb files
in place, a folder's, or the current package's, but for hidden folders and a package's build/, with
`--check` listing those it would change, and project.toml's `[format] width` (40 to 1000, 100
without it); compiler/src/format.cpp's format_source(), which the LSP will call, laying out
lex_layout()'s tokens (trivia_t: comments, and each outermost #If's lines, left as written) by
parse_layout()'s layout_t (each line's block depth, the line breaks that end statements, where one
may follow outside brackets, and tokens' roles), and refusing any output whose tokens or tree differ
from the file's; std's style, as tooling.md now writes it, settled with the user: 4 spaces a block,
Cases a block under Select, a statement going on after a comma, operator, `=`, `=>`, or opening
bracket, lined up one past the innermost bracket, items packed, or with its expression's start, a
bracket that ends its line 4 deeper and a signature's 8, the author's line breaks kept and lines
never joined, a too-wide line broken at the loosest break that fits, wide characters counted as two
columns, and a statement that can't fit left as written; map literals tight, `{"a": 1}`; sema's
warning, with a fix-it (fix_t, diagnostic_t::fixes), for a constraint written the other way than
spec 07's, one inline and more on Where lines, which fmt never moves; and std, tests/programs, and
tests/packages formatted, with a test that they stay so. Its review's 24 fixes
(docs/review/m8-step6.md): `>=` and `>>=` after type arguments, `- -a`, the next-best break when the
best leaves too wide a line, comments that share a line, a comment at the end of a Select's last
Case, a block of only comments or #If lines, attributes on a declaration's line, and a block comment
that ends on an #If's line; an #If that holds part of a statement says so; the constraint warning's
relations, its message, and its fix-its' line breaks and comments; and `fb fmt`'s .FB files, files
reached twice, project.toml's warnings, and its options' messages.

Step 7, testing (2026-10-29, e111738b): `@Test` Functions, which take nothing and give nothing back,
though they can throw, anywhere in a package, seeing their file's Private names, and a package's
tests/ folder, a package of its own, `<name>.tests`, which depends on it and on what it depends on,
so its tests use it from outside; no code uses a test, `fb build` and `fb run` leave tests out and
globaldce the code only they reach, and a library's binary and interface file leave the tests out.
std.test's `Expect`, which on failure says the condition as it's written and where, and a
comparison's sides' debug forms, worked out once, and goes on; `ExpectThrows(x)` and
`ExpectThrows<E>(x)`, which catch as Try? does and give the error; `Fail`; and `Skip`, which ends
its test as a Return would, only in a @Test Function's own code. They're compiler built-ins that
only a file that imports std.test has, over the runtime's `fb_test_` functions. `fb test [package]
[--filter <name>]` builds a test executable whose main hands the runtime's runner each test; the
runner runs them on the pool, but for those that use what only the main thread can, which run on the
main thread, and writes each one's start, failures, and end to FB_TEST_REPORT's file, a panic's
through a hook. fb starts the executable again after a test that panics, with FB_TEST_PLAN's file
listing the rest, one at a time when no test is to blame, then prints each result, the failures, and
a summary with how many were filtered out, and exits 1 on a failure. In debug builds each object
records the test that made it, work a test starts on the pool or a Thread included, and what's left
once the Globals are let go of is that test's leak, which fails it. Its review's 19 fixes
(docs/review/m8-step7.md): work a test starts belongs to it wherever it runs; a panic, an early end,
or a leak outside every test fails the run; a Global's panic shows its text; Ctrl+C stops fb test;
another main file's top-level code and a tests/ folder with its own project.toml stay out of a test
build; a package's own code can't import its tests/ folder; Skip without the import, ExpectThrows as
a value, and From std.test or Import std.test As each get an error that says what to do; @Test
Async, Const, and Unsafe each say what's wrong; and spec 09's example and its rerun rule. Doc tests
wait for step 8's `///` comments.

Step 3, debug info and stack traces (2026-10-29, 297a90f2): codegen describes the program through
LLVM's DIBuilder, with a compile unit for each file a function is in, its functions by their
FutureBlitz names, each statement's line from the places MIR now keeps, a lexical block for each
scope that declares Locals, and in debug builds the parameters and Locals with their types (Structs,
tuples, and value T?s as structs, Enums with their members, and Strings, Classes, and T[]s as their
names for pointers to their objects, header first, each Class's own struct named by its module),
DWARF 4 everywhere and CodeView too on Windows; release builds have the line tables, and macOS
builds a .dSYM. Panics and uncaught errors print a stack trace, " at <function> (<file>:<line>)",
from fb's own table in every binary, a beside library's too (compiler/src/trace.cpp): after
optimizing, an inline-assembly marker before each call names its place and the calls it's inlined
into, and the runtime walks the stack with the platform's unwinder and finds each return address's
marker (runtime/src/trace.cpp), showing at most 100 places. No build makes tail calls, so no caller
drops out, and an error keeps the stack of its first Throw until it's released (both the user's
calls). tools/lldb/futureblitz.py shows Strings, arrays, Maps, nullables, and objects, by their own
Class, in lldb and CodeLLDB. The program tests and verify.py compare a trace only for programs whose
name says "trace". A beside library that takes what its executable lacks no longer waits on Windows'
"entry point not found" box. Its review's 21 fixes (docs/review/m8-step3.md): a checked operation's
panic and ok blocks stay in source order, so a breakpoint on a line after one stops; sibling scopes'
Locals of one name each show their own; code with no place starts at line 0, not the last block's; a
beside library built from source gets its folder; a captured Local shows its value, and an Async
Function's Locals and name after an Await; tail calls are off before optimizing; a frame with no
line keeps its function; a kept error's stack costs less, and one walk serves every platform; a cut
trace says so; a release at a function's end is at its Local's declaration; and the trace filter,
the lldb script, the debugger tests, and the docs.

Step 0, the design (2026-10-29): tooling.md's decisions 5 to 12, with the user: fb runs its own
lld, Linux programs link against glibc 2.28's stubs, releases on GitHub Releases as archives,
install scripts, an MSI, and a .pkg, unsigned; stack traces from fb's own table; CodeLLDB on
every OS; `@Test` Functions anywhere and a tests/ folder; and Markdown doc comments.

## M7, libraries and packages

M7 (2026-10-28 to 29) is done: steps 0 to 7, on Windows, Linux, and macOS.

Step 0, the spec: spec 11's `type` (`"executable"` or `"library"`), `link` (`"embed"` or
`"beside"`), and its Libraries section: each package in one place, a package two binaries use
beside, the runtime and std always in the executable, beside libraries loaded by the executable
itself as it starts and reaching it through its exported symbols (-rdynamic, -undefined
dynamic_lookup, and the import library, so a Windows executable keeps its built name), and the
edge's rules; prebuilt libraries, `build/<target>/<mode>/<name>.fblib` and the `.fbi`, which holds
every Public declaration, the Private and Package ones generic and @Inline bodies reach, and every
Field, under a first line with the build's name, version, and ABI hash; dynamic `[native]`
libraries and `{target}`; and package archives. Its review's 23 findings were fixed or, for the
plugins and run-time loading the user then left out of M7, dropped (docs/review/m7-step0.md), and
step 5, plugins and DynamicLibrary, left the build order, so steps 6 to 8 became 5 to 7.

Step 1, project types and the interface file: project.toml's `type` (manifest_t::type and
package_type_t), with an executable needing its main file, a library's main.fb a module, and
an executable as a dependency an error; the interface file, compiler/src/interface.cpp's
write_interface(), which `fb build` on a library writes to build/<target>/<mode>/<name>.fbi
(build_target(), mode_folder()): FutureBlitz source lexed for the build, so #If is resolved,
with each module after `// module <path> <lines>`, its Imports, every Public declaration and
what kept code names, every member of the types it keeps and every Extend, generic, @Inline,
and Const code whole, the rest as first lines and `=> ...` (function_stmt_t::prebuilt,
header_end, modifiers_t::start), Globals, Static Fields, and Class Fields as `= ...`
(variable_stmt_t::prebuilt), a type the source left out printed after a hidden Import, Consts
as values, and `@MainThreadOnly("why")` from analysis_t::main_thread_only, under
`// fbi <name> <version> <hash>`, a BLAKE3 hash; and the loader reading a package's
lib/<target>/<mode>/<name>.fbi in place of source (read_prebuilt(), interface_modules(),
parse_into's interface_file), with fb check on a prebuilt package checking its modules and a
program's build stopping until step 2 links the .fblib. Its review's 32 findings were fixed
(docs/review/m7-step1.md): a kept type's Operators, Private Methods, and Interfaces, every
Extend, generic ones too, the main-thread rules through prebuilt code, a prebuilt New's
Super.New, Class Field defaults that embed, .proto modules, hidden Imports by path and with
free qualifiers, where `= ...` goes, CRLF, a missing build for the mode, main.fb's letter
case, and clearer messages.

Step 2, prebuilt libraries embedded: `fb build` on a library writes its binary,
build/<target>/<mode>/<name>.fblib (write_binary(), read_binary()), beside its .fbi: a line
naming its build, one naming the bundled C libraries its code calls, and its code as LLVM
bitcode (emit_bitcode()), from lowering the library's build (lower()'s `library`), all of its
own code, an initializer and a releaser for each module, and its Classes' debug forms. MIR's
mir_linkage_t says what's the library's own (EXPORTED), a prebuilt one's (IMPORTED), and what
every build shares (SHARED, linkonce_odr); a program's build declares a prebuilt function with
its parameters (declare_parameters()), calls each prebuilt module's initializer in the modules'
order, links each binary into its module (link_libraries()), merges the Classes' list
(finish_class_list()), and makes all but main internal before it optimizes; the ABI hash covers
the bitcode, and a binary that isn't its .fbi's build is an error naming both
(prebuilt_binaries()). A Field's default is written in the .fbi as its value. Its review's 27
findings were fixed (docs/review/m7-step2.md): itemless and dependent prebuilt modules'
initializers, ThreadLocals and worked-out Consts, a module that starts with a type, alike
names, debug forms, shared T[] and Channel records, the program's Extends on a library's
objects, fb's own names for the Class list, LLVM's errors as fb's, `--show llvm`, the same
build wherever it's built, and the docs.

Step 3, libraries beside the executable (7dadf64f): project.toml's `link = "beside"`
(dependency_t::beside, link_span; "embed", the default, or an error), only a program's own, with a
warning that a library's is ignored; build_program's beside libraries (beside_library_t,
compilation_t::beside, module_t::beside), each its prebuilt binary or a build of its source
(beside_binary()), in dependency order, as the OS's own dynamic library, `<name>.dll`,
`lib<name>.so`, or `lib<name>.dylib` (beside_file()), which takes the runtime, std, and its own
dependencies from the executable, as the user chose. The executable links each one's bitcode for the
code it shares, every beside library's own definitions as declarations (read_beside_names(),
declaration_of(), declared_like()) and the shared code it lacks as weak_odr (link_beside()), so each
generic instance and type record is the executable's; once all are linked, each function of theirs
it calls is a thunk through a slot, `fb.(slot).<name>`, and each Global is reached through its
file's `fb.(address).<name>` (reach_beside(), thunk(), reach_through(), rebuilt()), and main's
`fb.(load beside)` calls the runtime's fb_load_beside(), which loads each file beside the
executable, checks its `fb.(build).<name>` record, "name version hash", and on Windows the
executable it was built for, and fills the slots, or stops the program with an error naming the
file. The library's file (emit_beside_object()) declares the shared code, exports its own with
--export-all-symbols, and reaches another beside library's Globals and the executable's ThreadLocals
through `fb.(address)` functions, which the executable makes for its own
(generate_thread_local_addresses()); the executable exports the rest it takes (exports_t,
--export-all-symbols with an import library, -rdynamic, -export_dynamic), and each library links
against the import libraries of those before it (link_beside() in link.cpp), or finds them loaded
first with RTLD_GLOBAL, with -Bsymbolic on Linux. A beside library's static natives are in the
executable, its system libraries link into both, and its dynamic ones go beside it; its file's name
can't be another file's there, case-folded, nor a system library's; a Windows executable with
libraries beside it keeps its ASCII name; and `fb build` prints each file it builds beside the
executable (program_build_t). In a build that links, a shared ThreadLocal's initializer is always
lowered, so every build's maker runs it. Its review's 42 findings were fixed
(docs/review/m7-step3.md): names with ", " that /export directives split, beside libraries using
each other in either order, Struct Globals' Fields and ThreadLocals across the edge, large results
in debug builds, static and system natives, two programs in one build folder, paths outside ASCII
and the ANSI code page, 8.3 names and symlinked executables, names alike but for case, a main file
given in a package with none, the bundled libraries only a library calls, the IR view, a library's
errors at their lines, an executable left without its library, and the docs; a review of those fixes
found 9 more, fixed too: a dependency's natives in each library's file, a beside library the program
doesn't use, one Callback per Function across the edge, links and missing executables on Windows,
and clearer errors.

Step 4, across the edge (03202ac0): what step 3 made, one type per instance in every binary, Globals
reached through accessors, and a program's Class extending a library's, with a test that a generic
instance made on both sides of a beside library is one type, for `Is`, Interfaces, and debug forms.

Step 7, loose ends and the milestone's review (d070a739): the milestone's program,
tests/packages/space, which embeds physics, prebuilt, and has arena beside it, prebuilt too, a
library over the engine package's C++ DLL, and runs from its build folder with only its two files
beside it and the OS's own libraries (the packages test prebuilds any package a project.toml names
as `../<name>-prebuilt`); plugins and loading at run time in Deferred work, as the user left them;
and every docs/review Status line fixed, dropped, or refuted. The milestone's review found 17 more,
fixed too (docs/review/m7-step7.md): a library's interface file records each package it was built
with (`// uses <name> <version> <build>`, package_build_t, compilation_t::builds, record_builds(),
source_build(), interface_uses()), which a program checks against its own, and a prebuilt library's
ABI hash is recomputed as it's read, so another build of a dependency, a mode's files in the other's
folder, or a changed interface file is an error naming the builds; what an interface file keeps is
reached by what each name refers to, not its spelling; a Func's result that's a Task with a Throws
is written in parentheses; a beside library's bad bitcode is named; on Linux and macOS a beside
library of another build that can't be bound says so; a thin arm64e dylib is another CPU's; on
macOS, whose dyld binds a library all at once, a beside library's build record is read from its
file's __DATA,__fb_build section; and the docs.

Step 5, dynamic natives (537d4680): `[native]`'s `{ dynamic = "native/{target}/engine" }`
(native_kind_t::DYNAMIC), a path without the file's "lib" and extension, which finds engine.dll
and its import library (engine.lib, libengine.dll.a, or libengine.a), libengine.so, or
libengine.dylib; `{target}` in static and dynamic paths (with_target(), build_target()), no
other `{...}`, and a target without the file an error naming the package; compiler/src/
dynamic_library.cpp's dynamic_library_name(), which reads a DLL, a shared object's SONAME, or a
dylib's install name (a universal file's slice for the CPU, not arm64e) without loading it, and
tells another CPU's, width's, or byte order's file and an executable from a library; the driver's
read_dynamic(), which records each as a dynamic_native_t under the name the executable records
(the file's on Windows, the SONAME on Linux, the install name's file on macOS, which has to be
@rpath/lib<name>.dylib or in @loader_path), and stops two of one name, case-folded through
utf8proc, and an executable named like one; link_program() linking them, on Linux by
`-l:<name>` from a link folder of its own inside `--push-state,--no-as-needed`, with
`-rpath $ORIGIN`, or `@loader_path` on macOS; and build_program's copy_beside(), with fb build's
"Copied" line. tests/native's dyncalc is the C DLL the tests load. Its review's 12 findings were
fixed (docs/review/m7-step5.md): every dynamic native recorded on Linux, so one only another
uses loads, hints that keep the folder written, an executable or a 32-bit, big-endian, or
arm64e file said to be one, "." and ".." as names, names folded beyond ASCII, -o over a
library's name, and tests of an uncalled library and a stale copy.

Step 6, a package with prebuilt natives (8e239fb4): tests/native/engine, a stand-in engine in
C++ (a world of bodies, std::vector and std::format, exceptions caught at its edge into a
thread-local engine_error()) behind engine.h's C API of fixed-size types, an opaque world, a
struct by value, an enum, constants, and a callback with user data, which CMake builds as
fb_engine, a DLL needing only the OS's libraries (-static -mcrtdll=ucrt on MinGW,
-static-libstdc++ and -static-libgcc and engine.map's exports on Linux); tests/packages/engine,
whose [native] engine is `native/{target}/engine`, with fb-bindgen's bindings in c/engine.fb
(one copy for every target, which the bindgen tests check is what fb-bindgen makes of
engine.h) and world.fb's World Class, Point, Shape, and EngineError over them; and
tests/packages/engine_game, the program, which the programs test and a CLI test build and run
from their build folders, staged with the engine's DLL. Its review's 7 findings were fixed
(docs/review/m7-step6.md): a missing body's shape is an error, bodies bounce as often as they
reach a wall and start inside it, engine_error's rule is the header's, the C++ runtime options
leave out MSVC, and on Linux only the C API is exported.

## M6, the full standard library

Done in M6: step 0, CI: .github/workflows/ci.yml builds Debug and runs ctest on windows-2025
(MSYS2's MinGW GCC, the LLVM installer in C:\LLVM), ubuntu-24.04 (g++-14, apt.llvm.org's LLVM 22),
and macos-15 on arm64 (AppleClang, Homebrew's llvm@22), then tools/verify.py's program checks;
tools/verify.py is the local check too (build, ctest, release outputs but RELEASE_DIFFERS,
concurrent programs ten times each way, clang-format and clang-tidy on what changed since
`--base`). .gitattributes checks text out with LF; fast_float reads floats (Apple's libc++ has no
float from_chars); every file includes what it uses (misc-include-cleaner); is_ascii() is
Windows-only. The AArch64 C ABI: `c_abi_t::AAPCS64` (aarch64 and arm64 triples), aapcs64() and
homogeneous_floats() (an HFA as `c_passing_t::together` pieces, one [N x float] or [N x double];
others of 16 bytes or less as i64 or [2 x i64], a result of 8 or less as an integer its size;
bigger ones INDIRECT or MEMORY), c_layout() listing a fixed array's fields up to 32 bytes, and
codegen's pieces_types() and entry_alloca() of at least 16 bytes; codegen registers X86 and
AArch64 (cmake/llvm.cmake's stand-in Targets.def too), so either's IR is made and tested on any
machine; c_extension() leaves a Bool alone on AArch64 outside Apple's targets, as clang does,
thunks too. driver_tests' packages_t takes off both spellings of its folder (macOS's /var is
/private/var); link_error() reads ld64.lld's "library not found for -l"; native_calls calls sqrt
(exact everywhere) where it called cbrt; bindgen's foreign target is riscv64, and the intrinsics
test reads the host's header. Two tests are Windows-only by nature: the ANSI code page one and
Windows thread handles.
Step 1, the foundation: std/core is loaded but imported like any module (the driver adds no core
import; `module_import_t::statement`), so every std file and test imports what it uses; sema's
`m_unknown_bounds` (unknown_bounds()) quiets what follows from a constraint naming nothing, and
std_import_hint() says which Import. `attribute_t::qualifier` (`@C.Rename`, never a built-in),
sema's attribute_of() and attribute_argument(); next_line_is_body() takes '@', so attributes go
before required Interface members. `modifiers_t::panics_at_caller` (@PanicsAtCaller):
`mir_function_t::panics_at_caller` and `call_t::place`, which lowering's give_place() fills from
`m_span` (scoped_value_t); codegen gives such a function a trailing ptr, location() takes
caller_place() or own_location(), an Interface table's slot is a without_place() thunk
(`m_placeless`), and function values and Callback thunks pass null; check_inherited() and
check_inherited_property() copy it to an Override and refuse adding it; std's Pops, Peeks, Nexts,
Mem, and CString.Length have it. std/core/error.fb: Place, Error's Cause and Place
(`fb_error_fields_t`, first in the object), and the area Errors with their Kind Enums; MIR's
ERROR_THROWN fills a Place (fb_error_thrown()), and fb_error_uncaught() prints it and up to 100
causes. Built-in DebugAssert (DEBUG_BUILD, true in the evaluator), Unreachable, and Todo
(ends_program()); std/core/assert.fb's AssertEqual. `symbol_t::shows` makes a `Printable...`
parameter a String[] whose arguments lowering's texts_of() makes text, with rank()'s fits and
choose_overload()'s `unpacked` tie-breaker; std/core/print.fb's Print, Write, PrintError,
WriteError (fb_write(), which flushes stdout before writing stderr), Flush, Input, and InputAsync;
process.hpp's run_process() takes stdin, which the program tests give from <name>.in (console.fb,
input_async.fb). third_party/ fetches libuv 1.52.1 (uv_a); runtime/src/io.cpp is fb_runtime_io,
the I/O thread (io_t, reader_t for a console, pipe, or file, line_waiter_t), with internal.hpp's
pending_requests(), which stuck() reads, and at_finish(), which fb_finish() runs before the leak
report; build_config's `bundled_library_t`s (prefix, runtime part, archive, notice, system
libraries), the driver's bundled_libraries(), `native_libraries_t::bundled`, and the CLI's
"Bundled" lines. `symbol_t::gives_task`: an Extern "runtime" Function that gives a Task<T> is
passed a new Task first (extern_call()); a top-level Await is TASK_WAIT and a rethrow
(lower_await() without `m_async_task`). Its review's fixes: a DebugAssert's condition lets go of
what it made before a release build's way past joins (lower_panic()); Await and Wait settle()
their Task first; the I/O thread's finish_request() signals once the count is down
(start_request(), requests_pending()); the scheduler's tick() panics off the main thread;
showable() refuses runs_elsewhere() types; an Override Property and an Abstract one's
required_accessor() keep @PanicsAtCaller; unknown_bounds() quiets For ... In, static_members(),
and unsendable(), and an INVALID Throw quiets its Catch; an Attribute can take a built-in's name;
attribute_of() says a type isn't a module; std's CString.Free panics at the caller over
fb_cstring_free_known(); libuv's notices are one generated file (third_party's notices/libuv.txt)
under `FB_LIBUV_LICENSES`; reader_t keeps stdin's flags on POSIX (a terminal's copy, a pipe's
F_GETFL); run_process() writes the input from a thread of its own; an uncaught error shows 100
Causes and says there are more.
Step 2, indexing, iteration, and ranges: `Operator []` and `Operator []=` are Methods of Self
(parse_operator_name()'s "[]" and "[]=", sema's indexes() and indexing(), check_index_operator(); a
Struct's or Interface's []= is ByRef); the parser makes a hidden call beside each index
(`index_expr_t::call`, "object.[](indices)") and each assignment, op=, ++, or -- to one
(`index_set`, "object.[]=(indices, value)", the value "object[indices] op value"), which sema
checks only for a type that has_index_operator() (check_index_call(), check_index_set(), with
`m_checked` so a shared part is checked once, takes_indices(), a []= that takes the indices as []
does) and lowering lowers (`m_reused`, settled, in lower_index_set()); check_receiver() refuses a
ByRef Method on what a Property gives and lets an object receiver through. Interface defaults
outside the table: generic ones, conditional ones (Where lines on the Interface's own type
parameters, implemented_bounds() in take_default(), and a type that can't meet one doesn't get
it), and Final ones (newly allowed) run as written, through a value as a direct call with the
Interface for Self (sema's and lowering's outside_table(), copies_outside_table() out of slotted()
and is_virtual()); a type's own Method can't take their place (implementation_of()), a Class
can't override the copy (check_inherited()), and a required generic Method makes an Interface only
a constraint; type_arguments_follow() takes Self. Iterable<T>: boxes_into() lets a Method giving a
Struct or Class that implements Iterator<T> implement one giving Iterator<T>; a table gets a thunk
("X.Iterate() as Iterator<T>", table_thunk(), lower_table_thunk()) and generic code boxes at the call
(boxed(), std's Package `Boxed<S, T>`, `analysis_t::boxed_iterator`); std's collections implement
it. std/core/iterator.fb has Iterator<T>'s Final adapters and consumers over private Final
Classes (Mapped, Filtered, Taken, ... Ordered over the Package MergeSort, moved from array.fb),
Repeated, and Empty; Numeric has One (lowering's builtin_method()); @PanicsAtCaller goes on a
Final, generic, or conditional default. Ranges: `range_expr_t` from parse_range() at
`precedence::range`, between ? : and ?? (`m_counting` keeps a For loop's bounds and a Case's
values as they were), sema's check_expr(range_expr_t) (`m_range`, T by the For loop's rule,
integers only), and lowering's lower_expr(range_expr_t), which fills std/core/range.fb's
Range<T> (Current, Step, Left, More), its count of steps worked out unsigned with wrapping
arithmetic, a run-time direction picked by a branch, and an empty range its zero.
Its review's fixes: a table thunk (`table_thunk_t`) passes ByRef parameters on, gives a Method an
error variable where it takes one and the Interface's member doesn't ("X without an error
variable") or passes its own where the member throws, which a covariant Iterate may now; an
overridden Method takes an error variable where an override does, and a static call decides by
the callee; a @PanicsAtCaller call inside another passes that one's caller's place as it is,
null too (codegen), and std's consumers and every box's and adapter's Next are @PanicsAtCaller;
check_implementation() compares bounds through as_implemented(); implementation_of() decides a
default the type can't have before its place checks, and takes_instance_of() refuses a Method
taking a generic default's parameters with some types for its own, there and in check_inherited()
(report_outside_override()); check_refinements() refuses an Interface redeclaring an
outside-table default, and check_instance_defaults() an Extend of one instance doing it;
check_expansion() follows each default's Interface parameters apart (`expansion_node_t`,
runs_as_default()), calls of a type's copy (runs_copy()), and the defaults a Class's tables run
when it's made, so a default wrapping Self without end is an error, not a hang
(check_table_defaults() went); implemented_bounds() keeps a bound on a type made of the type's
parameters, like Wrap<T>, which argument_for() and mentions() check per instance; set_field_type()
reaches Classes extending an instance; outer_functions() lets a call by name in a type's code mean
a top-level Function where none of its Interfaces' defaults of the name fits; report_uninferred()
names only what told_arguments() can't tell, or a bound's failure; the parser's begin_of()
(`m_grouped`) starts an expression made with a parenthesized one at its parenthesis; Step takes a
whole expression; Range has an Operator ==; a Const Function's For ... In is refused at Next.
Its second review's: lower_function() decides a copy's error slot by the copy (lower_body()'s
`copy`), and give_error_variable() passes one in to_string(), hash_of(), and reflection's Call;
hide_in_place() lets a Method of a conditional default's types hide it where the instance doesn't
have it, which then hides nothing, check_instance_defaults() skips what !instance_has(), and
implementation_of() decides a default the type can't have before its matches; takes_instance_of()
checks the generic default's Where lines on its own type parameters; drop_clashing_extension()
refuses an unseen Extend's Method in a generic default's place, covers_outside_default() an
extending Interface's, and check_default_places() another Interface's default (any listing order);
implemented_bounds() keeps a bound whose Interface has the Method's own type parameters, which
told_type() lets told_arguments() infer from (Flatten on Iterator<Stack<Int>>); check_expansion()
reads each call's full type arguments (`m_call_arguments`) and gives runs_copy() the copy owner's
instance for Self; lowering stops at an instance whose type nests past too_deep() and sets
`mir_module_t::error` (`mir_error_t`), which codegen's build() and the CLI report; lower_place()
lowers a member without a symbol, like `s.Bytes[0]`, as a value; the parser's begin_of() starts
binary, ternary, and tuple-element spans too; Range's == is in a conditional Extend, and Range<T>
is only Where T: Numeric; report_uninferred() writes every type argument in its advice and says
nothing more after a count error.
Its third and fourth reviews': implements_for_some() makes a Where line on a default's own type
parameters hold when some types for them make it, for bounds_hold(), instance_has(), never_has(),
and implemented_bounds() (a type that implements no instance of the bound's Interface, like an
Iterator<Int32> for Flatten, gets no copy, and missing_default_hint() says why at "has no member
named"); told_type() counts a parameter told as itself as told; report_uninferred() reads a bound
on a type made of the parameters, and report_bounds() leaves out "for" on a copy's bound;
check_default_places() compares a type's own copies with the outside defaults its bases (through
never_has()) and its generic type have, and runs over Extends of one instance; check_changeable()
refuses changing an array's Count, alone in an Extend of T[] too, or a String's Chars or Bytes
(report_worked_out()), by assignment, op=, ByRef, or AddressOf, so an Extend's Property on arrays
can be set; a Map literal's Adds get an error variable; the type table refuses an instance whose
argument is made of more than max_size types (measure()) and fills one the depth limit cut short
again where it's asked for itself (`m_incomplete`), value_expression() can't overflow, and a fixed
array keeps a size past max_count unresolved; lowering stops growth along a chain of instances
made one from another (`origin_t`, chain_length() past max_chain), at a type too_large() (past
max_depth, 1000, or max_size), saying which, or a Method whose owner's instance lost its
arguments, and check_arguments() and check_values() report a Const type argument an instance
worked out outside 1 to max_count or its parameter's type (fits()); a String's Extend Property
reads as a Property; unify() is const. The lows: hide_in_place() lets an Extend's Method that a
call tells apart by ByRef stand beside a default, check_default_places() drops the copy it
reports, drop_clashing_extension() gives a generic default's clash its own message,
implemented_bounds() gives a tuple's or Func's bound no copy when no type fits (unextendable()),
report_uninferred() and implements_for_some() stay quiet after a constraint naming nothing,
run_const() reports a module lowering stopped, a Const Function runs for a Const type argument's
value, and a Const its own error left without a value is quiet where it's used
(`symbol_t::failed`, `m_read_failed_const`).
Step 3, collections I: std/core/values.fb's Min, Max, Clamp, Swap, and HashCombine;
interfaces.fb's Cloneable and Defaultable (`analysis_t::cloneable`, `defaultable`), which
builtin_implements() gives arrays (Clone) and the numbers, Bool, String, and Enums (their zero,
lowering's builtin_method()); a T?'s built-in Expect and OrElse (nullable_methods(),
`m_nullable_methods`, lowering's nullable_call()), on an array's T? and on a variable narrowed to
its T too; an array's Capacity (is_array_size(), `expr_info_t::count` for Count or Capacity alone
in an Extend of T[]), Reserve, and TrimExcess (MIR ARRAY_CAPACITY, ARRAY_RESERVE, ARRAY_TRIM over
the runtime's fb_array_reserve() and fb_array_trim()); TypeName<T>() (check_type_name(),
is_value_builtin(), `expr_info_t::type_name` for generic code), the declared name without its
module. Arrays go where Printable or an Interface an Extend of T[] or of the one array type gives is
expected: lowering's use_array_tables() fills `mir_module_t::array_classes` (array_interfaces(),
array_table_entry(), implementation() mapping a Property's Get and Set), which codegen's array
classes list; unify() takes an Interface's type arguments from the instance a Class, Struct, array,
or extending Interface implements (instance_for()); an array literal where an Iterable<T> of a known
T is expected is a T[]; converts_plainly() passes INVALID. std.collections: array.fb's search, ends,
bulk (AddAll and InsertAll from any Iterable<T>), order (Sort, SortUnstable's heap sort over
SiftDown and Swap, SortBy/SortByDescending's Sorting<T> with ThenBy), shape, Fold, Reduce, View,
and AsReadOnly, @PanicsAtCaller where they panic, going over what's left when a function changes
the array; map.fb in insertion order (each entry's Before and After), `==` and Hash by contents,
Operator [] and []=, `Map(default: v)` (Fallback, one shared value), Get(key, fallback), GetOrAdd,
TryAdd, Update (a removed key stays removed), Reserve; set.fb's algebra; the Try twins, ToArray,
AddAll, Drain, and Deque's index; Pool's IterateHandles and Capacity; view.fb's ArrayView (Place()
names the view's index) and ReadOnlyArray, ReadOnlyMap, and ReadOnlySet. Its review's fixes are in
those: the T? Methods on arrays and narrowed variables, Extends of one array type and Extend
Properties through tables, a T?? of an Interface, Try before TypeName or SizeOf, Capacity refused
ByRef and set, literals for Iterable<T>, inference through an extending Interface, views' and
sorts' panics, RemoveAll, FindLast, and Reduce under a changing array, Update, and PriorityQueue's
AddAll of nothing.
Step 4, collections II, std only: std/collections/sorted.fb's SortedMap and SortedSet (a B-tree,
SortedNode, Order 16, Find(), Split(), Fill(), Merge(), SortedMapIterator's stack of nodes),
ring_buffer.fb, grid.fb's Grid and Grid3 (GridIterator), bit_set.fb (UInt64 words, BitCount()),
lru_cache.fb and counter.fb over a Map, multi_map.fb (a Map of arrays), trie.fb (TrieNode by
byte), linked_list.fb (LinkedListNode with a Weak Before and Owner, LetGo() unlinking each
node), interval_tree.fb (an AVL tree, IntervalNode's Top), spatial.fb (PlacedValue, Bounds in
Doubles, Around(), SpatialHash's cells, and SpaceTree, which QuadTree and Octree wrap,
SpatialIterator), and immutable.fb
(ImmutableArray, ImmutableMap, and ImmutableSet, Structs over AVL trees of Shared ArrayNode,
HashNode, and HashEntry); each is checked against a plain array or Map over random operations
(tests/programs/<name>.fb), with a panic program for each panic. The runtime's let_go() lets
go of what an object's fields held one object at a time (`letting_go_t`, release_fields()), as
tests/programs/long_chains.fb shows with a million of each kind of object, and lowering's store()
copies a value read through a reference, or a Global's, before it releases the old one, as in
`Root = Root.Children[0]` (assign_through.fb).

Phase 1, lane 2's first batch (A5 to A7's highs, and A5-5): `++`/`--` finish their statement; For
... In holds a fixed array in a local of its own unless it's a whole temporary; a Select's type
match goes through concrete(); lower_jump() finds its loop by position again after release_scopes();
through_reference() counts a ByRef parameter's or ByRef Self's root, and store() copies from one;
evaluate's unary folds a @Flags Enum's integer and keeps the Enum; `leaving_t`, `m_leaving`, and
left(), so the cancel block of an Await in a Defer or Finally on a way out releases only what that
way out still holds; lower_try() sends an Await through lower_effect(); reflected_methods() leaves
out Async Methods and Methods with a ByRef parameter (spec 13); generate_main() releases the Globals
last to first through let_go(), which zeroes each reference before releasing it, and the ready flag
is an Int32 (0, `ready`, `released`) whose check panics "used after it's released" (spec 02). Tests:
increment_temporaries, for_in_fixed, scheduler_cancel_cleanups, globals_released,
panic_global_released, and cases in generic_bases, errors, borrowed, compile_time, async_throws, and
reflection_more.

Phase 1, lane 4's first batch, the review's A11 and A12 mediums and four lows: a Queue's Items are a
`T?[]` and Dequeue empties the slot it takes, so a dequeued value goes when the program lets go of
it, and its compaction compares Head with Count - Head (HalfEmptied()); T[]'s SiftDown and
PriorityQueue's find a child through HeapChild(), which stops at count / 2, so no index overflows
past 2^30 elements; a Map's Grow stops at 2^30 buckets (BucketsFor()) and returns when that leaves
them as they are; a RingBuffer's index wraps through SlotFrom(); tests/compiler/std_tests.cpp runs
those Package helpers through an extra std file with counts up to 2^31 - 1; spatial.fb's Within
takes Doubles, so each offset is a Double's, and takes every value for an infinite radius, one at
the center's infinity too; sorted.fb's CheckKey panics at a key that isn't equal to itself, like
NaN, in SortedMap.Add and []= and SortedSet.Add, IntervalTree.Add does at such an end, and Floor,
Ceiling, Range, Overlapping, Containing, Contains, and Remove find nothing for one; sema's
unmet_extension() lets report_unshowable() name the type argument that keeps an Extend's Printable
from a value; spec 14 gives FindLast a `T?`, says what compares values needs them Equatable, and
fixes LinkedList's sentence, spec 10 lists what's Sendable, and stdlib.md and interfaces.fb's
comments match. New tests: queue_releases, panic_sort_unstable_big, equatable_objects, spatial's
Edge cases, panic_sorted_map_nan, panic_sorted_set_nan, panic_interval_nan, std_tests' overflow
checks, and panics for a Map, Queue, PriorityQueue, SpatialHash, and Octree changed in a loop, every
spatial NaN check, and an empty LinkedList's Last.

Phase 1, lane 4's lows: Counter.Add works a count out with CheckedAdd and panics past what an Int
holds, in every build, and its comments and spec 14 say a value whose count went to 0 comes back
last; ImmutableMap.AddAll and ImmutableSet.AddAll, as ImmutableArray's; every collection's Changes
comment says it counts values added or removed (a Deque's setter in a loop is tested); comments
fixed in map.fb (Buckets and Shift), values.fb (Clamp's panic), trie.fb, and panic_chain_changed.fb.
New test: panic_counter_overflow. Lane 4's 23 findings are all fixed.

Phase 1, lane 2's second batch (A5 to A7's mediums and lows, and A7-new-1): raise() keeps the error
on its way out in `m_raising`, which an Await's cancel block releases; Lock reaches its Mutex once
and holds the innermost object it's in for the block (not Self's); combine() steps a Ptr with
pointer_t; set_property() settles the receiver for op= and ++; evaluate() takes a `build_mode_t`,
which analyze() (sema.hpp) gets from the driver, so a release build's Const Function leaves
DebugAssert out (spec 13); `call_t::span`, runtime calls' spans filled in by give_place(), and the
evaluator's `m_at` and reach(), so the step, depth, and size limits point at the last call or
operation reached (spec 13); the runtime's reserve_one() stops the capacity at 2147483647 and panics
past it (spec 14); codegen's constants are `fb.string`, `fb.text`, `fb.location`, and `fb.classes`;
declare_extern() extends a runtime Extern's small integers as C does; declare_global(), and a Const
known at compile time gets its global only when place_of() needs it; a slice's panic shows its
bounds as written; the MIR dump escapes \r, \0, control characters, and braces, prints a T? of an
Enum by member, marks `async` functions, and paints a Callback's function; a map literal runs Map's
New(); comments fixed in lower.cpp, mir.hpp, mir.cpp, codegen.cpp, and c_abi.hpp. Tests:
tests/compiler/evaluate_tests.cpp, release_const_debug_assert, release_panic_array_full,
panic_slice_unsigned, a runtime test, mir_tests and c_abi_tests cases, and cases in
scheduler_cancel_cleanups, atomic_mutex, pointers_more, and property_kinds.

Phase 1, lane 1's first batch (sema), the review's A3 findings: a Class's own Fields without a zero
are assigned before Super.New (Swift's rule, A3-1): super_new_of() takes the first of New's own
statements that's a Super.New, check_class_call() reports an own field unassigned there and a
Super.New that's nested or repeated, check_body() reports a New that never calls it while the Class
has such fields, and check_inherited_ready(), `Self.f`, and Return keep New's code before Super.New
to its own fields, which spec 06 says (the Boss example); a lambda or Parallel For body in New isn't
New's own code (in_new_of()), so it can't change ReadOnly Fields (A3-2); lambda_context_t's
`unready` keeps a lambda made before New's fields are assigned from capturing Self (A3-3); assigning
a Property by name, or as Self.P, checks check_self_ready() (A3-4); lower_reflection() refuses f.Set
on a ReadOnly Field, as spec 13 says (A3-5); a fixed array's count is checked for overflow before it
multiplies (A3-6); only a Static member hides T.Has and T.Attribute (A3-7); For Method skips Async
Methods (A3-8, with lane 2's A6-5); For ... In over a nullable Channel gets the right error (A3-9);
m_finally_returns and finish_returns() let a Finally assign what New's Returns in its Try need
(A3-10); For over literals alone takes Float32 when one has a decimal (A3-11); a throwing call in a
Defer or Finally says it needs Try? or a Try block inside it, through the SEALED handler (A3-12).
New tests: tests/programs/fields_before_super and reflect_skips.

Phase 1, lane 3's findings (A8, A9, A10): let_go() keeps a Weak to an object waiting its turn
(weak_waiting.fb). The parser's enter() counts each level once, exactly 1000 passing: blocks,
operators, calls, members, indices, brackets and interpolations, lambdas, each part of a type
(parse_type's suffixes, type arguments, Weak, Func, tuples, Const sums), Defer's and a single-line
If's statement, an Extern block, and nested_statement() for what a reported mistake leads to; the
lexer's #If condition has max_condition_depth. A single-line If refuses a labeled loop, an Unsafe
block, and an Extern block. skip_continuation_newlines() follows every statement's ',', '=', and
constraint '&', and stops on a line that already failed. An inactive #If branch is lexed for its
directives (m_skipped_from drops its tokens, report() sends its errors to m_unshown, but invalid
UTF-8 and what runs past the branch), finish_directive() skips trivia, and skip_inactive_lines() is
gone (multi_line_strings.fb). A multi-line string in a single-line string's interpolation is an
error. The parser takes '>=' as '>' then '=' (m_split), m_accessors closes End Get and End Set,
describe_current() reads the very next token, New in an expression says to call the type, and an #If
condition cut off at the end of the file stays inside it. fb::escape_controls() escapes messages,
paths, and notes, and dump.cpp's escape_token() and quote() escape every control character, quote()
doubling braces. key_of() keys a file by real_path(), and on_disk() falls back to the entry the disk
finds that isn't a link; a folder Import of links is one error; a [native] folder path or one ending
in '/' says it's a folder; a module's file ends in .fb (respelled_file()); project.toml takes '\' as
'/' (with_slashes()). fb build words a default output that's a folder apart from -o; parse_options()
gives a usage_error_t with its --color; fb-bindgen refuses an -o that's the header, empty, ends in
'/', or is a folder, and _Static_asserts each integer macro, `(NAME) * 0 + 1`, with
-Werror=gnu-folding-constant. scheduler_t::stuck() counts the I/O thread's requested_tasks() as
coming. at_end() and run_end() put back stdin's flags (POSIX) and the console's code page and
stderr's mode (Windows) however a program ends, and prepare_terminal() restores the console at exit
(tests/native/console_probe). run_attached() waits out Ctrl+C (ignore_interrupt() on Windows, SIGINT
and SIGQUIT ignored on POSIX), and spawn() starts programs with SIGINT, SIGQUIT, and SIGPIPE at
their defaults. A thread of its own pushes its id before m_pending goes down; shown_class_name()
names an uncaught error's chain (uncaught_alike.fb); resize_buffer() says Reserve's place and count
(panic_reserve_huge.fb); leak counts take the singular. cli_tests reads a program's PE, ELF, or
Mach-O imports and checks they're all the system's own. Comments: ast.hpp, manifest.hpp, module.hpp,
manifest.cpp, process.hpp, commands.cpp, and take().

Step 9, Math I: std/math/. numbers.fb: `Bounded` (MinValue, MaxValue), `Integer` (BitWidth, LowBits,
FromLowBits), and `FloatingPoint` (Epsilon, FromDouble, ToDouble, and libm's Floor through Fma),
Interfaces the ten number types implement through Extends with Private Static Methods over `Extern
"C"` libm, so std.math's Functions are generic and take their type from their arguments (`Lerp(0,
10, t)`); `MinValue<T>`, `MaxValue<T>`; Float32's and Float64's `ToBits()` and `FromBits()`, by
Unsafe Ptr casts. constants.fb: Pi, Tau, E, Epsilon, Pi64, Tau64, E64, Epsilon64, and Inf and NaN,
Consts worked out at run time since the compiler refuses a non-finite one. scalar.fb: Abs and Sign
(Numeric & Comparable), Floor, Ceil, Round (half away from 0), Trunc, Fract, Mod (floored) and Rem
for integers (IntegerRem(), safe at -1) and floats (fmod), Gcd, Lcm, IntSqrt (digit by digit in a
UInt64), Lerp, InverseLerp, Remap, SmoothStep, Wrap, PingPong, MoveTowards, IsClose (a tolerance, or
0.00001 of the larger), Sqrt through Atanh, Log(x, base), Atan2, IsNaN, IsFinite, CopySign, Fma,
Deg, Rad, ToRadians (with Int overloads), and ToDegrees; no Pow or IntPow, since `**` is the power.
bits.fb (std.math.bits): PopCount (SWAR), LeadingZeros, TrailingZeros, RotateLeft, RotateRight,
ByteSwap, IsPowerOfTwo, NextPowerOfTwo, AlignDown, and AlignUp, generic over Integer through
LowBits, and each integer's SaturatingAdd, SaturatingSubtract, and SaturatingMultiply over its
Checked Methods (Saturated()). vector.fb and int_vector.fb: Vec2, Vec3, Vec4, Vec2i, and Vec3i,
Numeric, Hashable, Printable, and Defaultable, with Operators by component and by a scalar on either
side, `[]`, Vec3's Up through Back and xy, Vec4's xy and xyz, and ToVec2i, ToVec3i, ToVec2, and
ToVec3; Dot, Cross, Length, LengthSquared, Distance, DistanceSquared (lengths in Doubles,
Magnitude()), Normalize (zero stays zero), Reflect, Project, Angle, Rotate, Perpendicular,
ManhattanDistance, ChebyshevDistance, and the component-wise Lerp, Min, Max, Clamp, Abs, Floor,
Ceil, Round, MoveTowards, and IsClose. quat.fb: Quat, Identity, FromAxisAngle, FromEuler (Y, X, Z),
FromMat3 (DirectXMath's four cases), FromTo, LookRotation, ToMat3, ToMat4, ToAxisAngle, ToEuler,
ToEulerDegrees, `q * v`, `a * b`, Slerp, Lerp (normalized), Inverse (the conjugate), Normalize, Dot,
Length, Angle (the relative rotation's Atan2), and IsClose (q and -q alike). matrix.fb: Mat3 and
Mat4, their columns the Fields x, y, z, and w, Identity, Scale, Rotation, RotationX, RotationY,
RotationZ, Translation, Compose, LookAt, Perspective, and Orthographic (glm's RH_ZO),
TransformPoint, TransformVector, ProjectPoint, Decompose, ToMat3, ToMat4, `[column, row]`,
Transpose, Determinant, TryInverse (glm's cofactors, four at a time), Inverse (panics), and IsClose.
color.fb: Color (linear Floats) and Color32 (sRGB bytes), the named colors, Hex and TryHex (#rgb to
#rrggbbaa), ToHex, Srgb, Hsv, Hsl, OkLab, ToHsv, ToHsl, ToOkLab, ToColor, and ToColor32 (sRGB's
curve in Doubles; ByteOf() keeps 0 to 255, NaN as 0), Operators, Lerp, and IsClose. geometry.fb:
Rect (X, Y, Width, Height), AABB (Min, Max), Sphere, Ray, Plane (Normal, Distance), and Frustum (six
Planes from a matrix, Gribb and Hartmann for 0..1 depth), with Contains, Intersects, Raycast (slabs;
0 from inside), Intersection, Union, Grow, ClosestPoint, Transform (Arvo's), SignedDistance,
FromPoints, and FromPointNormal. sema's is_later_name() lost Vec2 through Color and Rect, so an
Extend of one names std.math's Import. Tests: math_scalars, math_bits, math_vectors, math_quat,
math_matrices, math_color, and math_geometry, printing libm's results rounded to four places, and 36
panic_math_ programs.

Step 9's review (25 findings, 2 high): Sphere.Raycast in Doubles from the ray's nearest point;
integer Wrap and PingPong through UInt64 distances, for unsigned types and wide ranges; float Mod
and Wrap never give b or high; Lerp exact at 1; PlaneOf's zero-normal rows; Grow stops at nothing;
ToEuler's fold within 0.02 degrees; Vec2i, Vec3i, Mod, Rem, Gcd, Lcm, and Abs @PanicsAtCaller;
Sphere tests, Project, Angle, MoveTowards, and Quat's Length in Doubles; matrix inverse and
determinant in Doubles; FromTo without its shortcut; Green as CSS's, with Lime; colors' linear
values rounded once; Mat3 and Mat4 scale from the left and divide; ToDegrees(Int); stdlib.md's SIMD
claim dropped, the compiler's item 13; spec 02 and 06's math types.

Step 21, std.crypto: third_party/ fetches mbedTLS 4.1.1 (TF-PSA-Crypto's PSA API, with
`MBEDTLS_THREADING_C`), xxHash 0.8.4, BLAKE3 1.8.7, Argon2 20190702, crypt_blowfish 1.3, and
Monocypher 4.0.3 (Ed25519 only), each built -O2 as its own archive beside a runtime archive
(fb_runtime_crypto, with crypto_pem.cpp apart, fb_runtime_xxhash, fb_runtime_blake3,
fb_runtime_argon2, fb_runtime_bcrypt, fb_runtime_ed25519; fb/crypto.hpp), and
`bundled_library_t::archives` takes several. std/crypto: `Hasher`, `Md5`, `Sha1`, `Sha256`,
`Sha512`, `Blake3` (keyed, derived, any length); `Hmac`, `Hkdf`, `Pbkdf2`; `SecureRandom` (`Bytes`,
`Below`), `ConstantTimeEquals`; `Crc32`, `XxHash`; `Argon2id` (`Hash`, `Verify`, `Derive`) and
`Bcrypt`; `Aes256Gcm` and `ChaCha20Poly1305` (`NewKey`, `Seal`, `Open`); `Ed25519` (RFC 8032's
encodings only), `EcdsaP256`, `Rsa` (`Verify`, `VerifyPss`, `PublicKey`), `X25519`; `KeyKind`, `Key`
(`FromPem`, `ToPem`), and `Certificate` (`FromPem`, `AllFromPem`). Tests from RFC 1321, 2202, 4231,
5869, 6070, 6234, 6979, 7515, 7748, 7914, 8032, 8410, 8439, and 9106, BLAKE3's and xxHash's vectors,
crypt_blowfish's, and Wycheproof's.

Step 21's review (13 findings): crypto_internal.hpp's `stored_key_t` (glibc has a key_t); Hmac
hashes a key longer than its block first, as RFC 2104 does; third_party/ gives mbedTLS's libraries
-O2 after their own -O0 in debug builds (fb_options_after), which driver_tests checks through
compile_commands.json; crypto_pem.cpp reads PKCS #8, SubjectPublicKeyInfo, and certificates with a
DER reader of its own, so a certificate of any key reads, names come out as OpenSSL writes them
(T61, BMP, and Universal strings decoded, RFC 4514's escapes, no length limit), Key.FromPem takes
the first key block and throws SYNTAX for a damaged one, X25519 keys stay unclamped, and mbedx509
isn't linked; PSA_VENDOR_RSA_MAX_KEY_BITS is raised to 8192 and RSA verifies through mbedTLS's RSA
module, any size; Bcrypt.Verify refuses $2x$; crypto_macs checks RFC 7914's PBKDF2-SHA256 vectors
and HKDF and PBKDF2 by SHA-512, crypto_signatures RFC 7748's 5.2, and crypto_certificates and
crypto_rsa_keys are new.

Step 27, std.algo: std/algo/. search.fb: `Bfs`, `BfsOrder`, `Dfs`, `DfsOrder` (an explicit stack of
`DepthStep`s), `Dijkstra`, `DijkstraAll`, and `AStar` over successors Functions and Hashable nodes,
the weights of any Numeric and Comparable type, through one `Search<N, C>` (a PriorityQueue of
`Waiting<C>`, least total, then guess, then order; stale entries skipped, so A* reopens), `Path<N,
C>` (Nodes, Cost), and `ShortestPaths<N, C>` (Nodes, Contains, CostTo, PathTo, Parent); a negative
or NaN weight panics. grid.fb: `GridNeighbors`, `GridPath` (A*, octile or Manhattan, diagonals of
the square root of 2 that don't cut corners, Moves()), and `FlowField` (DijkstraAll from one start
before every goal, each step the cost of entering the cell nearer the goals; Distance, Next,
Direction, Reaches). union_find.fb: `UnionFind` (path halving, union by size, Add, Groups,
SetCount). topological.fb: `TopologicalSort` (Kahn's, earliest-given ready first, Gather()) and
`FindCycle` (depth first, by states). combinatorics.fb: `Permutations` and `Combinations`
(`PermutationIterator<T>` and `CombinationIterator<T>`, itertools' order), `Factorial`, and
`Binomial` (by gcds, so only a too-large answer overflows). delaunay.fb: `Delaunay` (Bowyer-Watson,
a `Mesh` with a point at infinity, Locate() walking, Insert() by cavity, Orient and InCircle in
Doubles), `Voronoi` (KeepNearer(), Sutherland-Hodgman by each Delaunay neighbor's bisector), and
`LloydRelax` (Centroid()). Tests: algo_search, algo_grid, algo_union_find, algo_order,
algo_combinatorics, and algo_delaunay, against CLRS's graph, a Python Dijkstra, and exact rational
Delaunay and Voronoi references, and 29 panic_algo_ programs.

Step 27's review (14 findings, 1 high): Delaunay's Orient and InCircle are Shewchuk's, filtered and
then exact in expansions, so near points give a true triangulation; A* opens a settled node again;
Voronoi and LloydRelax check bounds; Permutations and Combinations of more than there are keep
nothing; a NaN guess panics; the searches' weights needn't be Printable; GridNeighbors checks its
cell; a Voronoi cell is cut by each neighbor once, has no corner twice, and is empty where it only
touches bounds; points on a line are cut by their neighbors only; TopologicalSort's docs and
InfiniteAt's comment say what the code does.

Phase 1, lane 1's second batch: reaches_class() catches an inheritance cycle through an instance of
the Class being declared, like A Extends B<Int32> with B<T> Extends A (A1-1); Destroy can't be
generic or have a Where line (A1-2, spec 06); common_type() wraps the inner common type with
types.nullable(), so T?s nested to different depths meet at the deeper one (A2-1);
m_pending_sendables checks a Mutex's or Channel's T again, through check_carried(), once types are
declared, with the constraints where it was written (A2-2); check_threads() leads an Interface's
Property to the Get and Set of what implements it, with getter_of() beside setter_of() (A2-3);
note_shown() and note_compared() record the ToStrings and Operator ==s that showing a value and
comparing values run, for the main-thread rules (A2-4); comparable() keeps a seen-list, so == on a
Struct that holds an array of itself doesn't overflow the compiler's stack, and its comment says
what it refuses (A-new-2, A2-7); `a?.M()` on an Async Method gives a Task<T>? (a Task? for one that
gives nothing), which lowering's Async call makes from its T?'s T, and lookup_member() says a
nullable Task, Channel, Thread, or Scheduler may be Null (A4-1, spec 10); implements_alone() is the
one rule for the built-in Interfaces that sema's builtin_implements() and fb::implements() share,
Defaultable and Cloneable among them (A4-2); a Const Function call that was reported while it was
checked doesn't run (A4-3); an Async lambda isn't work for Spawn or Thread.Start (A4-4, spec 10);
list_object_interfaces() lists Equatable and Hashable for every Class once every Extend is declared,
leaving Hashable off one with an Operator == of its own (written, inherited, an Interface's default,
or an Extend's) and no Hash, with gets_default(), extended_with(), and has_hash(), and
unhashed_reason() saying why in the error for a Set or Map key (A-new-1, specs 06, 07, and 14).
Tests: sema_tests cases beside each, nested_optionals, async_methods, extend_more, equatable_objects
(no Implements Equatable any more), and conditional_defaults and several sema tests that now use a
Struct as their type that isn't Hashable.

Lane 3 fixed ten of lane 1's sema findings. A Scheduler's Run without its Task is one error, not a
crash (A4-5). fold_integer_call() doesn't fold a known argument of another type (A4-6), and pointer
arithmetic on a known Null Ptr isn't folded (A4-7). conformance_query_t::sendable() has a WEAK case,
so a Weak<T> of a Shared Class is Sendable for lowering's Interface tables too, as sema and now spec
10 say (A4-8). choose_overload() prefers the one Function that isn't generic among convertible
candidates too, not only exact ones (A4-9). common_type_of() takes typed values first, then
literals, tuples, and empty arrays and Maps with the common type as their hint, then Null, so
`[Null, 1]` and `[i, []]` work wherever the Null or the empty one is (A4-10). contains() sees a
generic Struct holding an instance of itself inline, like a Wrap<T> with a Wrap<Int> Field (A2-5). A
generic Function's name passed to an overloaded call or a New takes its type arguments from each
candidate's parameter: choose_overload() lets names_generic_function() through, and match() checks
it against each overload's parameter (A4-new-1, from lane 5); the parser's type_arguments_follow()
also takes a `>` before `]`, `}`, a line's end, or the file's end, so `f = Less<Int>` parses.
check_defaults() runs once per Function, on demand from run_at_compile_time(), so a Const Function
run where Consts and types are declared has its defaults (A4-11). check_enum_value() declares an
Enum early while types are declared, so `Int[Extra.BONUS]` works above the Enum (A4-12).

Phase 1, lane 1's third batch, the A1 mediums and lows: an override of an instance's Property
compares with member_type(), as a Method's does (A1-3); a ByRef default an array type gets isn't
ByRef (A1-11); body_t swaps m_locks, so a Const checked early isn't in the Lock blocks of the code
that needed it (A1-12); report_baseless_overrides() refuses Override in a Class that extends
nothing, and check_inherited_static() keeps a Static Field or Property from taking an inherited
member's name and a Static Method an inherited instance Method's name and parameters (A1-7, A1-8);
@MainThread before a Property, Interface, or Enum is an error (A1-6); Static and ReadOnly before a
Struct or Class are errors, and ReadOnly on a Const says it's a Const already (A1-14, A1-16);
extension_target()'s own_only list says Atomic, Mutex, Ptr, Task, Thread, Channel, Scheduler, and
Weak<T> have only their own operations (A1-15); conform() skips the built-in Interfaces a built-in
type, Enum, or array comes with, so an Extend can list one that extends them, like `Real Extends
Numeric` for Float32 (A1-9); names_type() keeps a type parameter, and Extend T[]'s T, from taking
the name of a type the code sees (A1-13, spec 06); check_instance_redeclared() keeps an Extend of
one instance from redeclaring a Field, Property, or Method its generic type has, with the instance's
types, though not an Operator, which beats the generic type's (A1-4); check_default_own() checks a
default a type takes against its own Fields, Static Fields, Properties, and Static Methods, and
check_default_inherited() against an inherited Static Method (A1-5); hide_in_place() lets a default
every instance has from an Extend without Where lines take the place of a refining Interface's
conditional default (A1-10); comments on extension_member_fits(), m_declared_types, m_const_modules,
symbol_t::members, and conformance_t::type say what's there (A1-17, A1-18); the CLI's Ctrl+C test
counts only the interrupted-* folders its own run leaves in the temp folder every run shares
(A-new-3, in tests/cli/cli_tests.cpp, lane 3's file). Tests: sema_tests cases beside each,
extend_refined_builtins, and a case in conditional_defaults.

Lane 3 fixed the rest of its share of lane 1's sema findings. told_arguments() unifies an Async
Function's Task of its result with the type expected, so `Local t: Task<Int64> = Zero()` infers T
(A4-13). Lowering's check_values() checks every type lowering uses, and an instance's Fields and
base, and reports a fixed array whose sizes each fit but make more than 2147483647 elements, like
`G<65536, 65536>`'s `Int8[W, H]`; an instance used in code that isn't generic, whose Field works out
`N * 2` past the limit, was missed the same way (A4-14). Spawn and Thread.Start refuse type
arguments (A4-15), and Spawn takes what its work gives from a `Task<T>?` expected too (A4-17). An
Async lambda that returns nothing can end in `=> Await ...`, since is_call() takes an Await as a
call (A4-16). A fixed array's known UInt64 index or slice bound past Int64's range is an error at
compile time, and the runtime's new fb_unsigned_index_out_of_range() shows such an index as it is
when a T[]'s or fixed array's index panics (A4-18). An overload error shows a literal too big for
its type, or a generic Function's name, as written, not `<invalid>` (A4-19). Calling a Property says
it's a Property, and calling a Static Field says it's a field, not "only functions can be called"
(A4-20). A name repeated in a capture list is an error (A4-21). lambda_expr_t keeps its Function
keyword's span, where an Async lambda's errors point (A4-22). parameters_in() looks inside Tasks,
Channels, Ptrs, Atomics, Mutexes, and Callbacks, so check_expansion() finds instances without end
through them (A2-6). Misplaced and stale doc comments in sema.cpp and types.hpp were moved, merged,
or fixed (A2-8 to A2-11, A4-23), and spec 08 says AddressOf takes a ByRef parameter, as the compiler
does (A4-24). Lane 5's A4-new-2, an Int literal's exact match losing to ambiguity, is the two-tier
rule of spec 05 and decisions.md: it wins where the call's other arguments are exact too, which a
new test pins.

That closes phase 1: every one of the whole-codebase review's 161 findings, and the ones found on
the way (A-new-1 to A-new-3, A4-new-1 and 2, A7-new-1), is fixed with a test or dropped with its
reason in docs/review/.

Phase 1's review, lane 4's std findings: a Deque holds as many values as an Int counts and balances
without overflowing; a PriorityQueue in its smallest-first order panics at a NaN where it's added; a
Counter's one-value Add and AddAll panic at the caller; Sendable's comment and spec 14 match the
compiler and spec 07; a Map lets go of what a change takes out once it's whole again, so a key's or
value's Destroy can use it (A-new-5).

Step 10, Math II: `ThreadLocal Global`, each thread's own Global in LLVM's thread-local storage,
made by its initializer on the thread's first use, checked as work on another thread, and released
as each thread ends, the last made first (a pool worker's as the pool stops, the main thread's
before the Globals); Bounded's DoubleValue; Random, PCG32, with its ints, floats, picks, shuffles,
samples, normals, and directions, and the default generator over it, each thread's own, seeded from
the runtime's fb_random_seed until SeedRandom starts every thread's again: Rand, RandInt, RandFloat,
and arrays' Shuffle and Sample; Noise (Perlin, Simplex, Value, Worley, and Fbm); 30 easings; Mean,
Median, Variance, StdDev, Percentile, and Histogram over any number type; Bezier, Hermite,
Catmull-Rom, and B-spline points, and Curve2 and Curve3 with arc-length sampling; polygon area,
point-in-polygon, convex hull, earcut triangulation with holes, union, intersection, difference
(split, classified by winding, snapped), and offsetting; Fixed32 and Fixed64, whose Sqrt, Sin, and
Cos are correctly rounded integer math (Sin and Cos by Ziv's way, in 62 bits and then 124 near
halfway). Its review's 15 fixes: splines measured per segment; Fixed64's Sin and Cos correctly
rounded, ToFloat rounded once, and printed from its bits; Percentile, Median, Mean, and Histogram at
their edges, infinities, overflows, and subnormals; the booleans' probes and snapping by each part's
own size; Worley's 5 by 5 search and Simplex's cells as Doubles; WeightedPick past overflow, a
negative stream's panic, and the libm comments.

Step 10's ThreadLocal review (8 findings, 1 high): a ThreadLocal on a thread C started sets up its
letting_go_t first (ready_letting_go()), so it outlives the thread's locals; std's default generator
is each thread's Pcg, a Struct of PCG32's state with ByRef draws, so no thread releases it and any
Destroy can draw; fb_thread_locals_release marks the thread, and the pool's finish() on a marked
thread waits while restarted workers run late jobs; queued jobs carry their numbers, and a maker's
fb_thread_local_making() and fb_thread_local_made() keep run_own() to newer jobs meanwhile;
check_worker() doesn't take a Global's @MainThread as its initializer's, and the ThreadLocal,
Shared, and Async placement errors give way to the attribute-order error. Tests:
thread_locals_c_thread, math_rand_destroy, thread_locals_late_jobs, thread_locals_making.

Step 9's SIMD (m6-plan item 13): optimize() sets LLVMPassBuilderOptionsSetLoopVectorization and
SetSLPVectorization for release builds; tests/compiler/codegen_tests.cpp's ir_of() and its test that
a release build's IR of a Vec4 add and a Mat4 product, over arrays and on a Class's Fields, for
x86-64 and AArch64, holds `<4 x float>` operations, and a debug build's doesn't; stdlib.md says a
result that goes to memory is SIMD and one read a float at a time, as Print does, isn't.

Step 23, std.db and std.uuid but for Query<T> and V7: SQLite 3.54.1's amalgamation in third_party/
(without extension loading or what's deprecated, with foreign keys on, DQS=0, FTS5, and its math
functions), its own fb_runtime_db archive of fb_db_ functions, and its notice, the dedication and
blessing atop sqlite3.h; std/db/database.fb's Database (Open, with a 5-second busy wait, Close,
IsClosed, Execute, Rows, Prepare, Begin, Migrate, Changes, LastId), SqlValue (Extends of the
integers, Bool, the floats, String, and UInt8[], Nullable, and a program's own type through Bind),
Execute and Rows by count up to eight values, Row (Integer, Real, Text, Blob, Type, IsNull, by index
or name, read eagerly), Statement (Bind by index or name, Clear, Parameters, Execute, Rows),
Transaction (BEGIN IMMEDIATE outermost, SAVEPOINTs inside, rolled back when let go of, Commit,
Rollback, IsOpen, following SQLite's own rollbacks), Migrate by PRAGMA user_version, and
DatabaseError's kinds from SQLite's codes; std/uuid/uuid.fb's Uuid (V4 over SecureRandom, Parse,
ToString, Bytes, FromBytes, Version, and `<`). Tests: db_execute, db_errors, db_statements,
db_transactions, db_migrate, db_close, uuid, fourteen panic_db_ ones and panic_uuid_bytes, driver
cases for what db and uuid programs link, and CLI cases for SQLite's notice, a db program that loads
only the system's libraries, and file databases. The review's fixes: Rows(sql) panics at SQL that
takes parameters (S23-sqlite-1, tx-2, uuid-1); Collect reads a statement's columns after its first
step, which re-prepares it after a schema change (sqlite-2, tx-1); fb_db_close finalizes the
connection's statements, so Close lets go of the file and its locks at once, a Statement keeps its
parameter count, and Clear and Expect panic on a closed Database first (sqlite-3, tx-5);
Version(count) throws past the last migration inside each migration's Transaction too (tx-3); a NaN
Float throws rather than binding NULL (sqlite-4, tx-4); a CLI test opens, reopens, and shares file
databases, waits out the busy timeout, and opens one that isn't a database (sqlite-5); stdlib.md
names every panic (uuid-2).

Phase 1's review, lane 1's 17 sema findings: check_defaults() checks a Const Function's defaults in
an empty body with the kept bag, so a run inside another function or quietly() resolves the
function's own names and reports their errors (P1-sema-checks-1, -2); a generic Const Function's
call counts report_bounds()'s errors before it runs (-3); a T[]'s Contains calls note_compared(),
and link_compared_fields() leads Equatable's == to the Operator == of the fields of a Struct that
compares them, for the main-thread rules (-4); a cast to or from an Enum declares it early while
types are declared, and lowering's to_enum() takes an Enum with no values (-5, -6); lowering's
check_values() walks every Struct's and Class's Fields and base, at a parameter's or a Global's
place (-7); SizeOf holds a fixed array in what it measures to the written limit, through
oversized_array() (-8); common_type_of() meets the tuples that type alone first, quietly, so their
order doesn't matter (-9); check_inherited_static() keeps a Static Method from an inherited Field's
or Property's name (types-1); check_type_parameter_names() checks a type's type parameters once
every type is declared (types-2); is_hash() is the one test of a Hash(): UInt64, for has_hash() and
gets_default() (types-3); hash_instances() decides Hashable for each instance an Extend gives an ==,
with analysis_t's unhashed_instances, and has_hash() counts an instance base's Extend (types-4, spec
07); comparable() stops at an instance whose Field the type table cut short, and compared_growth()
says so in the == and Contains errors (types-5); a Static Property used by name skips
check_self_ready() (types-6); report_read_only() says a lambda or a Parallel For's body in New runs
later or on other threads (types-8); in_new_of() has one doc comment (types-9). Tests: sema_tests
and mir_tests cases beside each, and hashable_instances.

Step 18's library half (lane 6): runtime/src/concurrent.cpp and fb/concurrent.hpp, handles as
UInt64s (std::bit_cast): fb_semaphore_, fb_barrier_, fb_wait_group_, fb_once_, fb_rwlock_
(writer-preferring, a thread_local list of held locks, so taking one again gives 1), and fb_signal_
(fb_signal_task, an Extern giving a plain Task the signal finishes, not an I/O request).
std/concurrent/locks.fb: Semaphore (Acquire, TryAcquire, Release, Available), Barrier (Wait, True
for the round's last, Count), WaitGroup (Add, Done, Wait, Count), Once (Do, IsDone), RwLock<T:
Sendable> (Read<R>, Write, Value, over a Mutex<T?>); concurrent_map.fb: ConcurrentMap<K, V> (16
MapParts of Mutex<ImmutableMap<K, MapCell<V>>> by the hash's top 4 bits; Add, TryAdd, Get, GetOrAdd,
Update, AddOrUpdate, [], []=, Contains, Remove, Clear, Keys, Values, ToMap, Count, DefaultValue;
make and change outside the lock, retried when RefEquals sees another cell; CheckMapRoom) and
ConcurrentMapIterator over per-part copies; concurrent_queue.fb: ConcurrentQueue<T: Sendable>
(QueueValues: a Channel ring that GrownRing doubles to an Int's most, and TryPeek's Front; Enqueue,
Add, AddAll, TryDequeue, TryPeek, ToArray, Clear, Count; CheckQueueRoom); tasks.fb: TaskSource<T:
Sendable> (Task, IsDone, Finish, TryFinish; Finished awaits the signal's Task), WhenAll and WhenAny
for Task<T>[] and Task[] (AllOf, FirstOf). std/events/event.fb: Event<T> (Subscribe, Emit over the
handlers it started with, Count, Clear; EventHandler<T>s dropped meanwhile come out after the
outermost Emit) and Subscription (Cancel, IsActive, a Weak<Subscribed>). Tests:
concurrent_semaphore, _barrier, _wait_group, _once, _rwlock, _map, _queue, _tasks, events, 15
panic_concurrent_ programs, tests/runtime/concurrent_tests.cpp, and std_tests' Int-limits case.

Step 18's review (lane 6): ConcurrentQueue.Enqueue counts under its Lock; ConcurrentMap's changes,
ConcurrentQueue.Clear, and RwLock.Write keep what they replace in a Local until End Lock, so a
Destroy can use them; ImmutableArrayIterator and ImmutableMapIterator keep their paths in ArrayPath
and HashPath nodes that Next replaces, so a copy keeps its place; TaskSource's Finished awaits only
a signal not given yet, and the docs say a Task read before Finish is done at the main thread's next
tick; ConcurrentMap's PartIndex mixes the hash as Map's BucketOf does; WhenAny's docs give its tie
rule (of tasks done before it looks at a tick, the first); EventHandler.Drop keeps the handler until
its Event has counted it off, and Event.Clear takes its handlers out once at the end. Tests:
concurrent_queue's Count on four threads, a Destroy that uses its map, queue, or RwLock, partly used
iterators in immutable and concurrent_map, concurrent_tasks' Task read after Finish and WhenAny's
tie, events' self-cancelling Widget, std_tests' parts, and program_tests' Clear of 100000 handlers;
concurrent_map's race waits at a Barrier between its adds and removes.

Step 11 (std.math.big; typed units wait for the compiler lane's computed Const type arguments):
BigInt over libtommath, Decimal over mpdecimal, Rational, and Complex, in std/math/big/, each
library bundled statically through third_party/ with a runtime archive of its own
(fb_runtime_bigint, fb_runtime_decimal). BigInt's operators are an Int's without overflow, with
Parse and ToString in radixes 2 to 36, bytes, Int64 and Double conversions rounded to nearest, and
number theory (ModPow, ModInverse, Gcd, Lcm, Sqrt, and a deterministic Baillie-PSW test). Decimal
works in Python's default context, 28 digits half to even, and holds what it's made from exactly;
Rational is exact, in lowest terms; Complex follows CPython's arithmetic and cmath. The tests check
7926 values Python's int, decimal, fractions, and cmath gave, and 30 panics at the caller. A literal
beside a Struct's or Class's operand now takes the type of the Operator's parameter even when it
doesn't fit an Int, as spec 06 says.

Step 11's review: 15 findings fixed. A BigInt holds up to 2147483640 bits and a result past them
panics at the caller; Decimal.FromDouble gives the shortest digits and Round keeps its exponents in
range as quantize does; Complex's Sqrt, Exp, Log, and FromPolar scale and take special values as
cmath and C99 do, and its parts are ReadOnly; Rational.FromDecimal reads a Decimal's digits and
exponent, and ** takes the lowest Int64; BigInt's gcd and lcm take a step of Euclid's first; a
literal expression only an Int64 holds, beside a Struct, takes the Operator's Int64; Decimal reads
Doubles through fast_float, which macOS needs.

Step 16, std.time (lane 4): `2.Seconds` lexes as an integer and a member, and a literal before a
member keeps its default type or takes the first wider one that has the member, Float then Double,
Int then Int64 then UInt64 (sema's object_hint, holds, and has_member_as; spec 01). Wait and
RealWait take a Duration, replacing Seconds and RealSeconds (sema's find_duration and
check_wait_call; lowering's WAIT_TIME, fb_wait_time). std/time: Duration and the units of an Int64
and a Double; Instant, Stopwatch, Measure, Sleep; Timestamp; Date, TimeOfDay, LocalDateTime,
Weekday; TimeZone over the IANA tz database 2026e, compiled by its zic at build time and embedded in
fb_tzdata beside fb_runtime_tz, and the OS's zone; ZonedDateTime and Disambiguation; ISO 8601, RFC
2822, and Java-style patterns; Timer on the I/O thread's libuv timers, Deadline, FixedStep,
RateLimiter, Debounce. DST tests in twelve zones match Python's zoneinfo; Sleep and the timers never
wake early.

Step 16's review fixes (lane 4): VV reads a fixed zone's name back; std's Extends' Properties give
the Import hint; Sleep saturates at the monotonic clock's latest time (fb::time::sleep_end);
FixedStep keeps a step's part past MaxSteps and takes any MaxSteps; RateLimiter saturates;
calendar.fb's Combined() for the Unix and difference sums; RFC 2822's offsets under a day, DD's two
or three digits, RFC 3339's leap second and long fractions; Duration's exact Double scaling (Scaled,
AddPart, Nearest), its narrow-integer operators, and NaN's message; a Scheduler's timers in Int64
nanoseconds (later_by, real_nanoseconds); and Timer's comment.

Step 12, std.ffi's additions: `Extern "stdcall"` goes (parse_extern() says the block is "C";
convention_t::STDCALL went). Built-in `CLong`, `CULong`, and `CWChar` (type_id_t CLONG, CULONG,
CWCHAR after UINT64, is_c_integer()), C's long, unsigned long, and wchar_t: `c_integers_t`,
every_c_integers() (Windows', LP64's, AArch64 Linux's, Cygwin's) and on_target();
converts_losslessly() and make_number() hold for every target's, supports() doesn't negate a CWChar,
and an Enum's type isn't one; c_abi's c_layout() takes a target's integers, c_integers_of(triple),
and build_c_integers(), the build's target, which SizeOf and const_value's folding
(on_build_target(), as_c_integer()) use; codegen's `m_integers` sizes them by its triple
(llvm_type(), and on_target() in c_extension(), check_index(), to_int64(), value(binary_t),
convert(), hash(), and to_text()); sema's on_windows() says what a literal has to fit. fb-bindgen
writes them from the written type (c_integer() through peeled() typedefs, stopping at
sized_by_name() ones like int64_t and size_t; written_as(), map_pointee(), and map_parameter() keep
the written types), and a number macro of one where every target's holds it (fits_everywhere()).
std/ffi/cdata.fb's `CData` (builtin_interfaces_t::c_data), a constraint sema decides
(why_not_c_data(), said_c_data(), check_listed_c_data(), conformance_query_t::c_data(),
type_t::c_layout and set_c_layout()), so not_c_data() takes a type parameter that has it: a Ptr's T
(`pending_pointer_t` keeps its bounds), AddressOf's, and SizeOf's, which lowering measures per
instance (expr_info_t::size_of, size_of()), and check_pointer_arithmetic() steps and counts by it.
Mem's AllocUninitialized, AllocAligned, and FreeAligned over the runtime's
fb_mem_alloc_uninitialized, fb_mem_alloc_aligned, and fb_mem_free_aligned. std/ffi/buffer.fb's
`Buffer<T: CData>`: native memory with checked `[]` and `[]=`, Count, Slice views that keep it alive
(`Viewed`), CopyTo, ToArray, Iterate (BufferIterator<T>), the Unsafe Pointer(), and the Unsafe
Buffer<T>.View over C's memory. Tests: c_abi_tests' c_integers_of() per triple and the IR of a
binding using CLong, CULong, and CWChar on four targets; type_tests' and sema_tests' conversions,
literals, Consts, and CData; bindgen_tests' long, typedefs, and macros on three targets;
tests/programs/c_integers.fb, cdata.fb, buffer.fb, panic_buffer_index.fb, and
panic_mem_alignment.fb, unsafe.fb's aligned memory, runtime_tests' fb_mem_alloc_aligned, and vmath's
Buffered() through vecmath's vm_fill and vm_sum.

Its review's fixes: lowering's unsettable() keeps f.Set to what the loop's code could assign by name
(Private in its file, Package in its package, Protected in its file and the Classes extending it;
spec 13), and Buffer's Fields are ReadOnly; unsigned_of() takes a CLong's distance as a CULong and a
CWChar's as a UInt32; sema's takes_no_bytes() counts Ptrs at a generic Struct of a CData T, and
lowering's check_steps() refuses stepping a Ptr at an instance's T of more bytes than an Int64
counts; fb-bindgen's map_parameter() binds a function-typed parameter as a Callback, problem_of()
checks a field's written type, wide_character() makes L'x' a CWChar, is_target_wchar() keeps a
header's own wchar_t by size elsewhere, and map_array() parenthesizes a Callback that gives
something; Buffer's View bounds its count by SizeOf<T>() as New does, and its messages say "1 value
of a type"; vmath's Buffered fills a view from index 3; a panic program for each of Buffer's panics;
Mem's Resize and Free name AllocUninitialized; cdata.fb's example compiles.

S18-new-1, found while fixing S18-collections-4: SortedMap's and SortedSet's, Trie's, and
IntervalTree's iterators kept their stacks in arrays, so a copy moved the original (Next, then Count
twice, gave 49 0); each keeps its node and place in the Struct and the nodes above in a list of
nodes a step never changes (SortedPath, TriePath, IntervalPath), so a copy keeps its place.

Step 22's codecs over bytes (lane 6): third_party/ fetches zlib-ng 2.3.3 (ZLIB_COMPAT, no gzFile,
HAVE_UNISTD_H off on Windows), zstd 1.5.7 and LZ4 1.10.0 from their library C files
(ZSTD_DISABLE_ASM, ZSTD_LEGACY_SUPPORT=0; XXH_NAMESPACE=LZ4_), liblzma from XZ Utils 5.8.4 (no
threads, NLS, docs, or tools; lzma.h's folder given to its users), and minizip-ng 4.2.2 from its C
files (memory streams, HAVE_ZLIB, HAVE_LZMA, HAVE_ZSTD, MZ_ZIP_NO_CRYPTO, a written mz_config.h),
each static, with notices (minizip-ng's with its files' authors and the three libraries it links),
and BUILD_SHARED_LIBS and BUILD_TESTING off. The runtime's fb/compress.hpp and compress_internal.hpp
(result_t, entry_kind_t, output_t, failure_t, utf8_length, is_utf8, utf8_or_replaced, append_utf8),
and parts fb_runtime_zlib (zlib.cpp: fb_zlib_ encoders and decoders of raw Deflate, Zlib, and Gzip
members one after another, a plain gz_header), fb_runtime_zstd, fb_runtime_lz4, and fb_runtime_xz
(each an encoder and a decoder with a limit; liblzma's memlimit, zstd's windowTooLarge as
unsupported), seven_zip.cpp in fb_runtime_xz (fb_xz_7z_: signature and encoded headers, streams,
folders by bind pairs through lzma_raw_decoder with lzma_properties_decode and
lzma_raw_decoder_memusage, substreams and CRCs, files with empty streams, anti-items, attributes and
Unix links, UTF-16 names, the last folder kept), fb_runtime_zip (zip.cpp: fb_zip_ over minizip-ng's
mz_zip and mz_stream_mem, CP437 names, no data descriptors, a 1980 date, and mz_os_is_dir_separator
and mz_os_rand), and tar.cpp in fb_runtime (fb_tar_: ustar, pax, GNU long names and base-256, old
tars, sparse as unsupported; pax for long or non-ASCII names). bundled_library_t entries for the
five. std/core/error.fb's IOErrorKind.INVALID_DATA. std/compress/: codec.fb (MOST_BYTES, CheckLevel,
CheckLimit, Failed), deflate.fb (Deflate, Zlib, Gzip, and their Compressors and Decompressors),
zstd.fb, lz4.fb, xz.fb (Zstd, Lz4, XZ, and theirs), archive.fb (ArchiveEntry, ArchiveEntryKind,
LastNamed, CheckName, CheckFileName), zip.fb (Zip, ZipReader, ZipWriter, ZipMethod), tar.fb (Tar,
TarReader, TarWriter, Unwrapped), seven_zip.fb (SevenZip, SevenZipReader). Tests: compress_codecs,
compress_tools (with files/ and make_data.py), compress_archives (with make_archives.py), 21
panic_compress_ programs, and driver_tests' bundled libraries case.

Step 22's review (25 findings: 3 high, 11 medium, 11 low) was fixed on its branch before it landed:
Zstd and Lz4 decode output that fills their 64 KiB chunks exactly, and ZstdCompressor takes empty
parts; a 7z's LZMA coder is liblzma's LZMA1EXT with its size, so LZMA:BCJ without an end marker
reads; ZipWriter writes over a std::vector stream of its own, linear rather than quadratic; a 7z's
bind pairs are checked, a folder of links is decoded once, and a file of a solid folder past 2 GiB
reads; a zip's entries are checked against their CRC-32, LZMA and XZ entries are decoded over
liblzma within an array's memory, only Unix-like hosts' modes make links, names with '\' panic, LZMA
and XZ levels go from 1 to 9, and local headers take Zip64 only where needed; a tar takes POSIX's
unknown types as files, GNU's 'D' folders, pax sparse files and old sparse extension blocks, and old
tars' folders; Tar.Read skips skippable frames; archive writers' Finish panics at its caller past a
UInt8[]'s bytes; preset dictionaries are unsupported, not invalid; spec 09 lists INVALID_DATA; and
compress_tools reads the lz4 tool's, git's, and raw Deflate data.

Std.core's Value (lane 4, step 17's first unit): std/core/value.fb's `Value`, a Class of data of any
shape, and `ValueKind` (NULL, BOOL, INT, UINT, DOUBLE, STRING, BYTES, ARRAY, OBJECT), with a New per
type, `Array()` and `Object()`, the Is Properties and `Count`, the As getters, `[]` and `[]=` by key
and index, `Path`, `Add`, `Insert`, `RemoveAt`, `Remove`, `Keys`, `Contains`, a deep `==`, `Hash`,
and `Clone`, JSON text, and no Value holding itself (`Holds`, `CheckHeld`); the runtime's
`fb_value_quoted`, `fb_value_hex`, and `fb_value_path`.

Value's review fixes (8): ToString, Clone, Hash, ==, and Holds walk with stacks of their own, so a
Value 100000 deep works in debug and release (Hash through a Step Struct of each Array's or Object's
place and running hash, ToString through parts the runtime's new `fb_value_joined` joins); Holds
marks what it looks through with its walk's own object (a private `Walk` Class in a `Walked` Field),
so a part held in several places is looked through once; the docs say a Value holds no cycle but can
sit in several places, which ToString, ==, Hash, and Clone go through once for each, that AsArray's
elements are the Value's own, and name Keys, Contains, and bytes' `<hex>` text. Tests:
core_value_deep and panic_value_holds_itself_shared.

Step 28's first half, std.net without streams: std/net/address.fb's `IpFamily`, `IpAddress` (a
family and 128 bits, High and Low; `V4`, `V6`, `FromBytes`, which panics at the caller for a count
but 4 or 16, and `Parse`, strict: RFC 4291's text, no leading zeros in IPv4, no zones; `ToString` as
RFC 5952 has it, and `Text(dotted)` for a URL's host; `IsLoopback`, `IsUnspecified`, `IsPrivate`,
`IsLinkLocal`, `IsMulticast`, `IsBroadcast`, `IsDocumentation` with RFC 9637's 3fff::/20; `ToV4`,
`ToV6`; `Loopback`, `Any`, and the rest), and `SocketAddress` (`Address`, `Port`, `ScopeId`, which
panics at the caller on IPv4; `Parse` and `ToString` of `127.0.0.1:80`, `[::1]:80`, and
`[fe80::1%3]:80`). std/net/url.fb's `Url`, an immutable Struct from the WHATWG URL Standard's basic
URL parser (`Parser` and its 20 `State`s, the percent-encode sets as `PercentSet`, host parsing with
IPv6, WHATWG's IPv4 numbers, opaque hosts, and domain to ASCII, which maps what UTS #46 needs that
the test data has, without its tables, and encodes Punycode as RFC 3492 has it): `Parse(text)`,
`Parse(text, base)` with a Url or a String, `Resolve`, its parts, `PortOrDefault`, `HasOpaquePath`,
`Origin`, `Params`, `Encode`, `Decode`, and the serializer; and `QueryParams`, a Class as
URLSearchParams has it (`Sort` by UTF-16 code units). std/net/dns.fb's `Dns.Resolve` and
`ResolveAsync`, over runtime/src/dns.cpp's `fb_io_resolve`, `fb_io_resolve_async`, and
`fb_io_error_text` in fb_runtime_io: libuv's getaddrinfo, started on the I/O thread through io.cpp's
job queue (`fb::runtime::on_io_thread` in io_internal.hpp), as 17-byte records, each address once; a
NetError of HOST_NOT_FOUND or OTHER. The runtime's `fb_string_from_array`, for std's text made a
byte at a time. tests/data holds web-platform-tests' urltestdata.json (c48d587) and its license,
which url_tests.cpp runs as one program: every case but 732 (mathematical bold letters, which need
UTS #46's NFKC) passes. Programs: net_addresses, net_url (RFC 3986 §5.4's examples too), net_query,
net_dns ("localhost" and literals only), panic_net_address_bytes, and panic_net_address_scope.

Step 18's language part (lane 8): `Where T: Class`: parse_constraint_part() (the keyword as a named
type "Class"), `builtin_interfaces_t::any_class` (declare_any_class(), a constraint-only Interface
extending Equatable), constraint_interface(), is_any_class() and class_hint(), so Weak<T>, [Weak x],
RefEquals, 'Is', and Case As take a class-bound T, and a Weak<T> of a Class & Sendable T is
Sendable. `Sendable Func(A): R`: `function_type_t::sendable`, `type_t::sendable`
(type_table_t::function(..., sendable)), is_sendable_func(), func_like(); a lambda or Function's
name where one is expected is one (check_sendable_captures(), shared with check_spawn(); a worker "a
Sendable Func can run on any thread"), its closure Class set_shared() (function_class(function,
sendable), "as a Sendable value"; lowering's closure_copy() keeps it), Sendable, converting to a
Func; Spawn and Thread.Start take one; the MIR dump's ", Shared". `Task<T> Throws E` as a type:
`named_type_t::throws` (parse_named_type(throws), parse_task_throws(), `m_function_result`),
written_task() and check_task_throws() (`m_pending_throws` until check_throws()); `E: Error`
(`builtin_interfaces_t::any_error`, declare_any_error()), is_error() and throws_as() for an
error-bound type parameter, check_throwing_call() and Async calls with substituted Throws,
type_table_t::substitute() of a Task's throws, unify() of them, a Task converting to one whose
Throws covers its own, and lowering's Task local with the instance's Throws. Tests: sema's "Where T:
Class says...", "a Weak<T> of a Class that's Sendable...", "a Sendable Func is a lambda or a
Function's name...", "Task<T> Throws E is a type...", "a Task's Throws lists errors..."; mir's "a
Sendable Func's closure counts its owners atomically" and "a generic Async Function's Task has its
instance's errors"; parser's; programs class_constraint, class_owner_events, sendable_func,
sendable_lazy, sendable_parallel_map, async_throws_generic.

Step 18's language review fixes (24, one high): the Class and Error constraints are Public, so any
module's Public generic code has them; match() checks a lambda or a Function's name against a
Sendable Func parameter with that type; member_throws() and implemented_throws() map Throws E to the
instance's E for Overrides, a generic Interface's Methods and defaults, News from a generic base,
and For ... In's Iterate; a Func that's a function's result leaves the Throws after it to the
function, and in a Func type's or lambda's parameters a ',' ends a Task's Throws; unify() takes a
Weak<T>'s T from a T or T?; a Task's Throws checks covered and repeated type parameters and infers E
in any order; type_table_t::listed() puts a throwing Task in parentheses in tuples, parameters, type
arguments, and qualified names; a bad Throws is reported once; Spawn of a T? of a Sendable Func says
it may be Null, and a lambda capturing Self kept in a Lock of Self's Mutex warns of the cycle.
Tests: class_constraint_modules.fb, throws_generic_members.fb, and sema tests for each diagnostic.

std.debug (lane 9): fb/debug.hpp and runtime/src/debug.cpp in fb_runtime: fb_debug_attached()
(Windows' IsDebuggerPresent, macOS's sysctl P_TRACED, Linux's /proc/self/status TracerPid, else 0)
and fb_debug_break() (DebugBreak, or raise(SIGTRAP) with an empty handler in place, only with a
debugger attached). std/debug/debugger.fb: `IsDebuggerAttached` and `Breakpoint`. Tests:
debug_debugger, and debug_tests, which traces a child on Linux and macOS (a tracer that swallows
traps and one that hands them back) and debugs one on Windows. The review's two fixes are in
docs/review/debug.md.

Step 11 (lane 5's part): Const type arguments of any value of their type, computed with +, -, *, /,
and % (toward 0), -N, and parentheses, with a size below 1, a division by 0, or an Int64 overflow an
error where the instance is made; Operators with type parameters of their own, inferred from their
operands; `Alias`, a transparent name for a type, which calls and Statics go through; and std.math's
typed units, `Quantity<L, M, T>` with generic * and /, 14 unit Aliases from Meters to
KilogramsPerCubicMeter, and the units made from others, so Meters / Seconds is MetersPerSecond and
Meters + Seconds is a compile error.

Step 5, formatting, parsing, and the debug form: runtime/include/fb/text_format.hpp, a header-only
formatter that the compiler (const_value.cpp's formatted(), the evaluator) and the runtime
(runtime/src/format.cpp) both build, so folding and running agree: `format_spec_t` (fill,
`format_align_t`, `format_sign_t`, alternate, zero, width, grouping, precision, kind),
parse_format_spec() and its `format_spec_error_t`, format_spec_misfit() for a `format_subject_t`
(INTEGER, FLOAT, STRING, TEXT, DEBUG), shortest_text(), which writes floats invariantly (`1e21`,
`1.5e-7`, `NaN`, `Inf`, `-Inf`, for fb_string_from_float32/64 too), debug_text(), format_text(),
format_integer(), format_signed(), and format_float(); fb_compiler gets runtime/include, and
fb_runtime links fast_float. The lexer's FORMAT_SPEC token: in an interpolation, a `:` outside
brackets (`string_state_t::nesting`) with no space after it starts lex_format_spec(), raw text to
the `}`; ast.hpp's `format_t` and `interpolation_t`, so a string part is a
`std::variant<std::string, interpolation_t>`; the parser's errors for a spaced spec and for a
ternary's `:` without a space; dump.cpp's `(format value ".2")`. Sema's check_format() and
format_subject() check a spec at the spec, a Formattable type of the program's own and a Formattable
T skip it, and a T takes what its constraints give. std/core/interfaces.fb's Formattable (Extends
Printable, `Format(spec): String?`), which the numbers, Bool, String, and Enums implement
(builtin_format(), over the runtime's spec parsing); lowering's add_formatted() and own_format(),
which panics at the spec on Null, and an instance's misfit spec panics there too; runtime calls
FORMAT_INT, FORMAT_UINT, FORMAT_FLOAT, and FORMAT_TEXT (fb_format_int, _uint, _float, _text).
std/core/format.fb's Format(template, values: Formattable...) Throws FormatError, over
fb_format_split; a `values: Formattable...` parameter (`symbol_t::formats`) takes objects as they
are and boxes the rest in std's `Formatted<T>` (`analysis_t::formatted_box`). std/core/parse.fb's
Parse, a Static Method in an Extend of each integer type (base 2 to 36, SignedIn() and
UnsignedIn()), Float32, Float64, and Bool, over fb_parse_int, fb_parse_uint, and fb_parse_float
(fast_float), each giving a T?. check_defaults() walks an Extend's members, so its Methods keep
their defaults. The debug form: `generated_kind_t` DEBUG and DEBUG_ENTRIES, `(value, Int32 indent)`
functions that debug_of() calls and debug_body() fills, laid out by debug_layout()'s child, first,
next, and last; runtime calls DEBUG_STRING, DEBUG_OBJECT, and DEBUG_INDENT (fb_string_debug,
fb_debug_object, which dispatches through fb_class_t's new `debug` slot and keeps a thread_local
list of the objects being shown, for cycles, and fb_debug_indent); `mir_class_t::debug`, which
lower_library() fills for each Class, and each array type with tables, once `m_debug_used`; a Weak
shows what reading it gives. std/core/format.fb's Dump, DebugElements and DebugEntries
(`analysis_t:: debug_elements`, `debug_entries`), and ElementsOf (`elements_of`, found by
std_function()), with `builtin_interfaces_t::iterable`; sema's check_debug_attributes(); std's
collections but Grid, Grid3, and Range have one or the other. Tests:
tests/compiler/text_format_tests.cpp, lexer, parser, sema, mir, and driver cases, format_specs,
panic_format_generic, panic_format_own, format_template, parse, debug_form, and a case in extend.

Step 5's review (24 findings, docs/review/step5.md): sema's m_copied_parameters and copy_defaults()
give an Interface's default Methods' and inherited News' copied parameters their originals'
defaults. debug_of() sends a T[] through fb_debug_object, whose cycle marker for an array is "[ …
]", with mir_module_t's array_debugs naming each array Class's generated debug function for
codegen's array_class(); holds_fixed_array() makes a debug function take a fixed array, or what
holds one, ByRef. text_format.hpp's float_chars() has a 1400-byte buffer and checks to_chars;
shortest_fixed() writes the shortest digits in fixed form, for 'f' and '%' without a precision and
for ',' and '0'; hundred_times() moves '%''s point; Inf% and NaN% keep their '%'. formatted_number()
pads a number's debug form as a number; std's Formatted<T> box shows as its value; flags_debug()
shows a @Flags value's other bits as "| 0x8". Format's '?' fields fall back to the debug form
through fb_format_debug and fb_format_text when the value's Format gives Null. note_formatted() and
note_debugged() count what a spec runs for the main-thread checks. Parse reads an underflow as 0
(read_whole()) and "-0" as an unsigned 0; an empty spec is none. Spec 05, spec 14, and stdlib.md say
the rest.

Step 6 added text. `Char` is a built-in type, a Unicode scalar value of 32 bits, which a character
literal like `'a'` or `'\u{1F600}'` now makes, never a number; `s.Chars` gives a `Char[]`. A Char
compares by its code, is Equatable, Hashable, Comparable, Printable, and Defaultable (its zero is
U+0000), can be a Const, a Const Function's value, an Attribute's parameter, and a Select's subject
with `Case 'a' To 'z'` ranges, and `As` turns it into an integer, its code, while an integer that
isn't a scalar value `As Char` is an error at compile time and a panic at run time. A String holds
at most 2147483647 bytes, as an array does elements. std.text gives a String its Methods by byte
position, over the runtime's new `fb_text_` functions: `Length`, `CharCount`, `GraphemeCount`,
`Find`, `FindLast`, `FindAll`, `Count`, `Slice` (which panics inside a character), `Before`,
`After`, `SplitOnce`, `Split`, `SplitN`, `SplitLines`, `SplitWhitespace`, `Join`, `Replace`,
`ReplaceFirst`, the Trims, `ToUpper` and `ToLower` by Unicode's full case mapping ("straße" is
"STRASSE", and a word's last capital sigma lowers to the final one), `Repeated`, the Pads,
`Compare`, `EqualsIgnoreCase`, `NaturalCompare`, `Lines`, `Graphemes`, and `String.FromUtf8`; a Char
its categories and own case; `StringBuilder`; and `Scanner`, with its Line and Column.
std.text.unicode has `Category`, `Normalize` (NFC, NFD, NFKC, NFKD), `CaseFold`, and `DisplayWidth`,
over utf8proc 2.12.0 (Unicode 18), bundled as step 1 bundled libuv: a runtime archive of its own,
`fb_runtime_unicode`, whose `fb_unicode_` functions a program links, with utf8proc and its notice,
only when it calls them. std.text.encoding has UTF-16 and UTF-32 (each LE and BE), ASCII, Latin-1,
and Windows-1252 (WHATWG's, so every byte round-trips), BOM detection, Base64 (standard and
URL-safe), Hex, Base32, percent-encoding, HTML escaping, and C-style `Escape` and `Unescape`. A std
Property on a built-in type, like String's `Length`, now says which module it's in, as a Method
does.

Step 6's review (23 findings, 2 high) fixed, each with a test that fails without it;
docs/review/step6.md has their Status lines.

Phase 1's review of the front end and runtime found ten problems, which lane 3 fixed with A-new-4.
fb bindgen keeps the files libclang reads for a header, and an `-o`, or the module path it would
pick, that names one of them is a usage error, so it no longer overwrites a header the named one
includes. The parser stops a line's continuation only after a missing expression, so `x = 1e999 +`
then `2.0` gives one error, not one for each line. Blitz's `New Foo` without parentheses parses as
the call it means, with no second error, and the error names the type as it's written
(`shapes.Circle()`, `Stack<Int>()`). `--show ast` and `--show types` write an @Deprecated message as
source writes it. On Windows, a program fb builds that Ctrl+C, Ctrl+Break, or closing the console
ends runs its end first, through a console control handler, and gives back the code page and modes.
On Linux and macOS it does the same through SIGINT, SIGTERM, and SIGHUP handlers, which then raise
the signal again, so stdin's flags come back. fb reads both streams' console modes before it changes
either, since they can be one screen buffer. The leak report's overflow line says "and 1 more
cycle". A full array's panic gives its place, from Add (`fb_array_push` takes a location now) and
from Insert. Every Status line in docs/review/ names a commit on master. A7-2's line and 62 others
had named the copies lanes' branches held before a rebase. fb run tries removing its temp folder
again for up to two seconds, through the driver's `remove_temporary()`, since Windows or a scanner
can still hold the program's executable just after it ends, and the Ctrl+C test's program is named
for its process, so copies of the test can run at once.

Step 7 added the rest of text's everyday tools. std.text.regex runs PCRE2 10.49, bundled as utf8proc
was: an archive of its own, `fb_runtime_regex`, whose `fb_regex_` functions a program links, with
PCRE2 and its notice, only when it calls them. Its 8-bit library is built without its JIT. A `Regex`
reads its pattern as UTF-8, with Unicode's `\w`, `\d`, `\s`, and `\b`, and `RegexFlags`
(`IGNORE_CASE`, `MULTILINE`, `DOT_ALL`, `EXTENDED`). It has `IsMatch`, `Find` from a position,
`FindAll`, `Replace` and `ReplaceFirst` with `$1`, `${name}`, and `$$`, or a Function, `Split`,
`Escape`, `GroupCount`, and `GroupNames`, and a `Match` has groups by number and name. Each match
stops at its Regex's `MatchLimit` (10 million steps) or `MemoryLimit` (64 MiB) and throws a
`RegexError`, so a pattern that backtracks without end throws within its limit. A pattern that isn't
valid panics where its Regex is made, and `Regex.Parse` throws instead, for one from elsewhere.
String's `Matches`, `ReplaceAll`, and `SplitBy` share each thread's last 16 compiled patterns. The
compiler links PCRE2 too: a pattern it knows at compile time, a literal's or a Const's, passed to
Regex, `Parse`, or a shortcut, is an error unless PCRE2 compiles it, with the flags given when
they're known. The error points at the character PCRE2 stopped at, mapped through a regular
literal's escapes. std.text.glob matches a shell's wildcards (`*`, `**`, `?`, sets, braces), and
std.text.layout gives a String `WordWrap` in a terminal's columns, `Dedent`, and `Indent`, and the
Function `Levenshtein`. std.text.fuzzy scores matches as fzf does, and its test is fzf's own.
std.text.humanize has plurals, relative times, ordinals, numbers in words, and case styles.
std.text.diff has Myers' line and character diffs, unified diffs, and a three-way merge, whose test
is what git diff and git merge-file give for the same texts. In the language, a lambda whose
parameters have no types now takes them from the one overload whose parameter is a Func it can be,
and a Try around a call with an error no longer also warns that it isn't needed.

Step 7's review: 25 confirmed findings (1 high, 11 medium, 13 low; 3 refuted), all fixed with a test
each. std.text.diff goes as git's xdiff does: a line with no equal in the other text is changed
without a search, xdl_split's linear-space Myers marks the rest (memory linear, not the square of
the edits), and xdl_change_compact slides each run of changes, so Unified and Merge give git's
results, 2800 random cases checked; a merge takes a deletion both sides made (the high), joins
conflicts across runs of kept lines, writes "\r\n" markers where the texts' lines end so, counts
only ASCII letters and digits between conflicts, and Unified takes any context. Regex's MatchLimit
counts a search's steps over every place it tries, through a callout before each item, so a
quadratic search throws; after an empty match at "\r\n" a pattern with its own CR or LF looks from
the "\n"; Escape covers EXTENDED's non-ASCII whitespace; and a pattern's own (*LIMIT_...) says so.
The compiler checks literal patterns in Globals and Static Fields, skips one the lexer reported, and
ranks an untyped lambda by whether its body checks as each overload's Func and a lambda without a
return type by the Func's. Plural keeps a word's capitals, case styles keep combining marks,
WordWrap keeps no-break spaces and counts tabs to the next 8 columns, a glob's **/ is no folder only
as a whole one and ignoreCase folds each character to one, and a one-character fuzzy pattern takes
fzf's first word-start place. Tests: text_diff_git.fb's new git cases, text_diff.fb's 50000-line
case, text_regex, text_layout, text_humanize, text_glob, text_fuzzy additions,
panic_regex_memory.fb, and sema and pattern_tests cases.

Steps 6 and 7 on master (lane 3): s.Chars gives Chars, so master's std/time/format.fb and
duration.fb, std/math/big/rational.fb, and std/net/url.fb take Char[] (url.fb's PointsOf gives code
points as Int[] where the URL Standard counts them), sema_tests' CWChar case is `Local w: CWChar =
65` and c_integers.fb writes `'h' As CWChar`, and fb_array_push's callers in the runtime and its
tests pass a location (nullptr).

Step 13, std.io's streams: Async Methods in Interfaces, required, defaults (Final too), and Static:
the parser's next_line_is_body() takes `Async Method` as the next member; sema's
check_async_function() lets an Interface's Method be Async, check_implementation() matches
Async-ness both ways, and matches_required() doesn't box an Async Method's Struct result into an
Iterator<T>; lowering's call_interface_member() starts one through the table or the type's own
Method (start_async_member(), the Task after Self). std/io/streams.fb's `Reader` (Read into a
Buffer<UInt8>, ReadAsync), `Writer` (Write, Flush, WriteAsync, FlushAsync, the Final WriteBytes and
WriteText), and `Seeker` (Seek, the Final Position), `SeekOrigin`, and Copy, ReadAll, ReadAllText,
ReadExact, CopyAsync, ReadAllAsync, and ReadExactAsync, over the runtime's fb_string_from_bytes for
text; std/io/memory.fb's `MemoryStream`, `StringReader`, and `StringWriter`; std/io/buffered.fb's
`BufferedReader` (ReadByte, ReadLine, ReadLineAsync) and `BufferedWriter`, whose Async twins go
through what they wrap and whose Destroy writes what's kept; std/io/binary.fb's `BinaryReader` and
`BinaryWriter` in a `ByteOrder`; std.ffi's `Buffer<T>.From`. Tests:
tests/programs/async_interfaces.fb, io_streams.fb, io_async.fb, and panic_buffered_size.fb,
parser_tests' Async member, and sema_tests' Async implementations.

Step 13's review (13 findings, 3 medium) fixed, each with a test, but the doc-only ones;
docs/review/step13.md has their Status lines. When steps 5, 6, 7, and 13 landed together,
io_streams.out's Doubles became step 5's text (1e300), and math_units.out's infinity step 5's Inf.

Step 8 (lane 5's Markdown half): std.text.markdown over md4c 0.5.2, its own archive and notice, with
`MarkdownToHtml` and `ParseMarkdown`, a tree of `MarkdownNode`s with each kind's details and
`PlainText()`, and `MarkdownExtensions` (tables and task lists by default; None; GitHub's
strikethrough and autolinks); all 652 of CommonMark 0.31.2's spec examples pass as its spec_tests.py
compares them, with spec.json committed as test data; and two md4c bugs worked around, a NUL in code
and an indented code block's empty detail; the review's fixes, PlainText's blocks on lines of their
own however deep, md4c patched for a cell's `\|` and surrogates, and md4c's own rules documented.
std.text.template: Mustache-style templates in Handlebars' spelling, from a Value or a Map, with
partials, HTML escaping, and Mustache's whitespace rules; the review's fixes, padded and multi-line
block tags, `{{this.name}}` as the scope's own name, a .html partial that escapes inside a text
template, and indented partials kept apart from templates' names.

Phase 1's review, lane 2's lowering findings (P1-lowering-1 to -6) and step 18's S18-tasks-events-2:
store() puts the new value in first, through replace() (a local for the old value, the assign, then
its release), and so does a Channel's TryReceive; codegen's holds_fixed_array(), copy_bytes()
(memmove), put_zero() (memset, or each element's and field's zero), and put() build and copy a value
that holds a fixed array a part at a time, and a For ... In over one larger than max_stack_copy (64
KiB, by size_bound()) holds an array_of() copy, a T[] filled with ARRAY_RESERVE and ARRAY_ADD;
each_element() and element_count_of() take a place; global_t's `order` (lowering's m_item_order)
sets the order codegen lets go of the Globals in; outliving() copies a place through_reference()
reaches; zero_t's and mir_function_t's spans, and the evaluator's limit_place(); lane2-lowering.md's
second-batch Status lines name master's commits. Tests: store_destroy, for_in_large_fixed,
return_through_reference, release_globals_released, release_panic_global_released, cases in
globals_released, evaluate_tests.cpp, and mir_tests.cpp, whose store sequences now assign before
they release.

Step 20's formats: TOML 1.0, CSV, INI, XML 1.0 (a streaming reader with the internal subset's
entities and defaults, a tree, a writer, XmlPath, and xmltodict's Values), and YAML 1.2's core
schema, each reading into std.core's Value and writing from one; toml-test's 700 TOML 1.0 cases and
yaml-test-suite's 367 core cases pass and are committed, and the W3C XML conformance suite's 1627
applicable cases pass outside the repo, its terms not allowing a copy.

Step 20's review fixes (39): XML's entities capped at ten times the document and a MiB, and 64 deep
in attributes too; XML's trees, TOML's tables, and Values read, written, and queried however deep,
and TOML's arrays and inline tables 256 deep at most; mixed content and CDATA written back as read;
XmlPath's '|', text nodes, and string() as XPath has them; every reader's ParseError with its line
and column, a lone CR a line's end, and a byte order mark not a column; the writers panicking at
what wouldn't read back; and YAML's byte order marks, quoted characters, long keys, flows that don't
end, tags, directives, and big integers as YAML 1.2 has them.

M6 step 20's Protocol Buffers (lane 11): `Import "game.proto"` reads a .proto file, proto2 or
proto3, at compile time into a module named by its path, under its stem, through
compiler/src/proto.cpp's `parse_proto` (a hand-written lexer and parser of protobuf's grammar:
syntax, package, public and weak imports, options checked against descriptor.proto's names, messages
with nested types, every label, maps, oneofs, groups, reserved and extension ranges, extends read
past, enums with allow_alias, services with streams, and protoc's checks, each an error at its line
and column), `resolve_proto` (protoc's scoping rules over the file, its imports, and their public
imports; map keys, proto3's open enums, a map's enum starting at 0, packed, defaults by type, and
FutureBlitz names that clash), and `generate_proto`, which writes the module's source: a Struct per
message, Equatable, Printable, and Cloneable, with a New of its fields by name, Encode, Decode,
Merge, MergeFrom, Clone, IsInitialized, ToString in text format, WriteTo, ReadFrom, WriteText, and
FindMissing; presence as T?, repeated as T[], maps as Map; oneofs as Properties and an Enum of which
holds one; a message that holds itself in a ProtoBox behind a Property; proto2 defaults as
xOrDefault; an Enum per enum, proto3's @Open, a closed one's FromNumber and its aliases; and an
Interface of Async Methods per service. The driver's loader reads each .proto once, finds its
imports from its folder up to its package's and google/protobuf's in std, reports cycles, and leaves
a module empty after errors. std/data/ protobuf.fb has ProtoWriter, ProtoReader (protobuf's 100-deep
and length limits, ParseErrors at their bytes, unknown fields kept as written), ProtoText, and
ProtoBox; std/data/proto/google/ protobuf/ has protoc 36.2's eleven well-known types. The language
got `@Open Enum`, and an Extend's Static Property is reached through its Enum. tools/proto_golden.py
makes the programs' golden bytes with protoc 36.2 and checks with it the bytes they write;
proto_tests.cpp, and the programs proto_scalars, proto_messages, proto_proto2, proto_errors,
proto_imports, proto_names, panic_proto_required, panic_proto_oneof, and enums_open.

Step 26 added std.term, the console's tools. The runtime's new term.cpp has the `fb_term_`
functions: whether each stream is a terminal, the terminal's size, its colors from the environment
(`fb::term::color_level`: NO_COLOR, TERM, COLORTERM, and Windows' console), raw mode, and stdin read
with a timeout and a resize between, over Windows' console API with virtual terminal sequences, its
key and mouse records written as a terminal would send them where it gives those, and over termios
elsewhere, with SIGWINCH through a pipe and Ctrl+Z putting the terminal back while the program is
stopped. What the program changed, the screen too, is put back however it ends, and before a panic's
or an uncaught error's message, through runtime.cpp's new `before_report`. std/term/ has
console.fb's `StandardStream`, `IsTerminal`, `TerminalSize`, `ReadKey`, and `ReadPassword`;
style.fb's `TermColor` (16, 256, or 24-bit, each shown as the nearest a terminal has),
`TextAttributes`, `Style`, `ColorLevel`, `ConsoleColors`, `Paint`, `StripStyles`, and
`VisibleWidth`; input.fb's `KeyEvent`, `MouseEvent`, `InputEvent`, and `InputParser`, which reads
xterm's and the kitty protocol's keys, Alt before any of them, SGR's and X10's mouse reports, and
bracketed pastes; terminal.fb's `Terminal`, the console's for stdout or stderr, and `FakeTerminal`,
which keeps what's written and gives what it's fed, with resizes and pauses between; widgets.fb's
`Table`, `Box`, `ProgressBar`, and `Spinner`; prompts.fb's `Confirm`, `Ask`, and `Choose`, live on a
terminal and a line at a time on a pipe; region.fb's `Region` and `Constraint`; screen.fb's `Cell`,
`Canvas`, and `Screen`, whose `Present` writes only the cells that changed; and views.fb's
`TextInput` and `ListView`. Tests: tests/programs' term_style, term_input, term_widgets,
term_prompts, term_screen, and term_console (with its .in), the panic_term_ programs, and
uncaught_term_screen; and tests/runtime/term_tests.cpp, which runs tests/native's term probe in a
pseudoconsole on Windows and a pty elsewhere.

Two compiler items on their own branch. A-new-6, found by lane 7 in step 13: overload resolution
checks an array, tuple, or Map literal as each overload's parameter type, as a lone Function's call
does, so with `F()` and `F(bytes: UInt8[])`, `F([1, 2, 3])` and `F([])` take the second; a literal
of its own type is exact, two overloads that convert it are ambiguous, and untyped lambdas in one
take the parameter's Func type (step 7's S7-compiler-3 case) (sema's is_shaped_literal(), in match()
and choose_overload()). And step 32's Deferred work row: `For x In 0 Until n`, over a range written
there, counts as the For loop with those bounds does, without making a Range
(lower_range_iteration(), sharing lower_counting() with the For loop). Tests: a sema case, a mir
case, for_in_ranges.fb, and panic_for_in_range_step.fb and its unsigned twin.

Step 24, std.image I (lane 6's, finished by lane 1): third_party fetches stb_image 2.30 and
stb_image_write 1.16 (built with zeroing allocations), libwebp 1.6.0 with sharpyuv, and nanosvg,
each its own archive beside a runtime part (fb_runtime_stb with the APNG decoder, fb_runtime_webp,
fb_runtime_svg; PNG's and APNG's encoders in fb_runtime_zlib), and runtime/include/fb/image.hpp. The
runtime's own: QOI, GIF's decoder (browsers' disposal) and encoder (median cut, Floyd-Steinberg),
PNG and APNG encoders (libpng's filter heuristic), and the transforms (Pillow's resampling in
Doubles, alpha-weighted, without fused multiply-adds; crop, flips, quarter turns, draw, paste,
fill). std/image: `Image` (RGBA8, straight alpha, a UInt8[] until step 12's Buffer), `HdrImage`,
`Animation` and `Frame` (delays to 65535 s), `ImageFormat`, `ResizeFilter`, and `Png`, `Jpeg`,
`Bmp`, `Tga`, `Gif`, `Qoi`, `WebP`, `Hdr`, and `Svg`, each Decode(data, limit) an IOError of
INVALID_DATA, UNEXPECTED_END, or OTHER. Tests: image_codecs, image_tools (files from Pillow,
ImageMagick, cwebp, and others), image_transforms (Pillow 12.3's resizes within 1), image_threads,
and 34 panic_image_ programs. Lane 1's review of it: an OS/2 BMP isn't taken for RLE; stb_image's
undecoded JPEG pixels are zeros, not the heap's; GIF's dithering keeps the average; Hdr.Encode
clamps what RGBE can't hold; the animation encoders panic for a resized frame, and APNG's long
delays decode; libwebp codes on one thread at a time, as its SIMD setup races on Windows (it hung
image_threads).

Lane 3's fixes: the CLI test of the console's code page and mode no longer races other tests for the
console, and a String past its 2147483647-byte cap panics at the place that made it. Lane 8's fixes:
a Property's type is resolved once, so its errors show once, and an overloaded Function's name given
to an overloaded callee picks the overload whose Func type fits.

M6 lands on Windows from 2026-10-26 (the user's call): a stack lands once the whole of
tools/verify.py passes on it on the user's machine, and a lane checks its unit with the new
`verify.py --quick` (the unit tests and only the programs that changed); CI still runs on Windows,
Linux, and macOS, and lane 7 fixes what fails on Linux and macOS as it lands. Landing fixes along
the way: codegen numbers the Globals with std::iota (Apple's clang has no ranges::iota), std.data
reads Doubles with fast_float (Apple's libc++ has no from_chars for them), and math_units' and
io_streams' expected text is step 5's (Inf, 1e300).

Step 25, std.image II: runtime/src/draw.cpp fills regions by the nonzero rule, each pixel by the
exact part of its square they cover (rows cut where edges start, end, and cross, then trapezoids),
or by its center unantialiased, in Doubles without fused multiply-adds; ellipses as polygons within
a hundredth of a pixel, and strokes as pieces (segments, miter, round, and bevel joins, butt, round,
and square caps) filled together. The runtime reads fonts itself, every offset checked: font.cpp's
TrueType (cmap 4 and 12, hmtx, kern 0, glyf with composites) and cff.cpp's CFF (Type 2 charstrings,
every operator, subroutines, arithmetic, seac through the charset, CID fonts by FDSelect), with
glyph points bounded, and lays text out unrounded, curves as chords. std/image: `Canvas` (FillRect,
StrokeRect, StrokeLine, StrokePolyline, StrokePolygon, FillPolygon, FillCircle, StrokeCircle,
FillEllipse, StrokeEllipse, FillText; Antialias, LineWidth, `LineCap`, `LineJoin`, MiterLimit) and
`Font` (Parse, UnitsPerEm, Ascent, Descent, LineGap, LineHeight, Width, HasGlyph); stb_truetype
isn't used. Tests: image_draw and image_text match references worked out apart from the runtime, in
Fractions, byte for byte (image_text's fonts made by font.py and cff_font.py, which use every Type 2
operator), font_tests.cpp breaks the fonts (cut short, tables cut short, bytes changed), and 8
panic_image_ programs.

Step 18's rest: `Lazy<T>`; ParallelForEach, ParallelMap, ParallelReduce, and ParallelSort on the job
pool; `Subscribe(owner, handler)`, holding its owner weakly; `ThrowingTaskSource` and WhenAll and
WhenAny over Tasks that throw; `Cancellation`, which stops a Semaphore's Acquire, a WaitGroup's
Wait, Sleep, and a Channel's Receive and Send with a `CancelledError`; `Selector` over several
Channels and a timeout, with a Channel's `IsClosed()` and `TryReceive()` built in for it; std.log's
Logger, filters by module from FB_LOG, outputs, and console and JSON-lines sinks; and the job pool's
idle workers woken only for jobs, not at every Channel's change.

Lane 8's pinned test: an untyped lambda or array literal passed to an overloaded Function takes the
parameter's type where one overload fits by count (sema_tests); an integer literal past Int32
between Int64 and Double overloads stays ambiguous, as spec 05 has it on purpose.

Step 14, std.io's files, without streams: the I/O thread closes other parts' handles as it stops
(io_internal.hpp's at_io_stop(), io_t::at_stop()), and its job queue, step 28's on_io_thread(),
starts their work; runtime/src/files.cpp in fb_runtime_io, the fb_io_file_, fb_io_dir_, fb_io_temp_,
and fb_io_lock_ calls over libuv's synchronous file functions, each with an _async twin on libuv's
thread pool from the I/O thread's loop (request_t, run_async(), start_queued(), finishing a
Task<Int64>), errors numbered as IOErrorKinds (fb_io_error_kind, fb_io_error_message); path.cpp's
fb_path_windows(); watcher.cpp's fb_io_watcher_ over uv_fs_event, telling events by the names it
knows; mapped.cpp's fb_io_map_ over mmap and MapViewOfFile; runtime/include/fb/files.hpp. A String
converts to std.io's Path where one is expected (sema's find_path_struct() and makes_path(),
`analysis_t::path` and `path_new`, lowering's path_of()), spec 02. IOErrorKind's NOT_A_DIRECTORY,
IS_A_DIRECTORY, NOT_EMPTY, BUSY, NO_SPACE, and INVALID_PATH. std/io/path.fb's `Path` (Join and `/`,
FileName, Stem, Extension, WithExtension, Parent, IsAbsolute, Normalize, RelativeTo, Absolute,
NativeStyle) and `PathStyle`; request.fb's Package Request, Checked, and ErrorOf; file.fb's `File`
(ReadBytes, ReadText, ReadLines, WriteBytes, WriteText, WriteLines, AppendBytes, AppendText,
AppendLines, WriteBytesAtomic, WriteTextAtomic, Exists, Size, Metadata, Copy, Move, Delete,
SetReadOnly, each with an Async twin), `Metadata`, `FileKind`, and `LineEnding`; dir.fb's `Dir`
(Create, CreateAll, Remove, RemoveAll, Exists, Entries, Walk, Glob, with Async twins, Current,
SetCurrent, Temp) and `DirEntry`; temp.fb's `TempFile` and `TempDir`; lock.fb's `FileLock` (Acquire,
AcquireShared, TryAcquire, TryAcquireShared, AcquireAsync, AcquireSharedAsync, IsHeld, Close);
mapped.fb's `MappedFile`; watcher.fb's `FileWatcher` (Next, TryNext, NextAsync, Close), `FileEvent`,
and `FileEventKind`. Tests: tests/programs/io_path.fb, io_files.fb, io_files_async.fb, io_watch.fb,
panic_io_map_index.fb, panic_io_map_read_only.fb, panic_io_map_closed.fb, panic_io_map_range.fb,
panic_io_path_extension.fb, and sema_tests' "a String converts to std.io's Path where one is
expected".

Step 15's std part: std/io/vfs.fb's `Vfs` (Mount, MountFolder, MountEmbedded, Unmount, ReadBytes,
ReadText, ReadLines and their Async twins, KindOf, Exists, DirExists, Entries, Walk, Glob), mounts
over mounts with the last first, `VfsSource` (Read, KindAt, List), `FolderSource`, and
`EmbeddedSource` over EmbedFolder's Map<String, UInt8[]>; dir.fb's Package PatternNames, DepthOf,
and Matching, which Vfs.Glob shares; std/io/preferences.fb's `Preferences` (Get, Set, Remove,
Contains, Keys, Count, Clear, IsChanged, Save, SaveAsync, OpenAsync, Folder), over the runtime's
fb_io_dir_config(), in an escaped key=value file saved atomically. Its review's fixes: a
FolderSource is Final and has nothing outside its folder, a mount is a folder, KindOf takes the last
mount's kind as Entries does, Async reads read a snapshot of the mounts, Glob splits patterns at
'\', EmbeddedSource panics for a place that's a file and a folder; Preferences keep a change made
while saving, read a file behind a BOM or where a file blocks their folder, and refuse more app
names; and file times come from Windows itself as seconds and nanoseconds (fb_io_request_stat), past
2038 too. Tests: tests/programs/io_vfs.fb, io_preferences.fb, panic_io_vfs_mount.fb,
panic_io_vfs_embedded.fb, panic_io_vfs_embedded_both.fb, panic_io_preferences_app.fb,
panic_io_preferences_dot.fb, panic_io_preferences_device.fb, panic_io_preferences_long.fb,
io_files.fb's time past 2038, and sema_tests' "std.io's FolderSource is Final so its Async reads are
its reads".

Step 29's HTTP client (lane 9): third_party/ fetches libcurl 8.23.0 (HTTP and HTTPS only; no tool,
docs, libpsl, libidn2, libssh2, or brotli; CURL_CA_BUNDLE and CURL_CA_PATH none) over the bundled
mbedTLS's mbedtls and mbedx509, which curl's FindMbedTLS takes as targets, zlib-ng and zstd
(ZLIB::ZLIB an alias of zlib-ng), and nghttp2 1.70.0, built from its lib/ C files, for HTTP/2 over
ALPN; curl's notice has its curlx/inet_ntop.c's and inet_pton.c's ISC text.
`bundled_library_t::needs` names the bundled libraries one is built on, which bundled_libraries()
adds and fb prints, and an entry with no prefix or runtime is only needed; libcurl needs libuv,
mbedTLS, zlib-ng, zstd, and nghttp2; link_program() links a system library "Name.framework" as
`-framework Name`. io.cpp's run_on_io_thread() and at_io_stop() (internal.hpp) let another part run
work on the I/O thread's libuv loop and close its handles as it stops. The runtime's fb/http.hpp and
fb_runtime_http: http.cpp (client_t, a curl share of cookies, DNS, and TLS sessions, and a multi
handle for blocking transfers; transfer_t, refcounted between std.net.http and the I/O thread;
run_blocking(); the I/O thread's io_part_t, a multi handle over uv_poll_t sockets and a uv_timer_t,
start_on_io(), settle(), stop_on_io(); set_request(), redirect(), and origin_of(), which follow
redirects as the fetch standard does; a request's Cookie headers through CURLOPT_COOKIE;
outcome_of(), the outcomes as NetError Kinds) and tls_roots.cpp (os_roots(): Windows' ROOT store,
valid and for server auth; macOS's SecTrustCopyAnchorCertificates; else SSL_CERT_FILE or the first
of the distributions' bundle files; append_pem()), and http_text.cpp in fb_runtime
(fb_string_from_utf8, fb_headers_same_name, fb_headers_is_name, fb_headers_is_value,
fb_percent_encode), so Headers and Bodies link no libcurl. std/core/error.fb's
NetErrorKind.HTTP_STATUS. std/net/http/: headers.fb (Headers, HeadersIterator, CheckField),
status.fb (StatusCode: named statuses, Reason, IsSuccess, IsRedirect), client.fb (HttpMethod, Body
with Text, Json, and Form, HttpResponse, HttpClient with Get, Head, Post, Put, Patch, Delete, Send,
Download, their Async twins, DefaultHeaders, UserAgent, FollowRedirects, MaxRedirects, Decompress,
Proxy, TrustOnly, TrustOsRoots, ClearCookies; Transfer; Http's one-call requests;
HttpResponse.Version; Timeout and ConnectTimeout Durations). The runtime refuses text libcurl can't
take (set_text, refuse) and a redirect to a URL it can't use (OUTCOME_BAD_REDIRECT) before sending,
trusts the client's roots for an HTTPS proxy, gives one-line messages, and counts no redirect's body
in progress. tests/native/http_server: fb_http_test_server (plain sockets, HTTP/1.1 with keep-alive,
TLS through mbedTLS with its committed test CA and certificate from make_certificates.sh, HTTP/2
through an nghttp2 server session where ALPN agrees on it, an HTTPS port that offers only HTTP/1.1,
bodies compressed by zlib-ng and zstd), which runs a program as its child, with no proxy in its
environment, and its URLs and the CA as its input; program_tests.cpp and verify.py run each http_
program under it. Tests: http_client, http_tls, http_async, http_decompress, http_h2, http_progress,
http_refused, http_timeouts, net_http_headers, the panic_http_ programs, tls_roots_tests (RFC 4648's
vectors through append_pem, and the OS's roots read by mbedTLS), driver_tests' libcurl and notices
cases, and program_tests' environment-proxy case. docs/review/step29.md has the review.

Steps 14's and 15's reviews (docs/review/step14.md, step15.md) and step 29's client review
(docs/review/step29.md) were fixed before they landed, each finding with a test; the landing stack
merged step 29's tests' list with the stack's, and kept the I/O thread's at_stop() with its m_stops
under one mutex in place of step 14's own stoppers.

The step 32 audit's std.text and std.compress gaps (lane 3): a StringBuilder is a std.io Writer,
whose bytes it adds as text, and its Flush throws past a String's most bytes as its Write does;
Humanize.Relative of a Duration, and FormatBytes and FormatDuration. Each codec's compressor and
decompressor is a coder, which a CoderWriter and a CoderReader stream std.io's bytes through, and a
CoderReader whose coder throws doesn't give what it gave again. Zip's and Tar's entries keep when
they were last changed and their modes, which their writers take: a Zip's DOS dates are read and
written as UTC, its NTFS, Info-ZIP, and PKWARE times read, and a GNU Tar's base-256 times before
1970 too. Its review's five findings were fixed, each with a test (docs/review/lane3-gaps.md).

Step 8's rest, template format specs (lane 5): a VALUE tag takes a format spec after its first ':',
as interpolation's does, `{{price:.2}}`, `{{{x:spec}}}`, and `{{& x:spec}}`, by the value's kind; an
integer whose Format doesn't take the spec falls back to a Double's, every digit kept past 2^53
(fb_format_exact); a spec no kind takes, or one with a brace, is BAD_TEMPLATE when the template is
read, and BAD_VALUE says the template, line, and column. Its review's two findings were fixed
(docs/review/template-specs.md).

Step 17's compiler part: the built-ins `Load<T>(value)` and `Save(x)`, which convert between
std.core's Value and any type made of numbers, Bools, Strings, Enums, T?s, arrays, Maps by String,
Values, and Structs and Classes, by compile-time reflection. Load makes a Struct or Class without
its New, then runs its OnLoaded(). std/core/load.fb's @Rename, @Skip, @Optional, @Flatten, and
@Renamed adjust a Field, and Loadable and Saveable let a type convert itself. ParseError gets
MISSING and a Path, which its Message starts with, and a Load error's Place is the call's. Lowering
builds one Load and one Save per type over std/core/load.fb's helpers. Load and Save go 256 levels
deep. @Serialize became an error that says neither needs it.

Load and Save's review (docs/review/load.md) had 23 findings, each fixed with a test: a half-made
Class is let go of without its Destroy (OBJECT_DISCARD), instances follow analysis's rules
(fields_problem()), a @Flatten Field's keys clash with others', Enums and @Flags keep their values,
Save's helpers hand their errors back so Save panics at its call, integer text past every integer
type is OUT_OF_RANGE, and Load<Bool> takes 0 and 1, as SQLite keeps a Bool; then, by the user's
choice, a Field the Object leaves out takes its default, else its zero, else it's MISSING, and
@Optional is gone.

Step 28's second half, std.net's sockets: runtime/src/net.cpp (fb_runtime_io's, linked for the
fb_io_ prefix) keeps each socket's libuv handle on the I/O thread (socket_t, on_io_thread jobs),
blocking callers waiting on the socket and Async ones on a Task<Int32> (waiting_t, with a uv timer
for a timeout) before the fb code takes what came; streams read ahead 64 KiB into a buffer of their
own, Close returns once libuv closed the handle, and close_all closes what's left before the I/O
thread stops. std/net/socket.fb's StreamSocket and ListenerSocket, under tcp.fb's `TcpListener` and
`TcpStream` (Reader and Writer, ReadTimeout, ShutdownWrite, Nagle off), udp.fb's `UdpSocket` and
`Datagram` (broadcast, multicast), and ipc.fb's `IpcListener` and `IpcStream` (named pipes and Unix
sockets, a left-behind socket file taken over), each blocking and Async, a refused connection a
NetError of REFUSED, a wait past its timeout a TimeoutError, and a stream's errors IOErrors with the
NetError as Cause. lan.fb's LAN discovery over mDNS and DNS-SD: `LanAnnouncer` (a responder for PTR,
SRV, TXT, A, and the type enumeration, one-shot and QU queries by unicast, announcements and a
goodbye) and `Lan.Find`/`FindAsync`, through each interface (fb_io_net_interfaces,
fb_io_net_multicast_interface), with `Lan.LocalAddresses` and `HostName`. lower.cpp's zero_value()
gives a Ptr Null, so a Function that throws can give one. Tests: net_tcp, net_udp, net_ipc,
net_connections (1000 connections, 100 at once, no leaks), net_lan, panic_net_ipc_name,
panic_net_ipc_empty, panic_net_lan_type, and unsafe.fb's Checked. Its review's fixes: the I/O thread
blocks SIGPIPE, so a write to a reset peer on Linux throws RESET; a backlog below 1 and a multicast
TTL outside 1 to 255 panic; streams read from when they connect and UDP sockets from their bind, so
a zero timeout polls, past the 64 KiB read-ahead too; a Write after ShutdownWrite is CLOSED; an IPC
listener holds a lock file beside its Unix socket in place of the connect probe, which gave the live
listener a phantom connection, a file there that isn't a socket stays, names are lowercase on every
OS, and a path too long for sun_path goes in /tmp; Send throws past 65507 bytes on every OS; LAN
discovery gives each interface's address alone (fb_io_net_interfaces gives netmasks), puts a
LanService's sender first, leaves out a query's known answers, says goodbye to the service's own
records only, caps the text at 8192 bytes, and takes keys whatever their case. Tests:
net_lan_packets, the runtime's net_tests.cpp (POSIX), more in net_tcp, net_udp, and net_ipc, and
eight panic programs.

Partial type-argument inference (for lane 5's std.db Query<T>): a call writes a generic Function's
or Method's first type arguments and infers the rest from its arguments, a lambda's parameters
getting what's written, and a function value from the Func expected (resolve_arguments()'s
`leading`). What can't be inferred is named, with an example that keeps what was written. Written
type arguments of an Extend of T[]'s Method are its own, not the array's T, and a call that can't
infer its type arguments no longer tells its lambdas again that they need types. Tests: a sema case,
"generic mistakes"' Pair<Int>, and tests/programs/partial_type_arguments.fb.

Its review's four fixes: a lambda's own errors still show in a call that can't be inferred
(check_uninferred()), and arguments that don't bind are reported first; an overload or function that
can't take the type arguments written doesn't fit (rank(), check_written_arguments(), "no overload
of 'G' takes 3 type arguments"); and a function value's example writes them all.

Step 15's compiler part: the built-ins Embed, EmbedText, and EmbedFolder (spec 13's "Embedding
files"). Sema's check_embed() and embed_reader_t, whose file and folder callbacks are loader_t's
embed_file() and embed_folder(), with embedded_path() keeping a path in its package's folder as a
quoted Import's is (links, nested packages, std), a 64 MiB limit per call (max_embedded), and hidden
files left out; analysis_t::embeds and expr_info_t::embedded hold what each call puts in the
program, embedded_file_t its path and bytes. EmbedText folds to a String constant, checked as UTF-8.
Lowering's lower_embed() builds the UInt8[] from a String constant, and EmbedFolder's Map through
new_map() and add_to_map(), which the Map literal shares. compilation_t::embedded lists the files,
which `fb build -o` won't write over. Tests: tests/programs/embed.fb and its embed/ folder, a driver
test of every error, a disk test of links and nested packages, a MIR test of non-UTF-8 bytes, and a
CLI test of -o.

Embed's review's six findings were fixed (docs/review/embed.md): a file is read once and its bytes
held once, in one Global per file that every embed of it shares, and EmbedFolder finds code where it
really is, refuses links and packages in its folder, and warns at other letter cases. The landing
stack merged Embed's sema with Load and Save's (both sides' declarations, and the built-ins'
examples together), and lane 11's fix to NetErrorKind's ordinals, where step 29's HTTP_STATUS met
the sockets, came with them.

Step 29's TLS half (lane 11): std.net.tls over mbedTLS 4.1.1. The runtime's fb_tls_ functions
(runtime/src/tls.cpp, fb/tls.hpp, the archive fb_runtime_tls, a bundled entry of mbedTLS's without
notices of its own) run TLS through memory buffers, so TlsClient's Connect and TlsServer's Accept,
with Async twins, give a TlsStream<S> over any stream that's a Reader and a Writer, and TlsListener
takes TLS over TCP; a client trusts the OS's roots, read once by tls_roots.cpp, or roots of its own,
checks the host name and IP SANs, offers ALPN, and can give a client certificate. A handshake fails
with a NetError of TLS that says why. Its review's 18 findings were fixed (docs/review/tls.md): on
Windows a chain the ROOT store doesn't cover is checked as Windows checks it (os_verify), for
libcurl too through CURLOPT_SSL_CTX_FUNCTION; key usages and PSA's statuses have their own messages;
an address gets no server_name; a listener's timeout covers the handshake; a failed handshake's
alert is flushed; a write after Close throws; and hosts and ALPN lists past their limits panic at
the caller. Tests: net_tls, net_tls_tcp, tls_tests, tls_roots_tests, and panic programs.

Step 29's Download to a file (lane 9): files.cpp's atomic write became fb::runtime::atomic_file_t,
which fb_http_to_file opens before the transfer, so an unwritable target is an IOError with nothing
sent; on_data writes a 2xx's body to it as it comes, and the transfer commits it, renaming over the
target, only on success. HttpClient's and Http's Download(url, path, progress) and DownloadAsync.
Its review's finding, an empty path, was fixed. With TLS, step 29 is done.

Step 31, std.net.game (lane 12): reliable UDP for games in std/net/game/*.fb, with no runtime C++:
packet.fb's wire format ("FG", a version, CONNECT, ACCEPT, REFUSE, DATA, DISCONNECT; 16-bit
sequences compared as RFC 1982 does); link.fb's GameLink, which a UdpSocket is through GameHost.Bind
and any Class can be, as the tests' lossy network is; channel.fb's RELIABLE_ORDERED,
RELIABLE_UNORDERED, and UNRELIABLE channels, fragments, and a window of 1024; settings.fb's
GameSettings and DisconnectReason; peer.fb's GamePeer, with an ack bitfield, RFC 6298's RTT and RTO,
loss stats, and resends that back off; and host.fb's GameHost, with its handshake, Broadcast,
Update(now), Service, and ServiceAsync. Its review's 15 findings were fixed (docs/review/step31.md):
parts refused for MaxBuffered stay unacked and a sender keeps to its own, each channel keeps its
leftovers, keepalives time round trips, MaxPeers counts incoming peers, simultaneous connects
finish, unreliable ids survive outages, and Broadcast's and GameSettings' checks panic at the
caller. Tests: net_game (a seeded lossy link), net_game_udp, and panic programs.

Steps 8's and 23's rest (lane 5): a template renders a Struct, a Class, or a Map through what Save
makes of it, panicking at the caller past Save's 256 levels; std.db's Query<T>, of no values or one
to eight, on a Database or a Statement, reads each row as a T through Load<T>, a Bool from 0 or 1
and a missing column at its Field's default or zero, its type argument inferred where the values say
it. Its review's four findings were fixed (docs/review/template-query.md). With it, steps 8 and 23
are done.

std.image over a Buffer (lane 1): an Image's pixels are a Buffer<UInt8> and an HdrImage's a
Buffer<Float32>, with Pixels and Over, and the runtime's pixel functions (draw.cpp, image.cpp, the
codecs, transform.cpp) take pointers; Paste and Draw between images that share memory draw from a
copy; Image.Load, Font.Load, and their Async twins read files through std.io. Its review's finding
was fixed (docs/review/image-buffer.md).

Step 17's std.os part (lane 4): std/os/env.fb's Env (Get, Set, Unset, All, Expand, and Load, which
sets what a .env file names), system.fb's System (Platform, Arch, CpuCount, TotalMemory, Hostname,
UserName), folders.fb's Folders (Executable, Home, Config, Data, Cache, Temp, Documents) and
OpenDefault, and process.fb's Process.Id and Exit, over libuv through the runtime's fb_os_ functions
(os.cpp), whose environment lock starting a process will share. Lane 7 made value.cpp read doubles
with fast_float, as Apple's C++ library's from_chars doesn't, and the stack merged TLS's bundled
entry with std.os's renamed `prefixes` and TLS's http.cpp with Download's.

Step 32's audit's std.io gaps (lane 10): `File.Open`, with `OpenOptions`, and `File.Create`, each
with an Async twin, give a `FileStream`, a Reader, Writer, and Seeker over libuv's positional reads
and writes, with `Seek`, `Size`, and `Sync`, holding nothing back; misuse, a call while an Async one
is under way, and options no file opens with panic at the caller, and Windows' Truncate without
Create goes through ftruncate. `MappedFile.Bytes()` gives a `Buffer<UInt8>` that holds the map until
it goes, a read-only one's a copy-on-write view of its own, and Buffer's view New is Protected for
it. std.compress's `ZipSource` and `Vfs.MountZip` mount a zip, without links or names above its
root. Preferences get and set Int64s, Doubles, and Bools through step 5's Parse. std.log's
`FileSink` and `RotatingFileSink` add each line in one append, under a lock, moving a full file to
`.1` and on, keeping `keep`.

The std.io gaps' review's eight findings were fixed (docs/review/io-gaps.md): Buffer's view New is
Package, so no Class outside std can view memory it doesn't own; File.Create and File.WriteText
empty a file with ftruncate once it's open, so Windows' hidden files take them; RotatingFileSink
keeps the old files while the current one can't be moved aside; a FileStream's read near Int64's top
gives nothing and a write past the file system's limit is NO_SPACE; and the sinks resolve a relative
path once.

- `HttpServer` (`Listen`, `LocalAddress`, `Serve()` until `Stop()`, `IsRunning`, `MaxBodySize`,
`IdleTimeout`), `HttpRouter` (`Get`, `Post`, `Put`, `Patch`, `Delete`, `Route(method, ...)`, `Use`,
`Group`), `HttpRequest` (`Verb`, since Method is a keyword; `Target`, `Path`, `Query`, `Headers`,
`Bytes`, `Text`, `Version`, `RemoteAddress`, `Param`, `Params`, `Form()`), `HttpHandler` and
`HttpMiddleware` (Aliases of Funcs giving `Task<HttpResponse>`), and `HttpResponse.Of(status,
body)`, `Of(body)`, `Redirect(url, status)`: a handler gives step 29's own HttpResponse, as Deno's
and the Fetch API's do. - Paths are `:name` and `*rest` (Express's), since `{name}` in a string is
interpolation. - HTTP/1.1 per RFC 9112: keep-alive and pipelining, HTTP/1.0's keep-alive,
Connection: close either way, one Host or a 400, Expect: 100-continue (and 417 for another),
absolute-form targets, OPTIONS * (a 204), 404 and 405 with Allow, HEAD through GET's route without a
body, Content-Length/Date/Connection set, lingering close after a refusal, idle timeout. -
`WebSocket.AcceptAsync(request, protocols)` takes a handler's connection over; Stop leaves it. -
Tests: http_server (routes, params, groups, middleware order and short-circuit, bodies, form, query,
headers, redirects, 204, keep-alive and pipelining by hand, every refusal), http_server_websocket,
http_server_many (300 requests through one client, 200 through a client each fifty at once, 100
WebSockets one after another and 50 at once: no leaks), and 12 panic programs
(panic_http_route_{path,twice,rest,name,method}, group_{rest,gone}, listen_twice, not_listening,
max_body, redirect, panic_ws_accept_twice). All 20 times each way by hand.

Step 30's server half (lane 6) is HTTP/1.1 per RFC 9112 in std/net/http/wire.fb, HttpServer,
HttpRouter, HttpRequest, HttpHandler, and HttpMiddleware in server.fb, and std.net.ws's WebSocket
per RFC 6455 in std/net/ws/, which connects and accepts over TCP or takes over a handler's request.
Its review's 19 findings were fixed (docs/review/step30.md): a route that matches the whole path
beats one whose *name matches none of it; framings a proxy could read another way (an empty
Transfer-Encoding or Content-Length, a bare LF or a bad extension in a chunk, Transfer-Encoding in
HTTP/1.0) are a 400; a response's status is a final one and a redirect's a redirect's; two Hosts are
a 400 in HTTP/1.0 too; leading zeros and a later 1.x minor version are read as RFC 9112 has them;
lingering takes a second in all; and a WebSocket's 405 has an Allow, a Content-Length of 0 isn't a
body, and a close that isn't answered, or a failure, throws with 1006.

Step 30's static files (lane 6): HttpRouter.Files(prefix, folder) serves a folder's files for GET
and HEAD, an index.html at a folder's path, a Content-Type by extension, an ETag and Last-Modified,
304s for If-None-Match and If-Modified-Since in HTTP's three date forms, and a 206 for one range.
Its review's seven findings were fixed (docs/review/files.md): Windows' 8.3 short names and links
out of the folder are 404s, a redirect keeps its raw query, a range unit can be in any case, and a
file past 2 GiB is a 500.

Step 19, std.data's JSON and MessagePack (lane 9, finished by lane 13): the runtime's JSON tokens
(fb_json_reader_new, fb_json_next, and the rest, in runtime/src/json.cpp, with exact integers and
fast_float's Doubles), and std/data/json.fb's `JsonReader`, `JsonWriter`, and `JsonToken`, and
`Json.Read(text)` and `Read(bytes)` of a Value and `Json.Write(value, indent)`, which go through
arrays and objects with no call for each; std/data/msgpack.fb's `MessagePack.Read` and `Write` of a
Value, in the fewest bytes. `Json.Read<T>`, `Json.Write(x)`, `MessagePack.Read<T>`, and
`MessagePack.Write(x)` read and write any type through Load<T> and Save. JSONTestSuite's parsing
cases are in tests/programs/data_json_suite. Overloads may now differ by their count of type
parameters (sema's declaration checks and `rank()`, which a call's written type arguments narrow),
and Load reads a String of base64 as a UInt8[], so bytes round-trip through JSON.

Step 19's review's ten findings were fixed (docs/review/step19.md): overloads by count of type
parameters work in an Extend of T[], through inheritance and Interface defaults, and as function
values; Load's base64 comes before one Value alone for a UInt8[]; JSON's places count on from the
last one; MessagePack counts an empty array or map toward its depth; and step 19's milestones row
and decisions say what was built. With it, step 19 is done.

Step 30's ACME (lane 11): std/net/acme.fb's Acme (Jwk, Thumbprint, KeyAuthorization,
CertificateRequest, a P-256 PKCS #10 CSR) and std/net/acme_client.fb's AcmeClient (RegisterAsync,
ObtainAsync, through AcmeTokens, with nonces, a badNonce retry, and Retry-After polling) over
std.net.http's client; tests/native/acme_ca, a CA as Pebble is, which the HTTP test server hosts.
Its review's twelve findings were fixed (docs/review/acme.md).

Step 17's process half: std/os/process_command.fb's `Stdio` (INHERIT, PIPED, NONE), `ProcessOutput`
(Status, Stdout, Stderr, Text, ErrorText), and `Command` (Arg, Args, Env, Unset, ClearEnv, Folder,
Stdin, Stdout, Stderr, Spawn, Output, OutputAsync, Status, StatusAsync; no shell; a NUL, or a
variable's name that's empty or has an '=', panics at the caller), and `Process.Arguments()`
(CommandLineToArgvW, _NSGetArgv, /proc/self/cmdline); process_child.fb's `ExitStatus` (Code: Int64?,
Signal: Int?, Success), `Child` (Id, Wait and WaitAsync, which close a piped stdin first, TryWait,
and Kill, signal 9 on every OS), `ChildInput` (a Writer) and `ChildOutput` (a Reader), and
ProcessError (IOError kinds from libuv's errors, by a Select); process_signals.fb's
`Process.OnInterrupt` and `Process.OnTerminate`, each a Sendable Func() run on a Thread.Start thread
of its own that waits in fb_os_signal_wait for a count past what it saw. runtime/src/process.cpp's
fb_os_process_ and fb_os_signal_ functions run on a libuv loop of their own (processes_t, in
fb_runtime_io); a pipe is read only while a read waits, or drained (Output's stderr);
fb_os_process_spawn takes `fb::os::environment_lock()` (out of os.cpp) and builds a changed
environment from uv_os_environ, case-insensitively on Windows; SIGPIPE is blocked on its thread.
`fb::runtime::at_code_end()` and run_code_end(), which fb_scheduler_finish calls once the coroutines
are done, end the signal waits before fb_pool_finish. Tests: os_process (the program runs itself as
its child), os_signals, panic_os_command_nul and panic_os_command_name, and the CLI test of
Ctrl+Break on Windows and SIGINT and SIGTERM on Linux and macOS.

Step 17's Args (lane 8): For Field over a T? goes over its T's Fields; std/os/args.fb's
Args.Parse<T> fills a Struct through Load<T>, with @Program, @Positional, @Option, @Flag, and
@Subcommand, generated help and errors, and ArgsParser, the builder underneath. Its review's eleven
findings were fixed (docs/review/args.md), and the built-in Defaults<T>() (spec 13) gives Save's
form of a T at its defaults without OnLoaded, which the help reads. With the process half and its
review's fixes (docs/review/step17-process.md), step 17 is done.

Step 30's Workers (lane 6), as the user chose: Async handlers stay on the main thread, and
server.Work routes take a Sendable Func(WorkRequest): HttpResponse, which runs blocking on one of
Workers = n threads while the main thread awaits it; a WorkRequest is a Sendable copy of the
request, its Params too, and MaxQueuedWork (1024) calls wait before a 503. Two compiler changes came
with it: any thread reads a Static ReadOnly Field of a Sendable type, as it does a Sendable Const,
and a Struct's New starts a Func Field at a null function value. Its review's five findings were
fixed (docs/review/workers.md): the work threads last while the connections a stopped server answers
do, and a non-Sendable Static ReadOnly says its type is why.

The compiler part of step 30's Routes<T> (lane 2), as the user chose: a typed `For Method m: M In T`
whose Where line is a Func type can name type parameters of its own, which each Method that fits
binds, so the copy's body sees that Method's own parameter type, checked as generic code over it;
Async Methods count when the Func gives a Task (spec 13). Its review's nine findings were fixed
(docs/review/for-method.md): sema and lowering share one Method list (overloads and an instance's
Extend members included), a Method bound is met only by a loop of its Func type, nested loops see
the outer ones' type parameters, and the Where lines hold in the Func type.

Step 32's integration program (lane 12), tests/programs/readings: it fetches readings as JSON over
HTTPS from a feed of its own, keeps them in SQLite, sums them in SQL, draws a PNG chart, and serves
them as JSON, static files, and to its own WebSocket client, with no internet. Its review's seven
findings were fixed (docs/review/readings.md): a chart of a flat line, of nothing, or of hours
apart; a replace that really changes a value; honest labels; the WebSocket's close awaited; and
std's Hex and == in place of its own. Lane 7's fixes came with it: net_tests count HTTP_STATUS, the
nested-overloads sema test allows 30 seconds on Linux's CI, and CI gives a job three hours, as
macOS's runner needs more than two.

The parallel verify (lane 3): tools/verify.py builds and runs the test programs --jobs at a time,
each run in an empty folder of its own, with builds cached between steps, a run past 600 s failed
and its whole process tree killed, and --quick checking changed release_ programs and changes in a
program's own folder; the programs ctest test is 32 shards (FB_PROGRAM_SHARD), each with a 20-minute
limit. A full verify on the 32-core machine went from about 78 minutes to about 10. Its review's
four findings were fixed (docs/review/parallel-verify.md).

Step 30's HTTPS (lane 11): HttpServer.ListenTls(address, TlsServer), through a Package TlsDuplex in
wire.fb, each handshake in its connection's own Answer with IdleTimeout over all of it; wss:// for
WebSocket.Connect and ConnectAsync, which take `tls: TlsClient? = Null`; TlsServer.Identify, for new
connections only; and automatic HTTPS, ListenAutoAsync(acme, names, folder, https, http) with
AcmeClient.InFolder(directory, folder), which keeps account.pem and certificate.pem (key and chain
in one atomic file) in the folder, answers HTTP-01 on the HTTP side and 308s the rest (a 503 with
Retry-After until HTTPS listens), renews when a third of the lifetime is left, at start too, checked
every RenewalCheck, or at once through RenewAsync, and logs a failure through std.log while
RenewalError and CertificateNotAfter say it. Its review's nine findings were fixed
(docs/review/https.md). Step 32's program moved to it: its feed and site serve over ListenTls and
its WebSocket client connects with wss://, stopping its servers on every way out
(docs/review/readings-tls.md). Lane 9's test holds two keep-alive connections to the HTTP test
server and asks on a third.

Step 30's Routes<T> (lane 6), as the user chose: router.Routes(instance) adds a route for each Async
Method with @Get, @Post, @Put, @Patch, or @Delete that takes nothing, the request, one Struct or
Class, or that and the request; the argument is Load<A> of an Object of A's Fields from the path's
parameters, then the query, by name or @Rename in any case, with a @JsonBody Field read from the
body's JSON, @Flatten and @Renamed Fields too, and a Field that doesn't load is a 400 naming it; an
attributed Method of another shape, one that throws, or an argument Load doesn't make from an Object
panics at Routes, and Static and ByRef Methods aren't routes. Its two reviews' eight findings were
fixed (docs/review/routes.md, routes2.md). With it, step 30 is done.

For Method's picks (lane 2), as the user chose: `Where m Has Get Or Post` keeps only Methods with
one of those Attributes, so the rest get no copy and aren't checked, and a typed loop takes throwing
Methods where its Func type throws, `Func(A): R Throws E` and `Func(A): (Task<R> Throws E)`, a Func
type's Throws following a header's rules with or without a result (spec 13). Its review's ten
findings and two from lane 6 were fixed (docs/review/picks.md): a pick must match to vouch for
another loop, an override always takes its base's place, a nested Func value can't throw, a Callback
can't, and a thrown E needs no caller's say.

The last fixes (lanes 7, 11, 2, and 5): lowering read an If's, ElseIf's, While's, or Until's
condition from inside the statement's temporaries after releasing them, so `If Await x Then` read a
freed Task, which macOS's malloc reuses at once (net_tls hung on macOS's runner); lower_condition
copies such a condition into a local first (mir_tests, async_conditions). Windows' OS roots are only
those its SSL policy trusts for servers, so mbedTLS and libcurl trust none Windows distrusts from a
date or by its properties, and the OS-trust test asks Windows' own policy about each root. CI
cancels an older run on master too, and http_files' POSIX branch compiles. The five Deferred work
rows planned for M6 steps 11, 13, 18, 19, and 32 were already done and left the table. stdlib.md's
TimeoutError, easing, and LAN discovery say what was built.

## M5, modules, packages, FFI, and bindgen

Done: M5 (modules, packages, `project.toml`, FFI, bindgen), planned 2026-10-18 and finished
2026-10-20, in 11 steps, then a review of each project (compiler, runtime, cli, std).
Step 1, module scopes: compiler/include/fb/module.hpp's `module_t` (name, file, is_main)
and `module_table_t` (of(), is_main(), main()), which `ast_t::modules()` holds and the driver fills,
naming each library file by its path with module_name() ("std.core.error") and the main file by its
stem; `ast_t::outside_main()` replaces from_library(); sema's `m_modules` are `module_scope_t`s
(their own `names`, and `imports`: every module but the main one, for now), with `m_builtins` after
them; declare_in_module() replaces declare_in_file() and still refuses a built-in's name with the
same parameter types; top_level(name, span) finds a name from the module of the file it's written in
(module_of()), a module's own type or variable hiding imported ones and its Function hiding an
imported one of the same parameter types (same_signature(), which lines up generic ones' type
parameters in order and compares what their constraints ask, requirements(), and pairs declared
before every Interface knew what it extends, `m_unsure_overloads`, again in check_overloads();
Methods keep same_parameters()), imported Functions not hiding each other, so a call or function
value (check_function_value()) that two fit equally is ambiguous, and report_ambiguous() reports a
type or variable that two imports give, once per place (`m_ambiguous`, only for what reaches
`m_reported`, not an early run's scratch bag), and again after quietly_keeping_ambiguous()
(`m_quiet_ambiguous`), which an Extend of one instance and type_attribute() try names with;
std_type() finds Error, the built-in Interfaces, and Map in std.core's and std.collections.map's own
names, so a program's own Error, Map, or Stack hides std's only for its code
(tests/programs/own_names.fb); `type_t::module` (sema's qualifier_of() at each declaration) and
`type_table_t::qualified_name(type, main)` name types in MIR and LLVM, lowering's qualifier_of(),
name_start(), and function_name() start functions and Globals outside the main module with their
module ("lib.a.Make()", "std.collections.map.Map<Int32, String>.Add(Int32, String)"), and
`mir_module_t::main_module` names the main module; lowering's laziness (function_of(), use_class(),
lower_library()) keys on outside_main(); function_class() puts a function value's Class in its
Function's module with qualified parameter types ("lib.a.Make(lib.a.Node) as a value"); every
message showing two or more types uses name_of(type, other), name_of(type, { others }), or
names_of(types) over name_among() and alike(), which add each module (qualified()) when names match
or the types are, or are made of (declared_parts()), two declarations of one name, signature_text()
compares with the Methods that don't match, and std_name(type, beside, span) qualifies std's Error
and Printable where the name means something else in the code, in Throw's, Throws', Catch's, and
strings' messages (tests/programs/own_interfaces.fb declares each built-in Interface's name);
unsendable() gives an `unsendable_t` (the `held_part_t`s on the way, and a reason), field_misfit()
a `misfit_t`, and a `main_only_t` keeps its type apart (`type`, `rest`), so reason_text(),
misfit_text(), and code_name(code, shown) name their types among the message's; declared_here()
asks whether a variable, the Method's type's member, or a top-level name has a name without
reporting an ambiguous one, for in_place_maker() and spawner(); `fb_class_t` and
`fb_interface_t` gained `full_name`, which the runtime's shown_name() uses in leak reports (one
message: report_cycles() takes every leaked Class) and type panics when its alike() finds two of one
name, or two declarations of one name among what each is made of (declared_parts(), the words_of()
its full name spells with modules).
Step 2, `Import`, `From`, and qualified names: `import_stmt_t` (target, quoted, target_span, last,
alias, from, names) from parse_import() and parse_import_target() ("From" is a keyword only where it
starts a line before a module; KW_IMPORT's "not supported yet" and `unsupported_t::LINE` went),
`named_type_t::qualifier` ("C.Map<K, V>", parse_named_type()), `type_stmt_t::base_qualifier`, '.'
in type_arguments_follow(), and "(import ...)"/"(from ... Import ...)" in dump_ast; the driver's
`loader_t` loads the main file, std/core/, std/collections/map.fb, and what imports reach from a
`file_set_t` (compile()'s `library_file_t`s in memory, or compile_file()'s disk, from the main
file's folder), once per key_of() path or same_file() (equivalent() on disk, for a path that
differs only in letter case), reporting an Import that finds nothing (report_missing())
and two files of one module name; the pure resolve_import(target, quoted, importer) gives
`import_paths_t` (the file, and a dotted name's folder, which folder_files() lists); add_modules()
fills the table in initialization_order() (std/core/, std.collections.map, then depth-first from the
main module, last), each `module_t` with its `module_import_t`s (the unwritten std/core/ one first,
then each line's statement and modules), and `ast_t::sort_items()` follows it; add_std_names()
parses the std files the program doesn't load for `std_name_t`s (`module_table_t::std_home()`).
Sema's declare_imports() makes each module's `import_scope_t`s (modules, unqualified, From names,
alias) and its qualifiers, MODULE symbols (`symbol_t::modules`, declare_qualifier()), and reports
misplaced lines (report_misplaced_import(), and check_stmt() for one in a block); check_from_names()
checks From's names; top_level() goes through the module's names, its qualifiers, givers_of() (the
imports that bring a name in unqualified), and the built-ins, and in_module() looks after a
qualifier; qualifier_named() and qualified_name() (`qualified_t`) find "collections.Stack", which
check_qualified() (with check_variable(), split from check_expr()), lookup_callee()'s callable(),
type_named() and enum_named() (written_name()), and resolve_qualified_type() and named_type()
handle, marking `expr_info_t::qualified`, which lowering's lower_expr() and lower_place() treat as
the name alone; type_symbols() serves resolve_interface(), constraint_only(), extension_target(),
resolve_argument(), and declare_base() (report_not_in_module()); report_undeclared() and
import_hint() (home_of()) say
which module has a name ("'Stack' is in std.collections, which this file doesn't import", or how
an alias or From leaves it out); visible_from() shows an Extend's members only where
imports_module() finds its module: members_of(type, name, from) skips what code at `from` doesn't
see before a member further down hides one, and one type's members hide none of each other's;
members_here() and members_seen() (which own_members() uses, reporting only for lookup_value())
drop imported ones that the code's own module's take the place of (prefer_own_extensions(),
extensions_clash()) and report a Property two imports add (report_ambiguous_member(),
module_list()), leaving same-typed Methods to the call;
check_inherited() ignores unseen Extends; resolve_operator() takes an Extend's Operators only where
seen, saying which module has a hidden one, but == everywhere, and add_member() lets two modules'
Extends add one name except Operator ==; report_hidden() names the module; implementation_of()
and property_implementation() take what code at the Interface's listing sees, through
members_seen(), reporting two imported modules' same-typed Methods there (report_clashing()) and
naming an unseen Extend's module (unseen_hint()), and iteration_of() takes the Iterate the loop's
file sees; check_assignable()
reports a function, type, or module, as in `hud.Draw = f`; check_read() names the module of a
Global that initializes later; means_another() in name_among(), with `m_file` set in check_any(),
check_stmt(), the initializers, and for declarations by each_item() and in_file() (link_class(),
conform() per listing, link_constructors(), check_throws(), check_pending_bounds(),
check_worker()), qualifies a type whose name means another there, and shown_name(type) asks it
for every message showing one type (name_of(type) is the written name). std/collections/set.fb
imports std.collections.array and .map; collections.fb, generics.fb, own_interfaces.fb, and
own_names.fb import std.collections; the program runner passes tests/programs/<name>/'s files
(program_files()), for tests/programs/modules.fb (whose Classes extend S.Shape and S.Box<String>),
module_extends.fb, and panic_module.fb.
Step 3, visibility: `visibility_t` (PRIVATE, PROTECTED, PACKAGE, PUBLIC, in that order; spelling())
and `modifiers_t::visibility`/`visibility_span`, which parse_modified() takes among the other
modifiers (take_visibility(), visibility_of()), parse_visible() for an Enum or `Public Attribute`
(whose Fields get its visibility), parse_attributed() after attributes, and parse_enum() refuses on
a member; `unsupported_t::PREFIX` went, and dump_ast shows a written one; the driver's add_modules()
gives each `module_t` a `package_id_t` (`module_table_t::add_package()`, `package_name()`: "std",
and the main module's name for the program's files), and declared_names() gives std hints only
Public names. Sema's has_visibility() (declared symbols, News, and the defaults take_default()
copies, which keep their Interface's visibility even where an Extend gives a Public type a private
Interface, a default Operator but == too, which resolve_operator() and implementation_of() check;
not Interface members or other Operators), sees() and seen_names()
for top-level names (givers_of(), top_level(), in_module(), home_of(), and check_from_names() bring
in only those), can_use(declared, span, through) for every declaration (Private: the file; Package:
package_of(); Protected: class_at() a Class whose declaration holds the use, `m_class_spans`, that
is_or_extends() the owner, through Self, Super, or one of its own objects, or Static), and
unusable_text()/report_unusable()/label_declaration() ("'Entries' is private to
std/collections/map.fb", a Struct's constructor through least_visible_field()); members_seen(type,
name, from, use, through) drops what code can't use, recording it in `m_dropped` (`dropped_t`) with
usable_constructors() and drop_unseen(), so check_expr() of a call reports one that
only_unusable_fits(); members_here() and lookup_member() pass the object's type, or the code's Class
for Super, report_hidden(), unseen_hint(), unusable_own_member() (in report_undeclared()),
report_not_in_module(), and report_hidden_name() (attributes) say why, and import_hint() gives a
`hint_t` (text, declared) that report_hint() labels; callable() and lookup_super() filter News,
link_constructors() checks base_new, and inherit_constructors() takes only usable News;
check_not_protected() (top level, Structs', Extends' members), check_interface_visibility(),
check_operator(), and check_variable_modifiers(node, top_level) (Locals, and Consts in a block)
refuse modifiers where they don't go, and Interface members and Operators take their type's
visibility; the New a type gets has the type's, a Struct's no more than its least visible Field;
check_inherited() and check_inherited_property() refuse a hidden original for a Class's own member
(report_hidden_override(); an Extend's sits beside one) and narrowing (narrows(),
report_narrowing(), a default aside), and nothing else hidden clashes there or in add_member() for
an Extend's members, while members_seen()'s hide_in_place() lets a Class's member hide its bases' of
the name (Methods aside) and a type's own hide what an Extend adds in its place, where code can use
both; check_signatures() (after check_defaults()) runs check_shown_by() over each declaration seen
as far as reach_of() says, check_shown() reporting each less visible type its parameters, result,
Throws, constraints, type, base, or listed Interfaces show, naming the written visibility
(reach_text(): "'hp' is Protected in Enemy, which is Public"). std's API is Public (its Fields and
helpers stay private), the step 2 tests and programs mark what they share, and
tests/programs/visibility.fb runs with visibility/. Fixed on the way: lookup_super() finds the News
of an instance base, like Queue<String>, in its definition, and known_arguments() gives Super.New
the instance's type arguments; check_target_place() types an inherited field assigned by name with
own_view(); a generic type's Static Field or Property used by name, read or assigned, records whose
instance it is (static_arguments(): the type's parameters in its own code, the instance's in a Class
extending one or an Extend of one), and lowering's instance_global() gives a `T? = Null` initializer
the instance's type; debug builds run globaldce before the coroutine passes, so an Async Function
nothing calls is split too; check_assignable() lets any of a Struct's Methods set its Static
Property by name, since it's no part of Self, and calls a Property a Property in its error; an
Extend of one instance's Operators are found (operators_of() and members_of() walk each base as its
instance, with that instance's Extends, so a Class extending Box<String> has them too), an Operator
that isn't generic or a default copy beats generic ones that take the same types (beats_generic())
in resolve_operator(), the convertible ones too, and implementation_of(), take_default() marks a
default on one instance `extended` (no Method table slot), conform_instances() conforms a Class's
instance bases before check_inherited(), conform_instance_operators() gives an instance whose Extend
adds such an Operator a conformance of its own to the generic type's Interfaces (listing_of(),
put_instance_operators(), as the listing sees), and lowering's implementation() looks at each base's
instance, then its definition; what Extends of one instance add is `analysis_t::instance_members`,
which lowering's equal_operator() reads, walking the type and its bases, each instance's before its
definition's own (compares_two()), so an instance's == holds in Structs, arrays, tuples, T?s, and
generic code; matches_required() and property_implementation() give a generic type's own members the
instance's types (member_type()); check_instance_extensions() runs check_inherited() on what an
Extend of a Class's instance adds against the instance's base; synthesized() records each built-in
Hash in `m_built_in_hashes` (`built_in_hash_t`), and check_hashes() reports an Operator == that came
later, from an Interface's default or another file's Extend, or an instance's, through a generic
Class extending one too (instance_extending(), hashes_built_in(), report_unhashed()), while a
Class's built-in == no longer asks its fields to be comparable; and type_table_t::fill() keeps the
Interfaces an Extend of the instance itself lists, while conform() counts what an instance's generic
type gives it (given_interfaces()), so an Extend of the instance can't list another instance of one
of those. implementation_of() takes an Interface's own default before another Interface's of one
name and types that the type gets itself, so a type can have both, while one it has from the Class
it extends, or an instance from its generic type, implements what it fits (the Interface's own
first), a Method that holds a default's place with another result is a mismatch
(matches_required()'s `result`), and report_ambiguous_defaults() (with spoken_list()) reports an
Override of two inherited defaults or an Interface's member without a default that two fit;
redeclares() and drop_redeclared() (in members_of() and operators_of()) make an Interface's member
like one of an Interface it extends (refines()) one member, and conform() conforms an extending
Interface first; known_arguments() and own_view() give a member of an extended Interface used by
name in a default this Interface's Self; resolve_operator() lets a T? and a T compare the built-in
way; extension_implements(), interfaces_of(), conformance_query_t, and lowering's implemented() give
a Class what a conditional Extend gives a Class it extends; check_inherited_property() finds a
hidden Property wherever it is, and looks past it for an Extend's; lowering's compares_values()
compares a Class? through its Class's Operator ==, narrowed or not, and outliving() copies what a
Return reads from a temporary, like Make()[1], before the statement releases it. A conditional
Extend's defaults are conditional: conform() keeps each listing's own Where lines (an unconditional
listing of one Interface wins), add_conformance() and implementation_of() pass them on, and
take_default() marks a default taken for them `extended`, with them as its constraints;
implementation_of() and property_implementation() skip a member whose Where lines don't hold where
the Interface is listed (unmet_where_listed()), reporting a written one (report_conditional());
hide_in_place() lets a Method every instance has hide a conditional_default() in its place; sema's
reflected_types() and lowering's reflected_methods() and reflected_members() (has_member()) go over
only what an instance has; conformance_query_t gives a Class nothing of a constraint-only Interface
from its bases (`analysis_t::constraint_only`); lowering's lower_instance() gives a generic default
its own type arguments, and an array's copy keeps them after its T. A conditional Operator == is
only the instances' that meet its Where: sema's instance_has() leaves it out of resolve_operator()'s
candidates and comparable() for the others, which compare the built-in way, as lowering's
equal_operator() does with has_member(); check_inherited() and check_inherited_property() leave out
what never_has() says the instance a Class extends never has, like Base<Rock>'s, and an Override
beside a Method every instance has overrides that one; lowering's implemented() adds the Interfaces
a conditional one extends. Finishing step 3 also fixed what verification found: a conditional Method
can't be overridden (check_inherited()); a Const Function whose body didn't parse doesn't run
(`m_unparsed`); Self in a [Weak Self] lambda reads as a T? (weak_self_in_scope()); a Static through
a Class extending an instance takes its type arguments (known_arguments()); lowering's may_change(),
settled(), and changes_after() read operands left to right, and an index's T[] first; an Abstract
member takes its Class's visibility and an Override with none takes what it overrides'
(inherit_visibility(), report_unusable_abstract()); Pool's generations start at 1; reach_of() takes
an Extend's reach from every type it's added to (declared_parts()); unseen_hint() and
report_hidden() hint only usable members; resolve_argument() reports a hidden Const; an inherited
New is no more visible than the base's; members_of() lets nothing the code can't use hide what's
above it; lowering's member_chain() and sees_extension() give reflection an instance's Extends and
only the ones its file sees, and For Property replaces only what an Override overrides;
take_default() makes what an Extend's listing gives the Extend's (listed_in_declaration()),
check_default_inherited() checks a default like a written Method, and check_equality_defaults()
reports two Interfaces' default == at the type (`m_ambiguous_equality`), and implementation_of()
leaves it to that report. The second round made lowering's lower_arguments() evaluate a call's
arguments in written order (written_arguments()) and object_of() settle its object, in
call_interface_member() too; may_change() counts what runs_code() says a value's text or == can run;
settled_place() copies the reference an element or an assignment's target is reached through, and
op= and slices read their operands first; outliving() copies a Return's value read through a place
when anything's released before it; and closures' captured copies (`m_unchanging`) are never
settled, since a Parallel For's threads share them. The third round made settled() give a borrowing
temporary, like a cast of a variable, a reference of its own, and settles a `?.` call's receiver
(object_of()), interpolation's parts, a Property assignment's receiver (set_property()'s `changes`),
the objects a ByRef argument is reached through, and reflection's Set and Call.

Step 4, `Extern` and `CString`: `extern_stmt_t` (convention, library, functions, modifiers) from
parse_extern(), whose Functions parse_function() gives an `external_t` (`convention_t` C, STDCALL,
or RUNTIME, the `= "symbol"`, the library) and the block's visibility (`m_extern`); `"(extern ...)"`
in dump_ast. CSTRING is a built-in type id after STRING: a `const char*` whose zero is null, not
Printable (showable()), Equatable (comparable()), or Sendable. Sema's declare_extern() reports
`"runtime"` outside std (in_std()) and `Lib`, and check_extern_function() what C hasn't (generics,
Throws, Async, Const, @Inline, defaults, varargs), types beyond is_c_scalar() (a String parameter
only for the runtime, a String or String? result), and the symbols "", `main`, `fb_...`, `fb.`, and
`llvm.`, marks CString parameters `symbol_t::converts_string`, which rank() and check_argument()
let a String fit, or `string_escapes` for a Function that gives a CString or takes one ByRef, and
fills `m_externs`, whose check_extern_symbols() wants one signature per symbol, an Enum counting as
its integer and a String result as a CString. match() ranks a built-in's untyped parameter
`match_t::ANY`, below CONVERTIBLE. A Spawn's work can't give a CString (holds_cstring()), and
in_std() asks module_table_t::is_standard(), which only add_package()'s `standard` one is.
Lowering's declare_function() gives an Extern Function a `mir_function_t::external` (`mir_extern_t`:
symbol, c_abi), its parameters, and a CString return type for a String one, but no blocks, and
extern_call(), which a Func value's closure code calls too, copies a String into a CString
(CSTRING_NEW) before the call and frees it (CSTRING_FREE) right after, copies a String C gives
(STRING_FROM_CSTRING, before the frees), panicking on NULL for a String, and checks an Enum C gives
or sets ByRef (to_enum()); lowering's zero() of a CString is null; codegen's declare_extern() reuses
a declaration the module has, like the runtime's, and extend_for_c() adds zeroext for a Bool, and on
x86-64 outside Windows and on Apple's targets signext/zeroext for small integers. The parser skips
an Extern Function written with a body (extern_body_follows()). The runtime has fb_cstring_new(),
fb_cstring_free(), fb_cstring_live_count(), which fb_finish() reports, and fb_string_from_cstring(),
which puts U+FFFD for what isn't UTF-8, as fb_string_chars() does through decode(). Print moved into
std/core/print.fb over `Extern "runtime"`'s fb_print, and top_level() lets a module's own or
imported Function of a built-in's types hide it (declare_in_module() no longer refuses one).
tests/programs/extern_c.fb calls the C runtime, with tests/programs/extern_c/libc.fb's Public block,
and panic_extern_enum.fb and panic_extern_null.fb panic at the call.

Step 5, `project.toml` and packages: compiler/include/fb/manifest.hpp's read_manifest() reads a
project.toml with toml++ (FetchContent, TOML_EXCEPTIONS=0) into a `manifest_t` (name, version, main,
`dependency_t`s), each table in_order(), reporting at its lines; the driver's `package_loader_t`
reads the main package's and its dependencies' (each once by folder, by equivalent(), reporting a
missing or unreadable one, a name that differs, two of one name, or a cycle; an unreadable main
one is compile_file()'s error, unreadable()), and compile_package() makes the program from the
main file, given or project.toml's (a missing `main` is an error at its line), or for a library
(`package_info_t::library`), an empty "(library).fb" (named apart from the package's files) with
library_files() loaded as
`file_set_t::extra` modules. `file_set_t::packages` (`package_t`: name, version, root, prefix,
dependencies, main) maps every package's paths, the main one's too, by its name ("game/ui/hud.fb",
"physics/shapes.fb") to its folder (disk_path(), package_named()); resolve_import() takes an
`import_from_t` (importer, package, folder, dependencies), so `Import physics.shapes` reaches only
a dependency or the package itself, a folder Import skips a package's main file (`package_t::main`,
compared on disk) and the program's main module, a package's main module is named by its path
("game.main"), a quoted Import naming a folder is an error, and
nested_package() refuses an Import into a folder with a project.toml of its own
(report_nested()); each package is its own package_id_t; warn_respelled() warns where an Import's
written parts spell a folder or file in other letter cases. compile_file() takes a .fb file,
which builds in the package find_package() finds above it, or a package's folder;
`compilation_t::package` names the program (`program`), and the CLI takes nothing for the current
folder's package, writes a package's build to <root>/build/<program>, and won't build or run a
library. tests/packages holds game, which depends on the physics library (game.out), run by
program_tests.cpp's package runner.
Step 6, native libraries: manifest.hpp's `native_t` (name, `native_kind_t` STATIC or SYSTEM, path)
from [native] (read_natives(), read_native(); `dynamic` is an error until M8); the driver's
package loader finds each archive (find_archive(), archive_names(): the file, libx.a, x.a, or
x.lib) or reports it (report_no_archive()), `package_t` keeps its `natives` names and `linked`
libraries, natives_of() puts every package's in link order (finish(): a package's before its
dependencies') in `compilation_t::natives`, a link.hpp `native_libraries_t` (archives, system)
that link_program() puts after the objects, and -l<name> after the runtime; the module table's
add_package() takes each package's natives (`natives()`), which sema's check_library() checks
`Lib "name"` against; link_errors() (`link_errors_t`, complete when they're all the linker's)
turns lld's errors into errors at an undefined symbol's first Extern Function (undefined_symbol(),
with Apple's '_' only there) and a missing system library's project.toml line
(`compilation_t::system_spans`), which the CLI's report_build_error() shows before the rest;
link_program() passes `--error-limit=0`, and on Windows paths that aren't ASCII by their short
names or as ASCII copies (linker_path(), `link_folder_t`), writing such an executable in the
temporary folder and moving it (move_output()); the loader checks each archive starts as one
(is_archive()) and finds a file named in full whatever its extension (extension_is()).
tests/native's vecmath C library (CMake target fb_vecmath, built for UCRT on MinGW, FB_TEST_VECMATH)
is linked by tests/packages/vmath's [native], which native_calls depends on, with `m = { system =
"m" }`; program_tests.cpp's staged_packages() copies tests/packages into the build folder with the
archive in libs/.
Step 7, pointers and `Unsafe`: a PTR type kind (`type_table_t::pointer()`, "Ptr<Void>" for NONE)
with one field, "Value", so `p.Value` and `p[i]` are place steps that codegen's address_of() and
element_address() go through (pointee(), which panics on Null in debug builds); `unsafe_stmt_t`
and `modifiers_t::is_unsafe` (parse_unsafe(), line_ends(), next_line_is_body() for "Unsafe
Method" in an Interface); sema's `m_unsafe` (in body_t) and require_unsafe(), pointer_of() and
not_c_data() (Structs checked after declaring, check_pending_pointers()), null_beside() for a
Ptr's and CString's Null, Ptr<Void> in converts_plainly(), check_pointer_arithmetic(),
check_pointee(), check_pointer_index(), pointer_cast() for As, check_address_of() (`expr_info_t::
address_of`) and check_size_of() with c_layout(), check_unsafe_function() and the Override and
Interface rules, and a CString's Unsafe ToString (`analysis_t::cstring_text`); MIR's
`address_of_t` and `pointer_t`, which lowering emits for AddressOf, `p + n`, `p - q`, and op=;
std/ffi/mem.fb's Mem; tests/programs/unsafe.fb, panic_null_pointer.fb, panic_null_cstring.fb,
and vecmath's pointer functions through vmath (Sum, Filled, PairTotal, Shout, Numbers).
Its review's fixes: type_table_t::has_references() stops at a Ptr, so a Struct can point at its
own, and check_struct_cycles() runs before anything asks for a size; sema's is_c_pointer() types
aren't shown in a string (showable()) or taken by `??` and `?.` (check_coalesce(),
check_safe_object()), while a generic's T? of a Ptr is a real T?; points_at_text() decides which
Extern Functions take no String for a CString; AddressOf goes through check_changeable() and
takes a Struct's Self; through_element() lets what a Ptr points at go ByRef; in_c() makes every
pointer one type for check_extern_symbols(); For ... In needs Unsafe code for an Unsafe Iterate;
an Unsafe Function's defaults are Unsafe code; c_layout() saturates and SizeOf refuses what an
Int64 can't count; lowering's settled_place() settles a Ptr like a T[], and reflected_methods()
leaves Unsafe Methods out; tests/programs/pointers_more.fb. Its second review's: check_size_of()
declares what it measures first while types are declared (declare_held(), declare_early(),
`m_declaring`, `m_enum_types`), and refuses a Struct whose size depends on itself, as an early
Const Function run waits for one (holds_itself() in reach_type()); not_c_data() and
needs_objects() pass a Struct once; settle_string_parameters() decides the escape rule for
`m_c_functions` once types are declared, Ptr parameters too; AddressOf takes no ReadOnly Static
Field, temporary's field, or Self in a lambda; a Spawn's work gives no Ptr (holds_cstring());
through_pointer() places aren't checked for overlapping ByRef; lowering's CString ToString takes
Self by name.
Step 8, `@CLayout` and the C ABI: `modifiers_t::is_c_layout` (parse_attributed() takes it only
before a Struct); sema's check_c_layout() after link_structs(), is_c_layout(), plain_struct_in(),
and check_c_struct() for what an Extern Function sees; compiler/include/fb/c_abi.hpp's
c_layout() (which SizeOf uses), c_abi_of(), and the pure classify() (`c_passing_t`: DIRECT
pieces, INDIRECT, MEMORY; `c_registers_t`); codegen's declare_c_structs() and call_c_structs()
(`c_signature_t` per Extern, `m_c_signatures`), with sret and byval attributes, entry_alloca(),
and piece_type(); tests/compiler/c_abi_tests.cpp checks classify() for both ABIs and the IR for
x86_64-unknown-linux-gnu and x86_64-w64-windows-gnu, and vecmath's by-value Structs run through
vmath's Structs(). Its review's fixes: lowering's check_enum_fields() (holds_checked_enum()) checks
each Enum C gives in a Struct, by value or ByRef, through nested Structs and fixed arrays; the
parser checks @CLayout before an Enum; codegen refuses a by-value Struct for a C ABI it hasn't
(`m_unsupported`); c_layout() lists fields only for values of 16 bytes or less; vm_spans takes
seven Spans, which SysV passes partly on the stack; tests/packages/panic_enum_result and
panic_enum_byref, whose dependency's paths program_tests.cpp's from_folder() makes relative.
Its second review's: check_c_struct() refuses a by-value Struct of 4 GiB or more, the Enum-field
panics name the Field (check_enum_fields() takes a message function), and the parser leaves
@CLayout before Shared Struct to the Shared error.
Step 9, `Callback`: `function_type_t::callback` (parse_function_type() for KW_CALLBACK) and a
CALLBACK type kind (`type_table_t::callback()`); sema's callback_of() keeps it to C scalars,
check_callback_function() and check_callback_lambda() make one where a Callback is expected
(`expr_info_t::callback`, a worker each), check_operands() gives code beside a Callback its type
(is_code()), and check_invoke() calls one in Unsafe code; lowering's `callback_t` (a Function's
code from function_of(), or a lambda's code_of()) and invoke() for CALLBACK; codegen's
value(callback_t) makes a thunk per function ("fb.callback.<name>", `m_thunks`) with C's
extensions (c_extension(), extend_call_for_c()), and call_callback() calls through one;
tests/programs/callbacks.fb sorts with qsort, and vecmath's vm_apply, vm_count_if, vm_each, and
vm_measure_all call back through vmath's Callbacks(). The thunk checks each Enum C passes
(is_enum_value(), at `callback_t::span`), as vm_paint shows, which
tests/packages/panic_enum_callback panics on. Its review's fixes: check_callback_function()
refuses a ByRef parameter and reports two imports' Functions that fit, check_qualified() and
is_code() take a qualified Function, match() ranks code against an overload's Callback, quietly()
drops the workers its try added, type_table_t's copy keeps `m_callbacks`, codegen's convert()
and c_layout() treat a Callback as a pointer, lowering's invoke() checks an Enum a Callback gives
(`invoke_t::span`, which debug builds' call_callback() panics at on Null), lookup_callee() calls
`p.Value(5)`, and an array of Callbacks is named "(Callback(Int32): Int32)[]"; vecmath's
vm_colorer_of and vm_handle, tests/packages/panic_enum_callback_result, and
tests/programs/panic_null_callback.fb.
Step 10, bindgen: tools/bindgen's fb_bindgen library (include/fb/bindgen.hpp's
generate_bindings(), `bindgen_options_t`, `bindings_t`) over libclang (cmake/libclang.cmake's
fb_libclang, beside LLVM-C; without it nothing of bindgen is built), and the fb-bindgen tool next
to fb, which `fb bindgen` runs (`command_t::BINDGEN`, `options_t::passed`, process.hpp's
current_executable()); its generator_t names the header's structs and enums (a typedef's name
first, a '_' for a keyword or a taken name), maps C types (map_type(): integers by size,
CString for const char*, Callbacks, Ptr<Void> for void* and opaque structs, fixed arrays),
checks each struct's layout (problem_of()), makes enums Enums or Consts and number macros Consts
(read_macros(), evaluate(), constant_of()), wraps lines at 100 columns, and skips the rest with a
comment and a warning; bindgen_config.hpp's system_includes are the C compiler's include folders;
tests/bindgen/bindgen_tests.cpp (fb_bindgen_tests) checks each mapping and that the output
compiles, and the CLI tests run `fb bindgen`. Its review's fixes: decide_records() decides every
struct before anything's written (`m_deciding` makes a pointer a Ptr<Void> meanwhile), parameters
are the canonical type's, so an array is a pointer, read_macros() and evaluate() have clang work
out each macro's last definition in a values file (constant_of(), integer_literal(),
float_literal()), undefined() finds #undefs, could_be_number() keeps statements out, namer_t
names each scope (valid_name(), reserved()), gather() and emit_definition() bind what structs and
unions define inside them, each_constant() skips enum attributes, in_main_file() and place_of()
take a macro's expansion, wrapped() breaks after a comma or a parenthesis, errors_of() gives a line
per error, bindgen_config.hpp has c_standard and resource_dir (cmake/libclang.cmake's
FB_LIBCLANG_RESOURCE_DIR) and leaves the compiler's own folders out, `bindgen_options_t::standard`
is `--std`, and fb takes `--color` itself and passes `--fb` first, so fb-bindgen names itself
`fb bindgen`. Its second review's: evaluate() works out every macro in one values file, with
`#ifndef` for #undefs (undefined() went), a second copy after `#line` (position-dependent values),
names under `m_prefix`, which no macro starts with, and an error, -Werror warning, or has_comma()
on a macro's lines skipping it (each_diagnostic() reads a unit's diagnostics from one set, since
libclang makes it again for each clang_getNumDiagnostics()); could_be_number() follows the macros a
body uses
(`m_every_macro`, `m_could_be_numbers`) through could_be_expression(), which refuses "::";
convention_of() and described() skip a function or callback of another calling convention than
the target's C one; add_type() makes a flag enum Consts; parameter_name() takes a name from any
declaration (`function_t::declarations`); place() breaks a line inside its first parentheses,
each part again where it doesn't fit; the C library's folders come after clang's own headers
(-idirafter), `bindgen_config::gnuc_version` is -fgnuc-version, target_parts() compares triples,
and unreadable() names a target or C standard libclang doesn't know.
Step 11, the done-when package: tests/packages/bound calls vecmath only through vecmath.fb, which
fb-bindgen generated from vecmath.h (numbers, strings, pointers, structs, an enum, constants, a
callback, and vm_bag, an opaque handle), and bindgen_tests.cpp makes it again and compares the
module after its first lines (FB_TEST_VECMATH_HEADER, FB_TEST_PACKAGES), on x86-64. Its review's
fixes: sema's same_c_type() makes two @CLayout Structs of one shape one C type for
check_extern_symbols(), whose signature_difference() says what differs
(tests/programs/extern_twice.fb binds div twice); fb-bindgen skips a by-value struct where
c_abi_of() is OTHER (by_value_struct()); a package's files are named from the current folder
(shown_disk_path()); vecmath handles a failed allocation, and bound frees its bag with Defer.

Finishing M5, the lows: instances carry their generic type's module, and the compiler's own
Classes, like Cell<T>, none (type_t::made); name_start() compares modules; an ambiguous name's
uses give INVALID; std_constraint() and std_hidden() hint where std's Printable is meant;
declare_qualifier() refuses a built-in's name; import_advice() (module_t::path, add_dependency(),
depends_on(), has_manifest()) says how a file imports another or which package it would depend
on; modifiers_lead_to() and the attribute-order messages; a variable without a name is an
error_stmt_t; unseen_hint() gives a `hint_t` that label_hint() labels; written_instance() names a
generic constructor; check_shown() names the Interface instance; a plain Extend Method sits beside
a hidden one; from_failed_import() quiets uses of what a failed Import would bring;
std_member_home() and `std_name_t::extended`; check_read() and initialize_early() order a Static
Field's reads; hides_qualifier(); check_written_arguments() for Super.New and types that aren't
generic; name_static_instance(); what the code can't use is still what it names once reported;
`m_misplaced_externs`; narrows() treats Protected and Package as neither covering the other;
`unsendable_t::parameter` names a type parameter among a message's types;
drop_clashing_extension() refuses an Extend's Method that a default the type's declaration lists
gives it; codegen's field_label() names a hidden base Field by its Class in leak reports
(tests/programs/leak_hidden.fb); owner_in() names an instance base, like A<Int32>, in
check_inherited()'s messages, which report a hidden Method of the types only when the code can
use none; refined_result() and check_refinements() report, once at the Interface, a member that
takes the parameters of one it extends but gives another result; refines_inherited() and
drop_overridden() give a Class that lists an Interface refining a default its base took that
Interface's default, as an override, as in Java; check_replaced_default() refuses another
Interface's default, seen less far than the type, in place of an Interface's own; manifest.cpp's
error_span() puts invalid UTF-8 at its byte; process.hpp's real_path() and the loader's
linked_away() and report_linked() keep Imports, and library_files() (is_link()), from leaving a
package through a link or junction, and nested_package() asks the disk (equivalent()); dump_tree()
and dump_types() take `package`, and the CLI's package_tokens() shows a library's own files;
lowering's number_alike_names() numbers MIR names that come out alike ("x.y.Z()#2"). std.ffi is
mem.fb's Mem (Alloc, Resize, Free, Copy, Fill, Compare, over the runtime's fb_mem_*) and
cstring.fb's `Extend CString` (From, over fb_cstring_new, Length, over fb_cstring_length, Free),
with tests/programs/leak_cstring.fb and panic_cstring_length.fb.

The project reviews' fixes: nested optionals, `T??`: substitute() keeps the nesting, a T? of a
nullable type is {flag, T?}, sema's converts() and convert() go by nullable_depth() (a value wraps
into a deeper T?; a Null constant of a shallower one isn't folded, and the evaluator refuses one),
narrowing, `??`, and `?.` peel one level, lowering's variable_place() steps into "value" once per
level, and codegen's to_nullable() keeps a flag of the same depth
(tests/programs/nested_optionals.fb); a function an initializer calls that reads a Global main
hasn't set panics (MIR `ready_t`; global_order.fb, panic_global_order.fb, panic_global_cycle.fb),
in release builds only where codegen's holds_null_object() says its zero would crash
(release_global_zero.fb, release_panic_global_order.fb, which program_tests.cpp builds in release
mode);
check_expansion() refuses polymorphic recursion, only_gives_value() a SizeOf alone, and
check_not_protected() a Protected Extern block; the parser's max_nesting (1000) and
cmake/stack.cmake's 64 MiB stack; the evaluator's 256 MiB `m_held`; an Extern Function as a
Callback is the C function; lowering's pinned() and pinned_place() keep what a call borrows from a
Global, field, or element alive through it (borrowed.fb); a Parallel For can't read other indexes
of an array it changes; a Static Field or Property holding a Func is called through it
(static_value()); ambiguous_at() keeps an ambiguous name's uses from giving a type; bindgen's
name_field_structs() names a field's unnamed struct "<struct>_<field>". The runtime: a Scheduler
lives through its own Tick, realloc() never takes 0 bytes, the scheduler's end() destroys timers
nobody awaits and coroutines that wait on nothing that will run (stuck()), fb_channel_element()
visits a Channel's elements for leak reports, a finished Thread is joined at the next start,
Parallel For shares one range and runs first through last inclusive, panics keep long names and
NULs, and freeing a CString the runtime didn't copy panics. The CLI: display_width() for carets,
a window on long lines and 100 diagnostics at most, an ASCII temporary folder for the linker, -o
checks, `fb run x.fb -- args`, exit codes, and the help forms. std: iterators fail fast when
their collection changes (`Changes`), Deque's readers don't move it, PriorityQueue has Iterate and
Pool has Clone and `Used`, Handle is ReadOnly and Hashable, Map mixes its hashes, and Map's and
Set's text is JoinedText().

## M4, concurrency

Done: M4 (concurrency: Sendable, Shared Class, tasks, `Parallel For`, async and coroutines,
channels, Mutex and Atomic), planned 2026-10-17 and finished 2026-10-18, in 11 steps.
Step 1, threads in the runtime: runtime/src/thread.cpp's `pool_t` starts its workers
on the first job, one per core but one, each with a `queue_t` (own newest first, then steals the
oldest); `fb_job_start()`, `fb_job_done()`, `fb_job_wait()` (which runs the jobs a worker started
itself meanwhile, run_own()), and
`fb_job_release()`; `fb_parallel_for()` hands out chunks from a counter, the caller taking some;
main calls `fb_pool_finish()` before releasing the Globals, and fb_finish() again;
`fb_job_start_thread()` for a job on a thread of its own; fb_print() holds `output()`,
and fb_panic() and fb_error_uncaught() hold `ending()` and end with end_program() (`_Exit`).
Step 2, `Sendable`: an Interface in std/core/interfaces.fb (`builtin_interfaces_t::sendable`)
that sema's unsendable() decides from a type's parts, saying why not; implements() and the
exported fb::implements() (conformance_query_t::sendable()) ask it; conform() checks a type that
lists it or an Interface extending it; it's constraint_only, and Interfaces extending it aren't.
Step 3, `Atomic` and `Mutex`: ATOMIC and MUTEX type kinds (`type_table_t::atomic()`, `mutex()`;
a Mutex's fields are state, owner, and value, as `fb_mutex_t` and then T); sema's in_place() types
are kept where they're made by keep_in_place() at declarations and parts of types, and
check_any() reports a copy unless `m_place_use` marks a Method's object, a Lock's Mutex, a ByRef
argument, or a Global's or Class Field's start; check_in_place_made() checks `Atomic<T>(value)`;
atomic_methods() are ByRef Methods; `lock_stmt_t` (parse_lock()) declares a ByRef PARAMETER for
the value; lowering's make_in_place(), atomic_call() (MIR `atomic_t`), and a Lock's `lock_t`
with an `unlock_t` cleanup, its value in `m_aliases`; codegen uses LLVM's atomics (an
Atomic<Bool> is a byte) and the runtime's fb_mutex_lock() and fb_mutex_unlock().
Step 4, `Shared Class`: `modifiers_t::is_shared` (parse_modified() keeps it to types),
`type_t::shared` (set_shared(), copied to instances); sema's check_shared() in link_class()
checks bases and Fields, and unsendable() lets Shared Classes through; `fb_class_t::shared`,
which codegen's class_value() fills, makes the runtime's retain, release, Weak, and discard
atomic for its objects (destroy_shared(), finish_shared(), `dying`, and the object's own Weak).
Step 5, `Spawn` and `Task`: TASK and THREAD type kinds (`task()`, `thread()`; fields job,
closure, and a Task's result, as `fb_task_t` and then T); sema's spawner() and check_spawn()
check the work and its captures, and task_methods() give Wait, Join, and IsDone; lowering's
spawn() fills a new object and starts it (TASK_START, THREAD_START), generated() RUN makes each
Task type's Class, whose first Method runs the closure and keeps the result, and task_call()
waits (TASK_WAIT, TASK_DONE); the runtime's fb_task_start() and fb_task_start_thread() run it
through fb_job_start() or fb_job_start_thread(), whose threads fb_pool_finish() joins.
Step 6, main-thread rules: `modifiers_t::is_main_thread` (@MainThread); sema's note_use() calls
note_thread_use(), which records each function's or lambda's (current_code()) first
`main_only_t` use and what it reaches in `m_reaches`; check_spawn() and check_shared() add
`worker_t`s, and check_threads() walks from each, through overrides, conformances, defaults, and
Destroys, reporting the shortest path to a use (check_worker()).
Step 7, `Parallel For`: `for_stmt_t::parallel` (parse_parallel()); sema's check_parallel_for()
checks the body as a lambda with `lambda_context_t::parallel`, whose capture() copies Locals,
parallel_outside() and check_parallel_element() keep outside variables unchanged and arrays
changed at the loop's own index, and `m_element_use` marks an outside array's allowed uses; the
closure_t has `body`, and stmt_info_t `closure`; lowering's lower_parallel_for() makes it and
calls PARALLEL_FOR, and lower_parallel_body() loops over a chunk, which the runtime's
fb_parallel_closure() runs.
Step 8, `Channel`: a CHANNEL type kind (`channel()`); sema's check_channel_made() and
channel_methods(); lowering makes one with MIR `channel_t`, channel_call() copies through a slot
local (CHANNEL_SEND, TRY_SEND, RECEIVE, TRY_RECEIVE, CLOSE), and lower_channel_iteration() is For
x In ch; codegen's channel_class() and generate_channel_functions() release what's left; the
runtime's `fb_channel_t` waits with pool_t::wait_until(), which runs no jobs.
Step 9, `Async` and `Await`: `modifiers_t::is_async` and `await_expr_t`; sema's
check_async_function(), an Async call typed as its Task, and the Await check (`m_locks`);
`mir_function_t::is_async`, whose code takes its Task first (`m_async_task`), keeps its
parameters, and finishes with finish_async() (ASYNC_FINISH); a call makes the Task (ASYNC_BEGIN)
and calls the coroutine; lower_await() emits AWAIT and a `suspend_t`; codegen's begin_coroutine()
uses LLVM's coroutine intrinsics (presplitcoroutine, coro passes in debug builds too); the
runtime's `scheduler_t` resumes coroutines whose Tasks are done, in fb_scheduler_finish() and in
fb_task_wait() on the main thread.
Step 10, coroutines and schedulers: the built-ins NextFrame, Seconds, RealSeconds (@MainThread),
and MainThread give a Task, which lowering's timing_call() makes and the runtime's timers finish
(WAIT_FRAME, WAIT_SECONDS); a SCHEDULER type kind, made by `Scheduler()` (check_scheduler_made(),
main-only) with scheduler_methods() Run, Tick, and IsIdle (scheduler_call()); AWAIT passes the
coroutine's own Task too; the runtime's scheduler_t has its time, ticks, timers, and
current_scheduler(), take() moves a coroutine and what it waits for, and fb_scheduler_free()
destroys what still waits, through each `suspend_t`'s cancel block, which releases what the
coroutine holds.
Step 11, loose ends: Async Throws, whose Task type is `task(result, throws, error)` ("Task<T>
Throws E", `type_t::throws`, with an "error" field last), its Wait symbol has the Throws, and
sema's Await check propagates them (untried()); lowering's takes_error() is false for Async code,
raise() puts the error in the Task (error_field()), and rethrow() checks it after a Wait or an
Await. Async Methods: call_on() passes the Task after Self, through the Method table's slot, and
Overrides match, but not in Interfaces. Async lambdas: `lambda_expr_t::is_async`, from
parse_lambda() after Async; the closure's code makes the Task and calls a coroutine of its own
(`m_coroutine_code`, lower_async_closure()). block_lambdas() lets collect_field_bounds() reach
member loops in lambdas in generic code. tests/programs/concurrency_mix.fb runs it all at once.

## M3, generics, interfaces, Extend, compile time, and reflection

Done: M3 (generics, interfaces, `Extend`, compile-time functions, reflection), planned and
finished 2026-10-17, in 13 steps. Step 1, programs of several files: the driver adds every
std/**/*.fb file after the main file (`standard_library()`, `build_config::std_directory`;
tests pass `library_file_t`s to compile()); parse_into() puts them all in one AST, library
items first, and `ast_t::from_library()` tells them apart; sema takes the source_manager_t,
finds Error in std/core/error.fb (`find_error_class()`), and reports code that runs in a
library file; lowering declares library functions in function_of() and Classes in use_class() when
the program uses them, then lower_library() lowers them after main; codegen names the module after
the main file and gives panics the right file. Step 2, tuples: TUPLE types in the type table,
interned by `tuple()`, with fields "0", "1", and so on; `tuple_expr_t`, `tuple_type_t`, and
`destructure_stmt_t` in the AST, and parse_element() reads ".0" and splits "t.0.1"; sema makes
each tuple's elements FIELD symbols (`tuple_elements()`), so member access and lowering's field
steps work as for Structs; a tuple assignment's target is a tuple_expr_t, and
`stmt_info_t::matches` holds a destructure's Locals; lowering makes tuples with `aggregate_t`, and
`has_fields()` covers Structs and tuples for ==, zeros, and strings. Step 3, function values:
FUNCTION types ("Func(A): R", `type_t::parameters`); `lambda_expr_t` and `function_type_t`; sema
checks a lambda's body in place, with `m_lambdas` marking where each one's scopes start, and
capture() records `capture_t`s (VALUE, CELL, WEAK) in each lambda crossed; `analysis_t::closures`
has each closure's Class ("Function at file:line:col"), Func type, and captures, and
`expr_info_t::closure` points at it; a captured Local has `symbol_t::cell`, a "Cell<T>" Class;
lowering's make_closure() fills a new object, code_of() declares the closure's code and its
mir_class_t, and lower_closure() lowers it after the function, copying captures into locals;
`invoke_t` calls slot 0 of the object's Method table; codegen's is_object() covers Classes and
function values. Step 4, generics: PARAMETER types; a generic Struct or Class has
`type_t::parameters` and is named "Box<T>", and `type_table_t::instance()` makes "Box<Int32>"
(`generic`, `arguments`), whose fields set_fields() fills in; `substitute()` rebuilds a type with
arguments in place; `symbol_t::type_parameters` for a function's own; sema's `m_type_scope` resolves
names in types, infer() and unify() find a call's type arguments, `m_substitution` puts them in
parameter and return types, and `expr_info_t::type_arguments` records a call's own; lowering copies
the type table into `mir_module_t::types` (a shared_ptr), concrete() substitutes the instance's
`m_parameters`/`m_arguments`, instance_of() declares an instance like "First<Int32>(Int32[])",
and add_class() gives each Class instance its own Method table. Step 5, operators:
`function_kind_t::OPERATOR`, parsed with parse_operator_name(); an Operator is a static METHOD
symbol named by its operator, like "+"; sema's resolve_operator() picks among the operands'
types' operators_of() and records the call on the expression (`symbol`, `arguments`,
`type_arguments`), or on an "op=" statement's stmt_info_t; lowering's lower_operator() and
call_operator() call it ("!=" negates "=="), and equal_operator() makes every == on a type use its
Operator ==. Step 6, interfaces: INTERFACE types (`add_interface()`, whose `inner` is its Self
parameter) and `type_t::interfaces` for Implements and Extends; `type_stmt_t::is_interface`,
`constraint_t`s on functions and types, and `required` members, which parse_function() and
parse_property() tell from the next line; sema's declare_interface_members() declares members with
Self in the type scope, conform() records each `conformance_t` and copies defaults into the type
(`symbol_t::default_method`), `constraint_only` marks Interfaces that can't be types, `m_bounds`
holds the constraints in scope (bounds_of(), implements()), and report_bounds() checks calls and
instances, or check_pending_bounds() once types are declared; a call of an Interface's member keeps
Self in `expr_info_t::type_arguments`; lowering's call_interface_member() and call_operator() call
what implements it, or go through the Interface's table (`call_t::interface`), add_class() makes
each `mir_implementation_t`, and accessor_of() handles Interface, Abstract, and Static Properties;
fb_class_t lists `fb_implementation_t`s, which fb_interface_methods() and fb_object_implements()
search. Step 7, built-in interfaces: std/core/interfaces.fb declares Equatable, Comparable,
Hashable, Printable, and Numeric, which sema finds by name (`analysis_t::interfaces`);
builtin_implements() says which the built-in types implement, builtin_methods() gives them
ToString and Hash, and synthesized() lets a Struct or Class that lists Equatable or Hashable keep
the built-in == or Hash, whose conformance member is the Interface's own; showable() goes through
Printable, and Strings order with `<`; lowering's implementation() is nullopt for the built-in
meaning, which builtin_operator() and builtin_method() lower, hash_of() and to_string() give a
value's Hash and text, and generated() makes each array type's ==, Hash, and text functions; the
runtime has fb_string_compare(), fb_string_hash(), fb_hash_integer(), fb_hash_float(),
fb_hash_object(), and fb_hash_combine(). Step 8, `Extend`: `extend_stmt_t`, from parse_extend();
sema's declare_extension() runs after every declare_item(), finds the target with
extension_target() (a type, a generic type's definition, or T[] over `m_array_element`), and adds
the members, marked `symbol_t::extended`, to a Struct's or Class's members or to `m_extensions` /
`m_array_extensions`, which members_of() reads; T[]'s members take T as their first type
parameter, which known_arguments() fills from the array; an Extend's Interfaces join
`type_t::interfaces`, or with a Where or for a built-in type, `analysis_t::extension_interfaces`,
which extension_implements() checks; link_extensions() conforms the built-in types, Enums, and
T[]; lowering keeps extended members out of the Method table, and extension_implementation()
finds a built-in type's conformance. Step 9, sequences: generic Interfaces, named like
"Iterator<T>", whose instances fill() gives substituted `interfaces`; an Interface's
owner_parameters() are Self, then its own, so its members' `type_arguments` are Self, then the
instance's (`self_arguments()`); a type implements one instance of each (`instance_for()`,
`interfaces_of()`), a conformance records it, `symbol_t::default_interface` holds a default's, and
infer() takes type arguments from constraints; `For ... In` over an `Iterator<T>`
(std/core/iterator.fb, `builtin_interfaces_t::iterator`) or what an `Iterate()` gives
(`stmt_info_t::next` and `iterate`), lowered by lower_iteration() with call_method(); `.Chars` and
`.Bytes` fill a new array with the runtime's fb_string_chars() and fb_string_bytes();
`slice_expr_t`, lowered with a bounds check and an ARRAY_ADD loop; variadic parameters
(`parameter_t::variadic`, `symbol_t::variadic`), whose arguments are `binding_t::packed` and
`expr_info_t::packed`, which lowering's array_of() makes into an array. Step 10, compile time:
`#If`, `#ElseIf`, `#Else`, and `#End If` in the lexer, which takes the symbols from
`defined_symbols()` (compile() with a library and compile_file() take the build mode) and skips
the lines it leaves out; `modifiers_t` has is_const, is_inline, and deprecated, from
parse_attributed(), and sema's note_use() warns about @Deprecated uses; `mir_function_t::is_inline`
becomes LLVM's alwaysinline, with the always-inline pass in debug builds; Globals and Consts are
declared first, and initialize_early() checks a Const's initializer when something needs its
value, in a fresh `body_t` (swap_body()); run_at_compile_time() runs a Const Function's call with
known arguments: check_const_body() checks its body on demand, lower_function() lowers it, and
evaluate() (compiler/src/evaluate.cpp) runs the MIR on a stack of frames; a Const type parameter
is a PARAMETER with an `inner`, its arguments are VALUE types (`type_table_t::value()`), a fixed
array's size can be one (`type_t::parameters`), `value_type_t` is a value in type arguments,
resolve_argument() reads one, and `expr_info_t::parameter` marks a Const type parameter used as a
value, which lowering gives its instance's number. Step 11, reflection: `Attribute Name(params)`
is a `type_stmt_t` with is_attribute, from parse_attribute_declaration(), and a use is an
`attribute_t` in `modifiers_t::attributes`, with `argument_t`s; sema's check_attributes() runs
after the defaults and puts each one's `attribute_value_t` in `symbol_t::attributes`;
`for_member_stmt_t` is For Field or For Method, whose variable is a MEMBER_VARIABLE symbol and
whose stmt_info_t `type_arguments` are {T, F}; collect_field_bounds() makes F (`m_loop_fields`)
when the function is declared and turns the Where lines into `bound_t`s with `field` set, which
field_misfit() checks at calls; check_member_variable() and check_member_call() check f.Name,
f.Get(value), and the rest; lowering's lower_stmt() makes a copy of the body per field or
reflected_methods() Method, with F in m_parameters and the member in `m_reflected`, and
lower_reflection() lowers the calls. Step 12, std.collections: std/collections/*.fb (array.fb's
`Extend T[]`, Map, Set, Stack, Queue, Deque, PriorityQueue, and Pool), which the user typed;
`map_expr_t` is a `{key: value}` literal, which sema checks against std's Map (`m_map`, from
find_map_class()) with common_type_of(), recording its Add in `expr_info_t::symbol`, and
lowering makes with new_object() and a call of Add per entry; own_members() finds an array's
Methods in an Extend of T[], and `expr_info_t::count` marks Count alone there; sema's symbols are
a std::deque, so references to them survive add_symbol(); changed_in() reaches calls inside
array and Map literals, indexes, slices, and Is. Step 13, loose ends: `is_later_name()` for
names later milestones declare; an integer's Wrapping and Checked Methods (`integer_methods()`,
`integer_method()` and `fold_integer_method()` in const_value, `binary_t::overflows`, lowered by
integer_call()); Interface constraints, Const type parameters, Throws (`m_takes_error` in
lowering), and generic Methods (`own_type_parameters()`, skipped in interface_slots());
`For x In Try items` (check_iterate_call(), checked_call()); Extends of one instance or array type
(`is_instance()`, `is_every_array()`, own_view()), and conditional Interfaces as values through
`fb::implements()`, which lowering's implemented() asks; generic bases (`base_arguments`,
`member_type()` and owner_arguments() through the bases); generic Statics (instance_global(),
global_of(), Static Methods keeping their instance's type arguments); generic Const Functions
(`m_runs` and `m_const_modules` by instance), run_early() with `m_member_types` for sizes in
declarations' types, and `value_op_t` VALUE types for `N + M` (computed_value()); reflection's
`member_kind_t`, reflected_types(), method_signature(), `expr_info_t::reflected` for `T.Has(A)`,
check_default_loops(), and closure_copy() for lambdas in the loops.

## M2, data

Done: M2 (data: structs, classes, refcounting, arrays, errors, `Defer`), planned 2026-10-16 and
finished 2026-10-17, in 11 steps. Step 1, the type table (`type_table_t` in types.hpp: built-ins
at fixed ids, other types interned; the analysis owns it and MIR points at it). Step 2, structs:
`Struct` with `Field`, `Method`, `ByRef Method`, and `New`; the generated constructor's parameters
are the field symbols; MIR places have field paths, plus `aggregate_t` and `zero_t`; retain and
release walk a Struct's Strings in codegen. Step 3, classes: objects with a header (refcount,
`fb_class_t`) from runtime/src/object.cpp; `new_t` in MIR; codegen makes each Class's `fb_class_t`
and destroy; `RefEquals`; Strings' refcounts are atomic. Step 4, nullability: a `T?` in the type
table has one field, "value", which a place steps into once a null check narrows it
(`flow_t::narrowed`); `Null` is a const_value_t holding nullptr; `?.` and `??` lower to branches; a
String? or Class? is a pointer, and any other T? is {flag, T}; every New assigns the fields without
a zero. Step 5, inheritance: `type_t::base`; sema's link_classes() puts base fields first and sets
`overrides`; a Class's New is an MIR function that sets up an object its caller made (`base_new`,
Super.New); `mir_class_t::methods` is the Method table that `call_t::slot` calls through; `is_t` and
`downcast_t` use the runtime's fb_object_is and fb_object_expect. Step 6, arrays: `T[]` is an
object (`fb_array_t`, runtime/src/array.cpp) with a Class per array type that codegen makes;
FIXED_ARRAY types are inline; MIR places have `steps`, each a field or an element with index
locals; `array_t` makes arrays; runtime_function_t has the T[] operations; sema makes each T[]'s
Methods as symbols owned by the array type. Step 7, enums: ENUM types keep their integer type
and `enum_value_t`s in the type table; sema folds Enum constants as their integers; `As` to an
Enum checks membership in lowering; lowering makes a "State.Text(State)" function for strings;
`stmt_info_t::exhaustive` marks a Select that covers every value. Step 8, `Defer`: sema checks
the statement where it's written in a copy of the flow, forgetting narrowing for what the rest of
the block changes; lowering's scopes hold `cleanup_t`s (a variable to release or a statement to
run), and each way out lowers the deferred statements again. Step 9, errors: the Error Class
(`analysis_t::error_class`, now from std/core/error.fb); `symbol_t::throws`; sema's
`handler_t` stack checks that each error is caught or in Throws; a function that throws takes its
caller's `Error?` as a hidden ByRef last parameter, checked after each call; lowering's `raise()`
goes to the innermost Try? or Try block, or out through that parameter; a Finally is a scope's
`cleanup_t`, like a Defer; Panic and Assert are built-ins (`panic_t::text`); a Class without a New
takes its base's News. Step 10, weak references and leak reports: a WEAK type kind; sema's
check_any() reads a Weak<T> as a T? (`read_type()`), except as an assignment's target; lowering's
convert() makes the WEAK_LOAD and the borrowing cast; objects have a 32-bit refcount and weak
count; `fb_class_t` has destroy, release, and visit, which codegen makes; debug builds call
fb_track_objects(), and fb_finish (now in runtime/src/object.cpp) reports cycles; a New can throw,
and OBJECT_DISCARD lets its object go without Destroy. Step 11, class extras: `modifiers_t` has
is_static and is_read_only, and variable_stmt_t has modifiers; a Static Field is a GLOBAL symbol
whose owner is its type, in `analysis_t::globals`, lowered as the global "Enemy.count"; a Static
Method has no Self; `property_stmt_t` holds two function_stmt_t accessors that the parser makes,
and a PROPERTY symbol has `getter`/`setter`; lowering's get_property()/set_property() call them,
through the Method table; check_changeable() catches ReadOnly Fields and Property copies.

## M1, the first compiler

Done: M1, finished 2026-10-16, in six steps: source and diagnostics; tokens and lexer; AST and
parser with error recovery (tests compare `fb::dump_ast`); semantic analysis (types, const_value,
sema); codegen (runtime, MIR, lowering, LLVM C API codegen, linking with lld; end-to-end tests in
tests/programs); and the `fb` CLI (`check`, `build`, `run`, `--show` for every stage, color).
`#If` directives are deferred past M1; the lexer reports them as not supported yet.
