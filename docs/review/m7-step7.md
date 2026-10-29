# The review of M7 step 7 (the milestone's review)

17 confirmed findings across M7, each with its Status.

## docs-2 (low): Spec 11 still says a library "has nothing to build or run", but fb build on a library writes its .fbi and .fblib

Packages bullet: "`fb check` on a library checks each of its `.fb` files ...; it has nothing to build or run." Spec 11's own Prebuilt libraries section says `fb build` on a library writes build/<target>/<mode>/<name>.fblib and .fbi, and the code does that. m7-step0.md's spec11-10 asked for this line to be updated ("Update line 164"), and its Status says fixed, but the clause is still there.

Fix: Change the end of line 164 to something like "it has nothing to run, and `fb build` writes its .fbi and .fblib (see Prebuilt libraries)".

Status: fixed: spec 11 says a library has nothing to run, and fb build writes its interface file and binary

## docs-3 (low): m7-step0.md's Status lines for spec11-4 and spec11-12 describe rules that step 3 replaced, and they aren't true of the code

spec11-4's Status says "a package two binaries use has to be beside, or the build stops naming it and its users". decisions.md now says that rule was replaced: a beside library's own dependencies are in the executable. spec11-12's Status (line 115) ends "a beside library's natives are its own". Since step 3's review, its static natives are in the executable, which the library takes them from. The task asks that every M7 review doc's Status lines be true of the code.

Fix: Add "(replaced in step 3: a beside library's own dependencies and static natives are in the executable)" to both Status lines.

Status: fixed: both Status lines say step 3 replaced them

## docs-4 (low): m7-step1.md's writer-9 Status says Class Field defaults are written as "= ...", but the .fbi writes their values

writer-9's Status says "a Class's Field defaults are its New's, in the binary, written as \"= ...\"". Step 2 revised that (decisions.md: a Field's default is written in the .fbi as its value, and an EmbedText default as its text). A hand-written `= ...` on an instance Field is now an error. history.md's step 1 paragraph (line 28) also says "Class Fields as `= ...`". That's history, but nothing there or in the review doc says step 2 changed it.

Fix: Add to writer-9's Status: "revised in step 2: a Field's default is written as its value (decisions.md)". Optionally add a note in history.md's step 2 paragraph that this replaces step 1's "= ..." for Class Fields.

Status: fixed: writer-9's Status says step 2 revised it

## docs-5 (low): decisions.md has stale M7 rows: position-independent objects, plugins, and an eleven-step plan with C-callable libraries

Line 960 says a prebuilt library is "position-independent objects with its .fbi". Line 976, and the code, say the .fblib is LLVM bitcode, and that "position-independent objects would need comdats". Line 959 still says "every plugin" uses the executable's runtime, though plugins left M7 (line 967). Line 375, the M7 build order row, says "eleven steps planned 2026-10-22: ... static and dynamic libraries for C, Classes as handles, ... loading at run time", with "FB code needs to build libraries that C and FB both call". That contradicts line 956 (nothing FutureBlitz builds is called from C) and milestones.md's eight steps, 0 to 7. It also gives the engine as M9 to M16, where the M9 table says M10 to M17.

Fix: Line 960: "LLVM bitcode under its build and bundled lines (see M7 step 2's row)". Line 959: drop "and every plugin". Line 375: replace the row with the 2026-10-24 replan (eight steps, 0 to 7, in milestones.md), with plugins left for later, or mark it as replaced.

Status: fixed: the rows say the .fblib is bitcode, leave out plugins, and give the eight-step replan and M10 to M17

## docs-6 (low): libraries.md says a beside library has a metadata section with its target and mode, and that the hash leaves out the compiled code

Decided item 3, and decisions.md line 958, say a beside library has "a metadata section (its interface's hash, its version, target, and mode)". The code (codegen.cpp:4757) writes an exported constant `fb.(build).<name>`, "name version hash" plus " for <exe>" on Windows. The loader (runtime.cpp:629) checks that. It isn't a section, and it doesn't hold the target or the mode, which are only inside the hash. libraries.md line 104 also says "The ABI hash covers the interface file, the compiler's version, the target, and the mode". That leaves out the compiled code, which spec 11, decisions.md, and libraries.md's own .fblib bullet include, and which the code does hash.

Fix: Item 3: "with its build recorded as an exported `fb.(build).<name>`: its name, version, and ABI hash (which covers the target and mode), and on Windows the executable it was built for". Make the same change to decisions.md line 958. Line 104: add "the library's compiled code".

Status: fixed: libraries.md and decisions.md say the build is recorded as an exported `fb.(build).<name>`, its name, version, and ABI hash, and on Windows its executable, and the ABI hash covers the compiled code

## docs-7 (low): milestones.md still says loading a library at run time comes in M7, and build-order steps 1 and 2 have no done notes

M6's "Not in M6" paragraph ends "Loading a library at run time comes in M7." The same file's M7 intro and its Deferred work row (line 272) say it was left for later on 2026-10-28 (open question). stdlib.md:2010 already says "not in M7". In the M7 build order, steps 3 to 6 carry "(done 2026-10-28, <sha>)" but steps 1 and 2 don't, though CLAUDE.md and history.md list them as landed (e7470aa4 and e37b4e84; 81088159 and 34c9228a).

Fix: Change line 228 to "Loading a library at run time comes later (Deferred work)." Add "(done 2026-10-28, e37b4e84)" and "(done 2026-10-28, 34c9228a)", or the right dates and shas, to steps 1 and 2.

Status: fixed: run-time loading comes later, in Deferred work, and steps 1 and 2 have their done notes

## posix-1 (medium): On Linux and macOS, a beside library of another build that can't bind is reported as missing, not as the wrong build

fb_load_beside opens the library with dlopen(RTLD_NOW | RTLD_GLOBAL) and stops with "isn't there, or can't be loaded" as soon as that returns null, before it reads the fb.(build) record. A library from another build usually calls code the old executable doesn't export, such as a std function it didn't use before. With RTLD_NOW, dlopen then fails on that undefined symbol, so the program says a file that is there is missing. Windows already handles this case: when LoadLibraryExW fails it retries with DONT_RESOLVE_DLL_REFERENCES, reads the record, and reports the build mismatch. The POSIX branch has no such fallback and drops dlerror()'s reason. Spec 11 says a file that isn't the build the program was built against stops it with an error naming the file as that build. The cli test only swaps in a build that adds `More(): Int => 1`, which imports nothing new, so it never hits this.

Fix: On POSIX, check the build before binding. Either dlopen with RTLD_LAZY | RTLD_LOCAL first (enough on Linux for function imports), read fb.(build), dlclose, and then dlopen with RTLD_NOW | RTLD_GLOBAL; or read the record from the file without loading it (an ELF or Mach-O section, as dynamic_library.cpp already parses headers), which also works on macOS. When dlopen still fails, include dlerror() in the message. Add a cli test that swaps in a build that calls a std function the first executable doesn't have.

Status: fixed: on Linux and macOS a library that can't be bound is opened lazily to read its record, so another build says so, and the message has dlerror()'s reason

## posix-2 (low): A thin arm64e dylib passes the macos-arm64 check that a universal file's arm64e slice fails

mach_o_name skips arm64e only while choosing a slice of a universal file. For a thin image it compares only the cputype (number_at(image, 4, 4) != cpu) and never looks at the cpusubtype at offset 8. So an arm64e-only dylib is accepted for macos-arm64, while the same slice inside a universal file is rejected as another CPU's. An arm64 process can't load an arm64e-only dylib, and lld refuses to link one for arm64. decisions.md (M7 step 5) says a wrong file is an error when the package is checked, and the user would get it from the linker or dyld instead.

Fix: In the thin-image path, when cpu is arm64, also return OTHER_TARGET when (cpusubtype & 0x00FFFFFF) == 2 (arm64e), and add a unit test with a thin arm64e header.

Status: fixed: a thin arm64e image is another CPU's for macos-arm64, as its universal slice is

## fbi-1 (high): A library prebuilt against another library doesn't record that build, so a newer build of the dependency links silently and corrupts layouts

build_library hashes only the library's own .fbi text and bitcode (interface.cpp abi_hash). Nothing in b.fbi or b.fblib says which build of a dependency b was compiled against. b's bitcode has a's Struct layouts, a's @Inline and generic bodies, and calls to a's Private symbols baked in. When a is rebuilt (same version, new hash, new layout) and b's prebuilt build is left alone, read_prebuilt_binary only checks each .fbi against its own .fblib. The program builds and runs with no error, embedded or beside. Spec 11 says 'A program is built against one exact build of each library, and linking or loading another is an error', and decisions.md says the ABI is exact-version because 'layouts and Method tables change freely'.

Fix: Write the build (name, version, ABI hash) of each prebuilt or source dependency a library was compiled against into its .fbi, for example a line after the header, and cover it with the hash. Then read_prebuilt_binary or compile_package should compare it with the builds the program actually has, and stop with an error naming both. Add a CLI test that rebuilds a dependency under a prebuilt library.

Status: fixed: a library's interface file records each package it was built with, `// uses <name> <version> <build>`, a prebuilt one's hash or `source:` and a hash of a source one's files, which its ABI hash covers, and a program that has another build of one stops with an error naming the library and both builds, embedded or beside

## fbi-2 (medium): The ABI hash is never recomputed, so a debug build in the release folder (or another fb version's or target's build) is linked silently

abi_hash covers the compiler's version, the target, the mode, the .fbi body and the code. But no reader ever recomputes it: read_prebuilt_binary and the beside build record only check that the .fbi's header string equals the .fblib's header string. A build copied into the wrong lib/<target>/<mode>/ folder is accepted, and so is one made by another fb version or for another target, or a hand-edited .fbi that keeps its header. Spec 11 says the hash 'covers ... the compiler's version, the target, and the mode', that 'a build links the one of its own mode', and that linking another build is an error.

Fix: In read_prebuilt_binary, recompute abi_hash(the .fbi text after its first line with \r\n made \n, the .fblib code, the build's mode) and compare it with the header, giving an error that names the expected target, mode, and fb version when they differ. This also catches an edited .fbi. Add a test that copies a debug build into release/.

Status: fixed: a program recomputes a prebuilt library's ABI hash, of this fb, the target, and its folder's mode, its interface file, and its code, and a build that isn't the one its first line names is an error saying so

## fbi-3 (medium): A Func type whose result is a Task with Throws is written without parentheses, so the .fbi doesn't parse

type_text writes a FUNCTION's result as `: <result>` with no wrapping. When the result is `Task<T> Throws E`, the .fbi gets `Func(): Task<Int32> Throws E1`, which the parser reads as a Func that throws ('a Func value can't throw'). The source's own spelling is `Func(): (Task<Int32> Throws E1)`. The library builds, but every program that uses its prebuilt build fails to check. This applies to a Global whose type is inferred, a Global array of such Funcs (`(Func(): Task<Int32> Throws E1)[]`), and a Func parameter (`Func((Func(): Task<Int32> Throws E1)): Int32`).

Fix: In type_text's FUNCTION/CALLBACK case, write the result in parentheses when it is, or ends in, a Task with a Throws (the same test listed() uses), not just as type_text(inner). Add a driver test with such a Global and parameter.

Status: fixed: a Func's result that ends in a Task with a Throws is written in parentheses

## fbi-4 (medium): Reach is by spelling, so a Private generic or @Inline function nothing reaches gets its body published, or fails the build

note_names adds every identifier in the kept text to m_names, including member names in kept Class headers, parameter names, and Locals in kept bodies. wanted() then includes any candidate with a matching name, in any module. So a Private generic or @Inline Function that only shares its name with a Public Method, Field, or parameter is included verbatim, body and all, though no kept body calls it. Spec 11 says the .fbi holds 'the library's Private and Package declarations that those bodies reach', so that 'the rest of its code can stay closed'. If that function uses Embed, check_embeds fails the build with 'the library's interface file keeps this code, which programs compile themselves', which is wrong: nothing compiles it.

Fix: Find reach by symbol, not by spelling: for the identifier expressions inside a kept range, use the analysis's resolved symbol (and the types in kept headers), and include only the declarations of those symbols. At least, don't let member names, parameter names, or a body's Locals reach top-level declarations. Add a test with a Private generic whose name is a Public Method's.

Status: fixed: kept code reaches what each name refers to, as analysis resolved it; a declaration's own name, a member's, a parameter's, or a Local's reaches nothing, and only names analysis didn't resolve, like types in headers, reach by spelling

## fbi-5 (low): Stale comments: abi_hash's doc says two things, and write_interface says 'every Field'

abi_hash's doc comment starts with a leftover sentence from step 1 ('The ABI hash of an interface file's text after its first line, for the build that writes it.'), followed by the current one. interface.hpp's write_interface comment says the file holds 'every Field of the types it shows', but since step 1's review (decisions.md) it keeps every member of each type it shows (Private Methods, Properties, Operators, and Static Fields as their first lines) and every Extend.

Fix: Delete the first comment line above abi_hash. In interface.hpp, say 'every member of the types it shows, Private ones as their first lines, and every Extend'.

Status: fixed: abi_hash's comment is the one in interface.hpp, and write_interface's says every member and Extend, and the uses lines

## combo-1 (high): A prebuilt library built against another build of its dependency links silently and runs wrong or crashes

A .fbi/.fblib records only its own build: `// fbi <name> <version> <hash>`. abi_hash() covers the library's .fbi text and code, but not the builds of the packages it was compiled against. read_prebuilt_binary() (driver.cpp:2933) only checks that the .fblib header matches the .fbi header. A library's .fblib also carries linkonce_odr copies of its dependencies' code (llvm-nm on arena.fblib shows W fb.engine.world.World.Step(Float32) and the rest of World). So when a dependency is rebuilt and the prebuilt library isn't, the program links the library's stale code, compiled against the old layouts and signatures, with nothing to say so. Spec 11 says a program is built against one exact build of each library, that linking another is an error naming both builds, and that layouts can change freely between versions. That only holds if each library also records the builds it depends on.

Fix: Record each dependency's build (name, version, hash) that the library was compiled against: a line in the .fbi and .fblib, and part of the ABI hash. When a program links a prebuilt library, embedded or beside, compare each recorded build with the build of that package the program resolved. On a mismatch, stop with an error naming the library and both builds of the dependency. Add a CLI test that rebuilds a dependency and not its dependent prebuilt library.

Status: fixed: a library's interface file records each package it was built with, `// uses <name> <version> <build>`, a prebuilt one's hash or `source:` and a hash of a source one's files, which its ABI hash covers, and a program that has another build of one stops with an error naming the library and both builds, embedded or beside

## combo-2 (medium): A beside prebuilt library with bad bitcode kills fb with LLVM's bare message and leaves temporary folders

read_beside_names() and emit_beside_object() parse a beside library's bitcode in a fresh LLVMContextCreate() with no diagnostic handler. The main codegen context sets one at codegen.cpp:685. Without it, LLVMParseBitcodeInContext2's error goes to LLVM's default handler, which prints `error: <reason>` and exits the process. That skips the formatted error that names the file ("...isn't LLVM bitcode that this fb reads"), skips remove_temporary(folder) in build_program, and skips fb run's cleanup. The embedded path handles the same file properly.

Fix: Give both contexts a diagnostic handler that collects the message, as codegen.cpp:685 does, and return the error naming the prebuilt .fblib's path rather than <name>.dll. Better, report it before build_program creates its temporary folder. Add a test with a corrupt beside .fblib.

Status: fixed: a beside library's bitcode is parsed with a handler that keeps LLVM's error, which names its .fblib, and the build's temporary files are removed

## combo-3 (low): The ABI hash is never checked, so a .fbi edited by hand or a build of the other mode links silently

Spec 11 and decisions.md say the hash on the .fbi's first line covers the rest of the .fbi, the compiled code, the compiler's version, the target, and the mode, and that a build links the library of its own mode. No reader ever recomputes the hash. read_prebuilt_binary() only checks that the .fblib header's name, version, and hash equal the .fbi header's. So nothing checks that the files in lib/<target>/<mode>/ are that mode's (or target's, or compiler version's), or that the .fbi body is the one the binary was built with. A packaging mistake, or an edited .fbi, gives wrong code with no error.

Fix: When reading a prebuilt library, recompute abi_hash(.fbi body, .fblib code, this mode) and compare it with the header, or at least record the target, mode, and compiler version in the header and check them. Stop with an error naming the file when it doesn't match. Add tests for a .fblib in the wrong mode's folder and for an edited .fbi.

Status: fixed: a program recomputes a prebuilt library's ABI hash, of this fb, the target, and its folder's mode, its interface file, and its code, and a build that isn't the one its first line names is an error saying so

## combo-4 (low): Docs say a beside library has a metadata section with its target and mode, which it doesn't

libraries.md decision 3 says a beside library is built "with a metadata section (its interface's hash, its version, target, and mode)". decisions.md:958 says the same. The code (emit_beside_object, codegen.cpp:4757) writes an exported constant `fb.(build).<name>` holding "<name> <version> <hash>", plus " for <exe>" on Windows, in an ordinary data section. The target and mode aren't recorded, except inside the hash.

Fix: Reword libraries.md decision 3 and decisions.md's row to describe the `fb.(build).<name>` record (or add the target and mode to the record if they're meant to be read).

Status: fixed: libraries.md and decisions.md say the build is recorded as an exported `fb.(build).<name>`, its name, version, and ABI hash, and on Windows its executable, and the ABI hash covers the compiled code
