# The review of M7 step 0 (the spec)

23 confirmed findings, each with its Status.

## spec11-1 (high): The .fbi holds only Public declarations, but caller-compiled generic and @Inline bodies, and For Field, need the library's Private and Package members

Spec 11 says the interface file is 'the library's Public declarations, with the bodies of its generic, @Inline, and Const code ... and no other bodies', and that 'a program checks against it as against the source'. But spec 06 says 'a Public generic function uses its file's Private helpers for every type it's called with', and spec 13 says For Field / For Method 'go over Private members too'. A caller that compiles a library's generic instance or @Inline body needs those Private/Package helpers' declarations (and bodies when they're generic or @Inline), plus a Public Class's Private Fields for its layout and for compile-time loops. An EmbedText/Embed in a caller-compiled body or a Public Const names a file the prebuilt archive doesn't ship. Step 1's done-when ('a package checks against a library's .fbi as it does against its source') can't hold as written. The today's fb.exe is from 2026-10-20 and can't build current std (it rejects New), so this rests on the quoted passages.

Fix: In spec 11's Prebuilt libraries, say the .fbi holds what callers compile against: every Public declaration; the Private and Package declarations that its generic and @Inline bodies use, kept internal (visible to those bodies only) and exported from the binary; every Field of the types it shows, with their visibility, for layout and compile-time loops; and Consts as their values. Then say Embed calls in shipped bodies are an error, or are resolved when the library is built.

Status: fixed: the .fbi holds every Public declaration, the Private and Package ones its generic and @Inline bodies reach, and every Field (the user's call)

## spec11-10 (medium): Nothing says which command writes a library's .fbi and .fblib, where they go, or how a dependency is found prebuilt

Spec 11 still says that for a library 'it has nothing to build or run', but step 1's done-when has fb write a library's .fbi, and step 2 has it write .fblibs per target and mode. The spec doesn't say which command writes them (fb build on a library?), to which paths (build/<name>.fbi?), how the files for each target and mode are laid out in a package folder or archive, or how a dependency `{ path = ... }` is read as prebuilt rather than as source, including which wins when a folder has both.

Fix: Add to spec 11's Prebuilt libraries: 'fb build' on a library writes <folder>/build/<target>/<mode>/<name>.fblib and its .fbi. A package folder with lib/<target>/<mode>/<name>.fblib (or the archive's layout) and no .fb source is prebuilt, and having both is an error, or the source wins. Update line 164.

Status: fixed: fb build on a library writes build/<target>/<mode>/<name>.fblib and .fbi, and a package with lib/<target>/<mode>/ and no source is prebuilt

## spec11-2 (medium): The ABI hash can't tell two builds apart, so another build loads silently even when its layout has changed

The hash 'covers the interface file, the compiler's version, the target, and the mode', and the spec claims 'A program is built against one exact build of each library, and linking or loading another is an error that names both builds'. Two builds whose .fbi is the same (for example v1.0.0 and v1.0.1 differing in a function body, or in a Private Field of a Public Class, which spec 11's .fbi leaves out) get the same hash. So loading the other build isn't an error. When a Private Field was added, a Class in the executable that extends the library's Class (allowed across the edge) puts its own Fields at offsets that overlap the base's new Field, which corrupts memory without any error. The hash also doesn't include the version, though libraries.md's metadata section has one, and nothing says what 'names both builds' prints.

Fix: Make the hash cover everything callers bake in: the full .fbi (every Field, Private ones too, and each Class's Method table order), and say the version is part of what an error names. Or, if 'exact build' is really meant, add a build id (a hash of the compiled objects) next to the ABI hash and check it. Then say what 'names both builds' prints (name, version, and hash of each).

Status: fixed: the ABI hash covers the .fbi, the compiled code, the compiler's version, the target, and the mode, and an error names both builds' names, versions, and hashes

## spec11-4 (medium): 'Each package is in one place' contradicts 'a beside library's own dependencies are embedded in it'

Spec 11 says 'A beside library's own dependencies are embedded in it, unless the program lists them beside too. Each package is in one place in a program'. When two beside libraries depend on the same package X that the program doesn't list as beside, or when the executable embeds X (directly, or by listing it with the default embed) and a beside library depends on X, X is embedded in two binaries, with two sets of its Globals and two copies of its code. libraries.md also says a beside library takes 'what else it uses from the executable, from the executable itself', which contradicts the embedding rule. The same holds for a plugin and its interface package: the host embeds the interface package, and by this rule the plugin embeds it again.

Fix: Settle the rule: a package that more than one binary reaches goes in the executable (exported to the beside libraries), or the build stops with an error naming the package and asking the program to list it beside. Say a beside library's dependency that the executable already has comes from the executable, and say the same for a plugin's interface package (spec 12).

Status: fixed: a package two binaries use has to be beside, or the build stops naming it and its users (the user's call)

## spec11-5 (medium): The spec doesn't say where std's own compiled code and Globals live when there are beside libraries

Spec 11 says every binary uses 'the executable's runtime: one heap, one set of refcounts, one scheduler, and one registry of types', but std's .fb modules aren't the runtime, and they have Globals: std/math/random.fb's seeding Mutex and seedings counter, and std/term/terminal.fb's consoleOutput, consoleError, and consoleInput. If a beside library embeds the std modules it uses, as it does its other dependencies, there are two of each. SeedRandom in the executable doesn't reseed the library's generator, and the two console Terminals buffer and flush apart. It's also unsaid whether the executable has to contain and export a std module that only a beside library or plugin imports.

Fix: Say std's compiled modules are in the executable only, exported like the runtime. The executable links every std module that its beside libraries use, and a plugin can use only the std modules its host has (or the host links all of std's exports). Add that to the step 3 and 5 done-whens.

Status: fixed: the runtime and std are always in the executable, which links every std module its beside libraries use

## spec11-6 (medium): A beside library the OS loads at start-up fails before the program's code runs, so fb can't print its own error naming the file

Spec 11 says 'The executable loads its beside libraries as it starts ... one that's missing or isn't the build the program was built against stops it with an error naming the file', and libraries.md item 3 says 'The OS loads it'. With ordinary load-time imports, the process dies inside the OS loader before any of the executable's code runs, the runtime's start-up included. On Windows the exit is 0xC0000135 (no DLL) or 0xC0000139 (a mismatched build missing a symbol), with nothing on stderr and a system dialog in interactive sessions. Step 3's done-when ('a missing or mismatched file stops it with an error naming the file') depends on which mechanism is chosen, and the spec doesn't choose.

Fix: Choose the mechanism in the spec: beside libraries are delay-loaded (lld's -delayload on Windows) or loaded by the runtime's start-up (LoadLibrary/dlopen) before any Global, then their metadata is checked and imports bound. Or say a missing file is the OS's own error and only a present-but-mismatched one is fb's. Then make libraries.md item 3 match.

Status: fixed: the executable loads its beside libraries itself as it starts, before any Global, and names a missing or mismatched file (the user's call)

## spec11-7 (medium): libraries.md's macOS edge (-bundle_loader) can't build a .dylib

libraries.md says that on macOS the beside library 'is linked against the executable (-bundle_loader)', and spec 11 says the file is lib<name>.dylib, which the OS loads. Both ld64 and ld64.lld accept -bundle_loader only with -bundle (an MH_BUNDLE), and an executable can't link against a bundle as a load-time dependency. So the stated macOS mechanism can't produce what spec 11 and step 3 call for.

Fix: Use '-undefined dynamic_lookup' (or a flat namespace for the runtime's symbols) for beside dylibs and plugins on macOS, with the executable exporting its symbols. Or make plugins bundles (MH_BUNDLE, dlopen only) and beside libraries dylibs. Say which in libraries.md, and say it in spec 11's Libraries too.

Status: fixed: macOS links beside libraries with -undefined dynamic_lookup against the executable's exports (libraries.md too)

## spec11-8 (medium): On Windows, linking a beside library or plugin against the executable's import library ties it to the executable's file name

libraries.md says that on Windows 'the executable exports them and the library links against its import library', and spec 12 says a plugin 'uses its host's runtime, as a beside library does'. A DLL built that way imports from the executable by its file name ('game.exe'). Renaming the executable afterwards ('fb build -o', or a user renaming space-game.exe) makes every beside library fail to load. A plugin, built only against an interface package and not against a host, has no host import library to link against. If it links against one host's, it loads only into an executable of that exact name. Neither spec 11 nor spec 12 says how this works.

Fix: Settle it in spec 11 and libraries.md. Either the executable hands each beside library and plugin a table of its runtime and shared code when it loads them, so they import nothing by the executable's name, or the imports are delay-loaded with a hook that binds 'the executable' to GetModuleHandle(NULL). Then say how a plugin is built (what it links against) without its host.

Status: fixed: Windows keeps the import library, so the executable keeps its built name, and a renamed one stops naming both names (the user's call)

## spec11-9 (medium): dynamic natives: copying lib<name>.so or .dylib beside isn't enough when the library has a versioned soname or another install name

Spec 11 says `dynamic` 'finds ... libengine.so on Linux, and libengine.dylib on macOS', and 'A build copies every dynamic library the program needs beside the executable, which finds them there ... on Linux through $ORIGIN, and on macOS through @loader_path'. But the executable records the library's SONAME (Linux) or install name (macOS), not the file it linked. A package's CI usually builds libengine.so with SONAME libengine.so.1, and a dylib with an absolute or @executable_path install name. Then the copied libengine.so isn't what the loader looks for, and a dylib with an absolute install name is looked for at that path, whatever LC_RPATH says. Step 6's done-when ('runs from its build folder') fails for such packages.

Fix: Say the build copies each library under the name the executable records (its SONAME on Linux), and on macOS requires an @rpath/ install name or rewrites it (as with -change / install_name_tool) to @rpath/lib<name>.dylib. Or say a package's natives must have these names, and that otherwise it's an error naming the package and the file.

Status: fixed: dynamic natives are copied under the SONAME, and a macOS install name has to be @rpath or @loader_path, or the build stops naming the package and file

## spec12-1 (medium): DynamicLibrary's Function<F>(name) is named with a reserved keyword, so the API can't be declared or called

Spec 12's new section, the example, and stdlib.md's std.ffi line name the symbol lookup `Function<F>(name)`, called as `lib.Function<Callback(): CString>("sqlite3_libversion")`. Spec 01 lists `Function` as Reserved, which it says "Can never be used as identifiers", and it isn't on the contextual list. So step 5 can't declare the Method in std.ffi, and no program can call it. Today's parser rejects both the declaration and a member access named Function.

Fix: Rename the lookup in spec 12 (prose and example), stdlib.md's std.ffi line, and libraries.md, for example `Symbol<F>(name)`. Don't make Function contextual after a dot.

Status: dropped: plugins and loading at run time left M7 by the user's call (2026-10-28), so spec 12's section is gone

## spec12-3 (medium): The spec gives no way to build a plugin, and doesn't say where its dependencies (the interface package, std) live

Spec 12 says "The plugin is built beside, as its own .dll ... as a beside library does". Spec 11, though, says a library "is built only into the programs that use it, or beside them", and that `link = "beside"` is the using program's choice, because "the library's author doesn't choose". A plugin has no using program at build time, so neither spec says which command builds a library package into a .dll/.so/.dylib on its own. There's no `type = "plugin"` either, yet spec 12 says "A plugin without one [@PluginEntry] ... is an error": the compiler can't tell a plugin from an ordinary library missing an entry. Spec 11 also says "A beside library's own dependencies are embedded in it, unless the program lists them beside too. Each package is in one place in a program". For a plugin, nothing says whether the interface package and std are embedded in the plugin, which would put them in two places with two copies of their Globals, or taken from the host. If they come from the host, nothing says the host must export its embedded interface package or list it beside. Step 5's done-when, "a host loads an FB plugin", can't be built without this.

Fix: Settle it with the user. For example, `type = "plugin"` (or `fb build` of a library with `--plugin`) builds the .dll and requires exactly one @PluginEntry. A plugin takes the interface package, std, and every package the host has from the host, which exports them. The plugin embeds only packages the host doesn't have.

Status: dropped: plugins and loading at run time left M7 by the user's call (2026-10-28), so spec 12's section is gone

## spec12-5 (medium): What a relative path in DynamicLibrary.Open and Plugin.Load is relative to, and how the prefix and extension are tried, is unsaid

Open takes "a path without the OS's prefix and extension, which it adds, or with them", and Load takes "the OS's prefix and extension optional". The example's "native/sqlite3" and "mods/tanks" are relative. The spec doesn't say whether a relative path is taken from the current directory, which changes when a game is launched from a shortcut, or from the executable's folder, where M7 puts beside libraries and natives. It doesn't say whether a bare name like "sqlite3" is searched on the OS's library path, as dlopen and LoadLibrary do. It doesn't say how 'with them' is detected: whether Linux's "native/sqlite3.so" (extension, no lib prefix) or "libsqlite3.so.0" is taken as given, or the as-given path is tried first and the decorated one after. Each choice gives different results on Windows, Linux, and macOS.

Fix: For example: a relative path is from the executable's folder. The path as given is tried first, then with the OS's prefix on its last part and the OS's extension. A bare name with no folder is also searched as the OS searches. Say the same for Plugin.Load.

Status: dropped: plugins and loading at run time left M7 by the user's call (2026-10-28), so spec 12's section is gone

## spec11-11 (low): `type` leaves open: a library with main.fb on disk, an executable as a dependency, and `link` in a library's own [dependencies]

`main` is 'main.fb unless it says otherwise', the default type follows whether there's a main file, and only 'a library with `main` written' is an error. For `type = "library"` with a main.fb present and no `main` key, the spec doesn't say whether main.fb is the library's main file (left out of `Import physics`) or an ordinary module, physics.main. Spec 11 still says `Import physics` leaves out a dependency's main file, so it implies executable packages can be dependencies, but the new text says only library packages are built into programs. It doesn't say whether depending on an executable is an error or can be beside. It also doesn't say what happens when a library package writes `link = "beside"` on its own dependency, though 'the library's author doesn't choose'.

Fix: Say, for example: in a library, main.fb is an ordinary module unless `main` is written (which is the error). A dependency of type executable is an error naming it. `link` in a library's own [dependencies] is a warning that it's ignored, since the program decides.

Status: fixed: main.fb in a library is an ordinary module, an executable dependency is an error, and link in a library's own dependencies is a warning

## spec11-12 (low): spec 11's older [native] bullets weren't updated for `dynamic`

After adding `dynamic`, spec 11 still says 'A program links every [native] library of its packages, statically', and it gives the link order for those. The `\`-as-`/` rule names only 'main, a dependency's path, or a static library's', not a dynamic one's path. On Windows, 'its import library' isn't named the way the static files are (libengine.dll.a, engine.lib, or libengine.a?), though a C++ DLL 'built with any compiler' could come with either MinGW's or MSVC's. It's also unsaid where a beside library's own static and dynamic natives are linked (into its file or the executable).

Fix: Reword line 175 to 'every static [native] library', add dynamic paths to the `\` rule, list the import library names searched (for example native/<target>/engine.lib, libengine.dll.a, libengine.a), and say a beside library's natives link into its own file.

Status: fixed: the static-linking and path bullets name static and dynamic libraries, the import library names are listed, and a beside library's natives are its own

## spec11-13 (low): 'Embeds every package, is one executable' is false when an embedded package brings a dynamic native library

Spec 11 says 'A program that uses only std, or embeds every package, is one executable, which needs nothing but the OS's own libraries', and milestones.md says each M7 step keeps that. But an embedded package can have `[native] engine = { dynamic = ... }` (step 7's engine package is exactly this), whose DLL the build copies beside the executable and which the executable needs.

Fix: Add the condition in spec 11:200, libraries.md item 11, and milestones.md's M7 intro.

Status: fixed: one executable unless a package brings a dynamic native library (spec 11, libraries.md, milestones.md)

## spec11-14 (low): A beside library's file name isn't checked against the OS's own DLLs, or for names that differ only in letter case

A beside library is <name>.dll for any package name (an identifier, and identifiers are case-sensitive). The only check is against 'another file the build puts beside the executable'. A package named like a system DLL the program imports, such as `version` (or `dbghelp`, `bcrypt`), becomes version.dll beside the executable, and Windows binds the executable's system import to it. A KnownDLL name like `kernel32` makes the reverse fail. Two packages Physics and physics give Physics.dll and physics.dll, one file on Windows and on default macOS disks, and the spec doesn't say the clash check ignores letter case.

Fix: Extend the rule: the name is compared ignoring letter case, and a beside library can't take the name of a library the executable imports from the OS (or of a Windows KnownDLL). Or beside files get a prefix of their own, which spec 11 and libraries.md item 3 would both then say.

Status: fixed: a beside library's name is compared ignoring case, and can't be an OS library's or a Windows known DLL's

## spec11-3 (low): One .fbi per package can't start with an ABI hash that differs per target and mode, and #If can change its declarations

The .fbi's 'first line is its ABI hash', and the hash covers 'the target, and the mode', so every target and mode has a different hash. Yet a prebuilt library is 'a .fblib for each target and mode ... with its interface file', and an archive holds 'its prebuilt .fblibs per target and mode with the .fbi', which is one .fbi. Spec 13's #If DEBUG / PLATFORM_WINDOWS can also change which Public declarations exist, so the .fbi's text itself can differ by target and mode. The hash also covers the interface file whose first line is the hash, which is circular. Step 2's done-when ('a mismatched build is an error naming both') depends on knowing which hash is compared.

Fix: Say the .fbi is written per target and mode (inside or next to each .fblib), with #If resolved and its first line the hash of the rest of the file plus the compiler version, target, and mode. Change Package archives to say 'each .fblib with its .fbi'.

Status: fixed: the .fbi is written per target and mode, with #If resolved, beside its .fblib

## spec12-10 (low): decisions.md and libraries.md still say Plugin.Load(path), against the new Plugin.Load<T>(path): T

The new row (line 968) settles `Plugin.Load<T>(path): T` with exactly one entry giving one Interface. The older row 961 still says "`Plugin.Load(path)` loads a library built against a host's interface package". libraries.md, the design the spec must match, still says `Plugin.Load(path)` in Decided item 7 and in Loading at run time, and that the host "gets values of the host's Interfaces from the plugin's entry Function". The plural suggests several values or Interfaces per entry.

Fix: Update decisions.md row 961 (or note that row 968 supersedes it) and libraries.md's item 7 and Loading at run time.

Status: dropped: plugins and loading at run time left M7 by the user's call (2026-10-28), so spec 12's section is gone

## spec12-2 (low): stdlib.md's FfiError has NO_SYMBOL, but spec 12 says a missing symbol gives Null and lists only NOT_FOUND and OTHER

stdlib.md says FfiError's kinds are (`NOT_FOUND`, `NO_SYMBOL`, `OTHER`), "as spec 12 has them". Spec 12 only ever throws `NOT_FOUND` or `OTHER` from Open, and says `Function<F>(name)` "gives the symbol as an F, or Null when the library has none". Nothing in spec 12 throws NO_SYMBOL. stdlib.md also gives PluginError a `Path: String?` that spec 12 never mentions. FfiError has no Path, even though Open fails on a path just as Load does.

Fix: Drop NO_SYMBOL (or say in spec 12 what throws it). In spec 12, say both errors carry `Path: String?`, the path given, or drop Path from PluginError.

Status: dropped: plugins and loading at run time left M7 by the user's call (2026-10-28), so spec 12's section is gone

## spec12-4 (low): Plugin.Load<T>: what T may be, what 'gives a T' means, and what a non-FB DLL is, are unsettled

Spec 12 says Load "checks that it was built against the host's build of the interface package that declares T ... and that its entry Function gives a T", and NO_ENTRY is for an entry "whose entry gives another type". It doesn't say: (a) whether a T that isn't an Interface, or one declared in the host's own package with no interface package, or a std type, is a compile-time error; (b) whether 'gives a T' means exactly T, or also an Interface that extends T (an entry giving `TankMod`, which extends `GameMod`, loaded as `Load<GameMod>`); (c) which Kind a file that loads but isn't a FutureBlitz library gets, such as a C DLL with no metadata section (NOT_FOUND, NO_ENTRY, or OTHER); (d) whether the entry may be Throws, Async, generic, @MainThread, or give a `GameMod?`, and what Load does if the entry throws or gives Null.

Fix: Add: T is an Interface declared in a library package the host depends on, or it's a compile error. 'Gives a T' is exactly T, or T and any Interface extending it. A loadable file without FB's metadata is NO_ENTRY (or OTHER). The entry is not Throws, Async, generic, Unsafe, or @MainThread, and gives a non-nullable Interface.

Status: dropped: plugins and loading at run time left M7 by the user's call (2026-10-28), so spec 12's section is gone

## spec12-6 (low): DynamicLibrary's lifetime without Close, Close twice, and Function after Close are unsaid

The spec says only "`Close()` lets the library go; a Callback from it, called after, breaks memory". It doesn't say whether DynamicLibrary is a refcounted Class that also closes when its last reference goes. If it does, a Callback outlives a library dropped by scope and dangles silently, which the spec should warn about. If it doesn't, an unclosed library stays loaded until exit, which leak reports might count. It also doesn't say what Close twice does, or Function<F> after Close (a panic, or Null), or whether DynamicLibrary is Sendable. It doesn't say whether F not being a Callback type is a compile-time error with a message, the way CData's is.

Fix: Add a sentence for each to spec 12's DynamicLibrary bullet.

Status: dropped: plugins and loading at run time left M7 by the user's call (2026-10-28), so spec 12's section is gone

## spec12-7 (low): @PluginEntry isn't in spec 13's attribute table or rules

Spec 12 adds a built-in attribute, `@PluginEntry`, but spec 13's Attributes section, which lists every built-in attribute and where each goes, wasn't updated. That matters because spec 13 says a user Attribute "can't take a built-in attribute's name" and "`@Inline` alone is always the built-in": until @PluginEntry is listed there, a std or user `Attribute PluginEntry` is legal by spec 13. Spec 12's "the attribute on anything else" also doesn't say whether @PluginEntry on a Function in an executable package, or in a library that isn't a plugin, is an error.

Fix: Add a row and a bullet to spec 13's Attributes, pointing to spec 12.

Status: dropped: plugins and loading at run time left M7 by the user's call (2026-10-28), so spec 12's section is gone

## spec12-8 (low): 'loading one path twice gives the entry's value again' is ambiguous

It isn't clear whether a second Load of the same path calls the entry again, which gives a new object, or returns the same object as the first call. decisions.md's "gives it again" doesn't settle it. It also doesn't say what 'one path' means: whether "mods/tanks" and "mods/tanks.dll", or a relative and an absolute path to one file, count as one plugin. Nor does it say what a second Load<U> of that path with another T does (NO_ENTRY, presumably).

Fix: Say, for example: 'a second Load of the same file, however its path is written, gives the same object the first gave, if it's a T'.

Status: dropped: plugins and loading at run time left M7 by the user's call (2026-10-28), so spec 12's section is gone
