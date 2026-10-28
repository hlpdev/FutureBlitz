# The review of M7 step 3 (libraries beside the executable)

42 confirmed findings, each with its Status. The part that reviewed step 4's done-when (across the edge) is here too.

## drv-1 (high): A beside library with any Public Function or Method of two or more parameters fails to link on Windows

emit_beside_object marks each of the library's own functions dllexport. LLVM writes each one as a -export:"<name>" directive in the object, and lld splits an /export argument on commas. A mangled name like fb.two.t.Add(Int32, Int32) contains ", ", so the DLL's link (link_beside) fails. The executable has already been written by then, so fb build leaves a build/game.exe whose DLL was never made. The cli test only uses functions with zero or one parameter (Step, Count, Simulate) or generic ones, so it doesn't catch this.

Fix: Export the library's own symbols in a way that doesn't go through lld's comma-split /export parsing, for example a .def file with quoted names, or an --export-all-symbols link of the DLL. Or mangle names without ", " on the edge. If the DLL link fails, remove the executable too. Add a two-parameter Public Function and Method to the beside cli test.

Status: fixed: a beside library's file exports all of its own code with --export-all-symbols, not dllexport's /export directives

## drv-2 (high): A beside library using another beside library's Global: a link error in one order, a panic or wrong value in the other

beside_binary compiles each source beside library on its own with compile_package(library.root). It doesn't know which of the library's dependencies the program also puts beside, so those dependencies' code and Globals are linked into the library's bitcode as shared (linkonce) code. The DLL then imports them from the executable, and link_beside gives the executable its own weak_odr copy, besides the one in the dependency's own DLL. With engine listed before util, engine.dll's references to util's Public Global can't be resolved. With util listed first, the executable has a second copy of util's Global that is never initialized. The cli test 'a beside library's own dependencies go in the executable or beside it too' only lists engine before util, and its Global is set to a constant through functions, so it misses both cases.

Fix: Build each beside library knowing which packages the program puts beside. The modules of those dependencies are beside in its compilation too: IMPORTED, reached through the dependency's file or the executable's thunks and Global accessors, and never linkonce copies. Add tests that list the dependency first and use its Public Global and a Global with a run-time initializer.

Status: fixed: the executable makes its thunks and accessors once every beside library is linked, a library's copies of another's code and Globals are that one's, and one library takes another's from its file: through its import library on Windows, and elsewhere loaded first with RTLD_GLOBAL

## drv-3 (medium): Two programs built into one build folder share one beside DLL, which is bound to the last executable; the other crashes

build_program always writes the beside library as <output folder>/<name>.dll. On Windows the DLL imports the runtime and std by the executable's file name, which is the name of the executable it was linked against. Spec 11 says a package's build writes build/<name> after the package or after another main file it was given, so two programs of one package share build/. The second build overwrites engine.dll with one that imports from the second executable. The build string is the same, so the load-time check passes. Windows then maps the other .exe as a module, and the first program crashes.

Fix: Detect this at build time: an existing <lib>.dll beside another executable built for a different executable name. Or record the executable it was built for in the DLL's build record, which fb_load_beside checks, so the program stops with a named error. Or give each executable's beside libraries file names that can't clash.

Status: fixed: on Windows a beside library's build record names the executable it was built for, which the loader checks, naming both

## drv-4 (medium): A beside library's Extern Functions that the executable calls directly don't link: its static and system libraries are left out of the executable

natives_of skips a beside package's archives and system libraries for the executable, because they link into the library's file. But is_prebuilt (lower.cpp) leaves an Extern Function in the executable, and the same goes for generic, @Inline, and Const code that calls one. So the executable calls the C symbol itself, without the library that defines it. The error that results blames the project.toml, which does list the library.

Fix: Link a beside library's system libraries into the executable when it calls the library's Externs. For archives, reach the Extern through a thunk in the library's file, or link the archive into the executable too. Add a test where the program calls a beside library's Public Extern, and a generic that uses one.

Status: fixed: a beside library's static natives are in the executable, as its dependencies are, and its system libraries link into both

## drv-5 (medium): fb build to a non-ASCII folder writes a DLL that imports from '0.exe', so the program can't start

When the output path isn't ASCII, run_linker links the executable as a numbered file in its link folder, then moves it. The import library made with --out-implib records that temporary name, and the beside library links against it. So lib.dll imports the runtime from "0.exe", which never exists. fb run works only because its temporary folder is ASCII. The runtime's message says the file isn't there, but it is.

Fix: When the executable is linked through the link folder, give the temporary file the executable's real file name (in its own subfolder), or set the import library's DLL name explicitly. Add a non-ASCII-path beside test.

Status: fixed: an executable linked through the link folder keeps its own name there, its import library is written in the link folder and moved, and on Windows an executable with libraries beside it has to have an ASCII name

## drv-6 (low): Two beside libraries whose names differ only in letter case aren't a build error; one overwrites the other

The clash checks compare a beside library's file with the executable, with dynamic natives, and with the OS's libraries, but not with the other beside libraries. Spec 11 and decisions.md say files going beside the executable under one name, compared ignoring case, are an error naming both.

Fix: In build_program's clash loop, also compare each beside file with the beside files before it, using folded_name. Add a test.

Status: fixed: two beside libraries' files are compared, case-folded, and the build stops naming both

## drv-7 (low): A beside library named after one of its own system libraries isn't caught; its DLL imports from itself

system_library_name checks a beside file against compilation.natives.system. natives_of builds that list without beside packages' system libraries, so a beside library's own system libraries (or another beside library's) are never compared. Spec 11 says a beside library's file can't take the name of a library the executable takes from the OS.

Fix: Check against every package's system libraries, beside libraries' own included, for example a list collected separately from natives_of's output.

Status: fixed: as drv-4, every package's system libraries are the program's, a beside library's too, which its file's name is checked against

## drv-8 (medium): A program run by its main file in a package without main.fb has its link = "beside" ignored, with a warning that it isn't a program

The loader decides 'program' as index == 0 && !m_packages.front().main.empty(). package.main is only set from project.toml's main or a main.fb on disk. `fb run <pkg>/app.fb` builds the package as a program, which spec 11 allows ("or after another main file it was given"). But when there's no main.fb, every link is ignored, the library is embedded, and the warning says only a program's own project.toml decides.

Fix: Tell the loader when compile_package was given a main file, and treat the root package as the program then too.

Status: fixed: the loader treats the main package as the program when it's given a main file

## drv-9 (medium): A bundled C library that only a beside library uses isn't reported, so its license notice isn't printed

build_program links bundled_libraries(module, prebuilt + beside), but report_built lists bundled_libraries(module, prebuilt_binaries(compilation)), which leaves out the beside binaries. A program whose beside library calls XxHash prints no "Bundled xxHash ...; ship its notices" line, though embedding the same library prints it.

Fix: Give report_built the beside libraries' binaries as well, for example a driver function that returns every linked binary, prebuilt and beside.

Status: fixed: build_program says which bundled libraries it linked, the beside libraries' among them, which fb build prints

## drv-10 (low): fb build doesn't print the beside library files the program needs

Spec 11 says the build puts a beside library beside the executable, "printing each file the program needs, as it prints the bundled C libraries". libraries.md says the same. report_built prints only the dynamic natives ("Copied ... beside the executable, which needs it"), never engine.dll.

Fix: Print a status line for each compilation.beside file in report_built, and add a check to the cli test.

Status: fixed: fb build prints "Built <file> beside the executable, which needs it" for each

## drv-11 (low): A source beside library that can't be built gets an error without a location, which fb check doesn't show either

When build_library fails with diagnostics, beside_binary reports only the first diagnostic's message text, without its file or line. For an Embed in a generic Function, that message is "the library's interface file keeps this code ...", and the user can't find "this code". fb check on the program and on the library both pass. The same library embedded works.

Fix: Pass the library's diagnostics up, rendered with their sources, as the program's are, or name the file and line in the message.

Status: fixed: the library's error is shown at its line, and fb check on a library runs its interface file's checks too

## load-1 (high): A beside library whose code has a function with two or more parameters doesn't link on Windows

emit_beside_object marks each of the library's own functions dllexport. LLVM turns that into a /EXPORT: directive with the FB name, for example 'fb.lib2.lib2.Sum(Int32, Int32)'. lld reads the comma in that name as the start of an export attribute (',DATA', ',PRIVATE') and rejects the directive. Any external function whose name has a comma fails the DLL's link, Private ones included: every function with two or more parameters, a tuple parameter, or a Func(A, B) parameter. Most real libraries have one, so on Windows almost no library can be beside. The step's test has no such function, so it passes. The executable is linked first and is left behind (see load-10).

Fix: Don't export the DLL's functions with dllexport directives under names that can hold commas. Two options: link the DLL with -Wl,--export-all-symbols, or a .def file, which take any name, as the executable's link already does; or export them under comma-free names (escaped or hashed) and look those up with GetProcAddress. Add a cli test with a two-parameter Public function, a two-parameter Method, and a Private one.

Status: fixed: a beside library's file exports all of its own code with --export-all-symbols, not dllexport's /export directives

## load-2 (medium): Reading a field of a beside library's Struct Global stops the build

link_beside sends every use of a library Global through its accessor, but only when the user is an Instruction. The builder folds a field access on a Struct-typed Global into a constant getelementptr, which is not an Instruction, so the build stops. This happens on any read or write of a field of a Public Global, or a Static Field, whose type is a Struct. The message talks about LLVM addresses and doesn't tell the user what to change.

Fix: Before rewriting uses, turn each ConstantExpr user of the Global into an instruction (or walk the ConstantExpr users and rebuild them at each instruction that uses them), then put the accessor call in. Add a test with a Struct Global and a Struct Static Field across the edge.

Status: fixed: a Global's uses through constant expressions, like a Struct Field's address, become instructions that call its accessor

## load-3 (medium): A beside library that uses a Public Global of another beside library doesn't link

When both libraries are beside, the dependent library's file marks the other library's Global dllimport and expects the executable to have it. The executable never defines it: link_beside deletes that Global after redirecting its own uses to the accessor. So the DLL import is unresolved. The docs say a library's dependencies can be listed beside too and the library uses them from the executable. That works for functions but not for Globals. The step's test (beside_deps) only calls util's functions.

Fix: Rewrite a beside library's uses of another beside library's Globals through that Global's fb.(address) accessor, which the executable has as a thunk, in emit_beside_object too. Or have the executable define a forwarding accessor and redirect the DLL to it. Add a Global to the beside_deps test.

Status: fixed: the executable makes its thunks and accessors once every beside library is linked, a library's copies of another's code and Globals are that one's, and one library takes another's from its file: through its import library on Windows, and elsewhere loaded first with RTLD_GLOBAL

## load-4 (medium): A beside library with a [native] static archive can't be built

emit_beside_object lists every declaration the library's file uses as something it takes from the executable, including its own Extern C functions. build_program then gives the executable '-u c_add', and the DLL marks c_add dllimport. natives_of leaves the beside package's static archives out of the executable, because they link into its file, so the executable's link can't find the symbol. With link = "embed" the same package works.

Fix: Leave out of `imports`, and so out of exports.symbols and dllimport, every declaration that the library's own [native] libraries define, that is, its Extern functions bound to a Lib of its own package. Only the runtime's, std's, shared code's, and other packages' symbols should come from the executable. Add a beside test with a static native.

Status: fixed: a beside library's static natives are in the executable, as its dependencies are, and its system libraries link into both

## load-5 (medium): A beside program built to a path that isn't ASCII can't load its library: the import library names a temporary 0.exe

When the output path isn't ASCII anywhere along it, run_linker links the executable into the link folder as '<n>.exe' and moves it afterwards. --out-implib writes the import library under that temporary name, so every beside DLL imports from '0.exe'. At run time the executable is not loaded under that name, LoadLibrary fails, and the loader reports that the file isn't there, though it is. A user whose project or home folder has a name outside ASCII hits this on every beside build. The build itself reports success.

Fix: When the executable is linked under a temporary name, link it under a temporary folder but with its real file name, when that name is ASCII. Or pass the import library's DLL name explicitly (for example -Wl,--out-implib together with a .def LIBRARY line, or rewrite the import library). The DLL must import from the executable's real file name. Add a cli test that builds a beside program into a non-ASCII folder.

Status: fixed: an executable linked through the link folder keeps its own name there, its import library is written in the link folder and moved, and on Windows an executable with libraries beside it has to have an ASCII name

## load-6 (medium): A temporary folder outside the ANSI code page breaks every beside build: the import library's path reaches the linker as UTF-8

The import library is an output that doesn't exist yet when link_program runs. linker_path's fallbacks don't work for it: GetShortPathNameW fails on a missing file, and copy_file has nothing to copy. So the path under the driver's temporary folder (temp_directory_path()/futureblitz/...) goes to the C++ driver as raw UTF-8, which the driver reads in the ANSI code page. The other paths in the link use short names and work. The embedded build in the same environment links fine.

Fix: Write the import library into the link folder (folder.file(".lib"), which linker_temporary_folder keeps ASCII), then move or copy it to exports->import_library after the link. Or have build_program put program.lib under linker_temporary_folder() too.

Status: fixed: an executable linked through the link folder keeps its own name there, its import library is written in the link folder and moved, and on Windows an executable with libraries beside it has to have an ASCII name

## load-7 (medium): Two programs that share a beside library in one folder: the second build's DLL binds to the other executable, and the first program crashes

Building a second program into the same folder silently overwrites engine.dll with a copy that imports from the second executable. The loader checks only 'name version hash', which is the same for both. When the first program runs, LoadLibrary pulls the other executable in as a second module, and the library uses that executable's runtime and type records. The first program segfaults. If the other executable isn't there, the loader says the DLL 'isn't there, or can't be loaded', though it is there and is the right build. Shipping a game and its editor beside one engine.dll is a natural use of beside libraries.

Fix: Record in the DLL the executable name it was linked against, for example in fb.(build) or a second exported string. Read it before resolving imports (LoadLibraryExW with LOAD_LIBRARY_AS_IMAGE_RESOURCE or DONT_RESOLVE_DLL_REFERENCES, or by parsing the import table) and stop with an error naming both executables when it doesn't match. Add a cli test for this.

Status: fixed: on Windows a beside library's build record names the executable it was built for, which the loader checks, naming both

## load-8 (medium): A beside library's bundled C libraries get no 'Bundled ... ship its notices' line, though the executable links them

build_program now counts the beside binaries when it picks the bundled libraries to link (the `all` vector). The CLI's report still asks only prebuilt_binaries(compilation). So a bundled library that only a beside library uses, such as BLAKE3 through std.crypto, is linked into the executable, but fb says nothing about shipping its license notice. With the same library embedded, the line is printed.

Fix: In the CLI's report, add the beside libraries' binaries (from beside_binary, or those that build_program used) to the binaries passed to bundled_libraries, as build_program does.

Status: fixed: build_program says which bundled libraries it linked, the beside libraries' among them, which fb build prints

## load-9 (low): A beside program launched by its 8.3 short name is wrongly stopped as renamed

The Windows check compares GetModuleFileNameW's last component with the name the executable was built with. Launched by its short name, the module is called LONGPR~1.EXE, so the check stops the program. The Windows loader itself does bind a DLL's imports from 'longhostname.exe' to the running image in that case: a C host and plugin built with the same gcc and run by short name share the host's state (prints 43).

Fix: Compare the long form of the module path (GetLongPathNameW on GetModuleFileNameW's result) with `executable`.

Status: fixed: the loader takes the executable's long name before comparing it

## load-10 (low): When a beside library's link fails, the new executable is left in the build folder

build_program links the executable first and then each beside DLL. If a DLL's link fails, as in load-1, fb reports the failure but leaves the freshly linked <name>.exe in build/, without its library or beside an older one. Running it then fails with 'isn't there' or a build-mismatch error, after the build already said it failed.

Fix: Link the executable and the DLLs into the temporary folder and move them into place only when every link has succeeded. Or remove the executable when a later link fails.

Status: fixed: an executable whose beside library can't be linked is removed

## load-11 (low): fb build doesn't name the beside library's file, which libraries.md says it prints

libraries.md says that link = "beside" builds the library file, copies it beside the executable, and that fb prints each file the program needs. fb build prints a 'Copied <x> beside the executable, which needs it' line for each dynamic native (mul.dll), but nothing for the beside library's own file.

Fix: Have the CLI print a status line for each compilation.beside entry (its beside_file name), or change the doc.

Status: fixed: fb build prints "Built <file> beside the executable, which needs it" for each

## load-12 (low): libraries.md says the runtime registers each Class by name, which the code and decisions.md contradict

The ABI section says a generic instance made in two binaries is one type because 'the runtime registers each Class by its full name'. This step does it another way: the executable keeps one weak_odr copy of every type record, and the DLL declares them. decisions.md records that as 'without a registry of names'. The runtime has no such registry; object.cpp's registry_t only tracks objects for leak reports.

Fix: Reword that bullet: each instance's type record is the executable's (weak_odr), and every binary uses that one.

Status: fixed: libraries.md says the executable links each beside library's bitcode, so every instance and type record is the executable's

## load-13 (low): The tests miss step 4's done-when and the cases that fail above

Step 4's done-when says a generic instance made on both sides is one type. The beside test only passes Pair(1, 2)'s Int[] across the edge. It never checks `Is` on a generic Class instance made in the library against one made in the program, though that works. The tests also have no function with two or more parameters (load-1), no Struct Global (load-2), no Global shared between two beside libraries (load-3), no static native in a beside library (load-4), and no non-ASCII path (load-5, load-6), which is why those failures pass ctest.

Fix: Extend the beside test: a generic Class instance made in the library and checked with `Is` in the program and the reverse, a two-parameter function and Method, a Struct Global's field, and a beside-beside Global. Add the regression tests for load-1 to load-8.

Status: fixed: tests of the cases above, step 4's done-when (a generic instance made on both sides is one type, and a Class extending a library's), and each finding's program

## load-14 (low): On macOS the loader looks beside the symlink the program was launched through, not beside the executable (from reading; not run)

The macOS branch builds the library's path from _NSGetExecutablePath. Apple documents that this may give a symbolic link rather than the real file, so a program started through a symlink, for example /usr/local/bin/game pointing to /opt/game/game, looks for /usr/local/bin/libengine.dylib. Linux's /proc/self/exe resolves symlinks, and the library's dynamic natives use @loader_path, which resolves too. So only the beside library itself goes missing, and only on macOS. Not reproduced, because only Windows runs here.

Fix: Pass _NSGetExecutablePath's result through realpath() before taking its folder.

Status: fixed: on macOS the loader takes the executable's real path, through any link, before looking beside it

## gen-1 (high): Any Public Function or Method with two or more parameters, and any Operator, breaks the beside link on Windows

emit_beside_object marks each of the library's own functions dllexport, so LLVM writes a /EXPORT:<name> directive for each one. FB's symbol names carry the signature, like fb.lib.lib.Add(Int32, Int32). lld reads the comma in that name as the separator between /export's options and rejects the whole directive. Any function, Method, or Static Method with 2+ parameters, and every Operator, hits this, so most real libraries can't go beside. The existing tests only use one-parameter or generic functions (Step(body), Pair<T>), so they don't catch it. Importing such names from the executable works, because --export-all-symbols doesn't go through /export parsing.

Fix: Don't export through dllexport on Windows for names that /export can't hold. Options: link the DLL with --export-all-symbols, as the executable already is, or give each export a comma-free alias, such as a hash or numbered name that the slot table looks up by. Add a test with a multi-parameter function, a Method, and an Operator beside.

Status: fixed: a beside library's file exports all of its own code with --export-all-symbols, not dllexport's /export directives

## gen-2 (high): Debug build crashes fb (LLVM ERROR) when a program calls a beside function that returns a 4+ eightbyte Struct or tuple

thunk() marks its call through the slot musttail. At -O0 the x86-64 backend demotes a large aggregate return to sret and then can't keep the musttail. LLVM aborts the whole compiler with "failed to perform tail call elimination on a call site marked musttail", so there's no FB message. Because the process aborts, the build's temporary folder (%TEMP%/futureblitz/<name>-xxxx with program.o and beside0.o) is also left behind. Release builds are fine. A Struct of four Doubles or Ints, the shape of a Vec4, a Quaternion, or a Color, is enough. Three eightbytes work (a 3-Double Struct, a 3-Int tuple). AArch64 and SysV likely hit the same sret demotion, but only Windows was run.

Fix: Use an ordinary call (tail, not musttail) in the thunk, or musttail only where the backend can honour it. Add a debug-mode test that returns a large Struct across the edge.

Status: fixed: the thunk's call is a tail call, not musttail, with the function's calling convention and attributes, which declarations keep too

## gen-3 (high): A program that reads or writes a Field of a beside library's Struct or tuple Global fails to build

link_beside replaces each use of a library Global with a call to its accessor, but only where the user is an Instruction. Reading P.Y, a Field past the first, on a Struct Global, or Pair.1 on a tuple Global, gives a constant GEP: a ConstantExpr, not an instruction. So the build stops with "is used where only its address can go". P.X (field 0) folds to the Global itself and works, which hides the bug. The same goes for any shared code linked from a library's bitcode that does this. The message also reads like the user's mistake, when the fault is in the compiler. Step 4's done-when names Globals' accessors, but the tests only use Int Globals.

Fix: Before rewriting uses, turn ConstantExpr users into instructions (recursively, e.g. by walking constant users and rebuilding each as an instruction before its own instruction user), or have codegen never fold an IMPORTED beside Global into constant GEPs. Add Struct and tuple Globals to the beside test.

Status: fixed: a Global's uses through constant expressions, like a Struct Field's address, become instructions that call its accessor

## gen-4 (high): A beside library can't use another beside library's Public Global

emit_beside_object imports every Global it declares straight from the executable as data (dllimport on Windows). But a Global of another beside library isn't in the executable: link_beside deleted it and left only the accessor fb.(address).<name>. So the DLL references a symbol nobody exports. On Linux and macOS the .so/.dylib would also have an unresolved data symbol, since the other library is opened RTLD_LOCAL. That breaks spec 11's "A Global of a library is reached through its library" when the user is another beside library.

Fix: In emit_beside_object, reach Globals that belong to another beside package through their fb.(address) accessor, which the executable thunks and exports, just as the executable reaches them. The beside_binary_t list, or the build's package set, tells which declared Globals those are. Add the case to the beside_deps test.

Status: fixed: the executable makes its thunks and accessors once every beside library is linked, a library's copies of another's code and Globals are that one's, and one library takes another's from its file: through its import library on Windows, and elsewhere loaded first with RTLD_GLOBAL

## gen-5 (medium): On Windows, a beside library can't use a ThreadLocal Global of a package embedded in the executable

A beside library's own dependencies that the program doesn't list beside are in the executable, and the DLL imports their Globals as data. A ThreadLocal one is accessed with SECREL TLS relocations, which can't be imported across a PE image. So lld refuses to link the DLL. (Even if it linked, a dllimported TLS symbol would point at the exe's TLS template, not the thread's copy.) Plain Globals of the embedded package work. Std's own ThreadLocals (std.math's generator) aren't affected, because the DLL reaches them only through shared functions the executable has.

Fix: Give the executable an address function for each ThreadLocal Global that a beside library imports (like the DLL's fb.(address) accessors), and have emit_beside_object rewrite the DLL's uses of imported thread-local Globals into calls to it. Add a test.

Status: fixed: a ThreadLocal a library takes from the executable is reached through an fb.(address) function the executable makes

## gen-6 (low): Beside libraries load in project.toml order, not after the libraries they depend on

generate_beside_loads loads the libraries in m_options.beside order, which follows the program's [dependencies] listing. Spec 11 says the executable loads them "each after those it depends on". The visible effect: when files are missing, the dependent library is named first.

Fix: Sort options.beside by the package dependency graph (dependencies first) before codegen, or fix the spec sentence if listing order is what's wanted.

Status: fixed: beside libraries are listed, linked, and loaded after those they depend on

## gen-7 (low): MIR's beside indices are written by lowering but never read

lower.cpp fills global_t::beside, mir_function_t::beside, mir_library_module_t::beside, and mir_module_t::beside through beside_of(). But no code in codegen, the driver, or the MIR printer reads them: codegen finds slots and accessors by symbol name from the library's bitcode. beside_binary_t::package isn't read by codegen either. The comments say each field is "the library beside the executable whose file has it, by its index in mir_module_t's beside", which suggests codegen uses it. It doesn't, and --show mir doesn't show it.

Fix: Remove the four MIR fields and beside_of(), or use them in codegen, and print them in --show mir.

Status: fixed: the indexes are gone; mir_module_t's beside, which codegen reads, says a program links

## gen-8 (low): Tests don't check step 4's "a generic instance made on both sides is one type"

The beside test's only generic is the Function Pair<T>, and its only Is check is on a non-generic Class (heavy Is Heavy). Nothing checks that a generic Class instance made in the DLL and the same instance made in the executable are one type, for Is, As, and Interfaces. It works today (checked by hand), but the done-when is untested. Struct and tuple Globals and multi-parameter functions are also missing from the test, which is why gen-1 and gen-3 slipped through.

Fix: Add a generic Class instance made in both binaries to "a library beside the executable shares one runtime with it", plus a Struct Global Field read and a two-parameter Public Function.

Status: fixed: tests of the cases above, step 4's done-when (a generic instance made on both sides is one type, and a Class extending a library's), and each finding's program

## edge-1 (high): On Windows, a beside library that has any function with two or more parameters fails to link

emit_beside_object marks every external function dllexport. The symbol names have commas in them, like 'fb.lib.lib.Add(Int32, Int32)'. lld reads the -export: directive from .drectve and splits it at the comma, so the DLL link fails. The failure covers Public and Private Functions, Methods, binary Operators, and New with two or more parameters. A beside library is unusable in practice on Windows. Neither beside test has a function with two parameters, which is how this got through.

Fix: Export each function under a name with no commas or spaces (for example an escaped or hashed alias of the symbol, or exports by ordinal), and have the GetProcAddress names table use the same names. Add a CLI test with a two-parameter Function, Method, and binary Operator beside.

Status: fixed: a beside library's file exports all of its own code with --export-all-symbols, not dllexport's /export directives

## edge-2 (high): ThreadLocal Globals of a package or std module that only a beside library uses are never initialized

Sometimes only the beside library uses a package or std module, which then sits in the executable: unlisted, or listed with embed. In that case its ThreadLocal Globals' makers never run, and they read as zero. std's Rand() returns 0 every time. Int[].Shuffle() spins at 100% CPU forever in debug and release, presumably because the zero-state generator loops. Ordinary Globals of such a package work.

Fix: When the executable takes in a beside library's shared code, also generate the thread makers and init for every ThreadLocal Global it then holds: the packages and std modules that only the beside library's bitcode brings in. Alternatively, put those modules in the program's MIR so generate_thread_local_functions covers them. Test it with a std ThreadLocal (Rand) used only by a beside library.

Status: fixed: in a build that links, a shared ThreadLocal's initializer is always lowered, so the maker the linker keeps runs it; prebuilt embedded libraries had the same bug

## edge-3 (medium): A program that uses a field of a beside library's Struct Global doesn't build

link_beside reroutes each library Global's uses through its address accessor. It only handles uses that are instructions. A Struct Global's field access is a constant GEP expression on the global, so the build stops with an internal-sounding error. 'Public Global Origin: Vec2' is a common thing for a library to have.

Fix: Turn constant-expression users of the global into instructions before rewriting them (walk the ConstantExpr users, put an equivalent instruction in front of each instruction that uses them, then replace), or build these GEPs as instructions. Add a test with a Struct Global.

Status: fixed: a Global's uses through constant expressions, like a Struct Field's address, become instructions that call its accessor

## edge-4 (medium): A beside library can't use another beside library's Globals, or any other package's ThreadLocal, directly

The executable reaches a beside library's Globals through fb.(address) accessors, but another beside library's code still references them as dllimport data from the executable, which no longer defines them. A ThreadLocal of a package in the executable can't be imported into a DLL at all (SECREL). Only Globals of packages in the executable work.

Fix: In emit_beside_object, rewrite uses of other packages' Globals that are beside libraries, and of any ThreadLocal not defined in the DLL, to call accessor functions (the executable can export accessors for its own ThreadLocals). Add a test where one beside library reads another's Global and a ThreadLocal.

Status: fixed: the executable makes its thunks and accessors once every beside library is linked, a library's copies of another's code and Globals are that one's, and one library takes another's from its file: through its import library on Windows, and elsewhere loaded first with RTLD_GLOBAL; ThreadLocals as gen-5

## edge-5 (medium): A beside library with a static [native] archive doesn't build

emit_beside_object returns every undefined symbol in the library's object as something it takes from the executable, including the C symbols of its own static archive. build_program then adds each one that doesn't start with 'fb.' to exports.symbols, so the executable is linked with -u for it. natives_of (driver.cpp:2428) leaves the archive out of the executable's link, so that link fails with the C-symbol error. Dynamic natives work (n1d).

Fix: Only count as imports the symbols the DLL doesn't get from its own natives: drop C symbols that its own archives, dynamic natives, or system libraries define, or only add -u for runtime symbols. Add a test with a beside library that has a static native.

Status: fixed: a beside library's static natives are in the executable, as its dependencies are, and its system libraries link into both

## edge-6 (low): fb build --show llvm fails for any program with a beside library

Printing the IR of a program that has a beside library gives invalid IR: the library's function declarations get internal linkage with no body. The same program embedded prints its IR.

Fix: Give emit_ir the same beside and exports options build_program gives emit_object, or skip the beside thunks consistently there.

Status: fixed: a program with beside libraries links, so its LLVM IR view declares their functions

## edge-7 (medium): fb build prints neither the beside library's file nor the notices of the C libraries it bundles

Spec 11 and libraries.md say the build prints each file the program needs beside it, as it prints bundled libraries. report_built prints only natives.dynamic, so lib.dll is never mentioned. Its Bundled lines come from prebuilt_binaries, which leaves beside libraries out, so a beside library's bundled C libraries (SQLite here) get no 'ship its notices' line, from source or prebuilt. Users won't learn they have to ship the license notices.

Fix: In report_built, print each compilation.beside library's file, and pass the beside binaries (or their modules) to bundled_libraries for the notices, as build_program does for linking.

Status: fixed: build_program says which bundled libraries it linked, the beside libraries' among them, which fb build prints; the files as drv-10

## edge-9 (low): libraries.md still says the runtime registers each Class by its full name

The ABI section says one type per instance comes from 'the runtime registers each Class by its full name'. Step 3's code instead keeps one weak_odr copy of each shared record in the executable, and decisions.md's new row says this works 'without a registry of names'.

Fix: Reword the bullet: every binary uses the executable's one copy of each generic instance and type record, so Is, casts, and Interfaces agree.

Status: fixed: libraries.md says the executable links each beside library's bitcode, so every instance and type record is the executable's

## edge-10 (low): The beside tests don't check what the step 3 and step 4 done-whens say

The done-when says a missing or mismatched file stops the program 'with an error naming the file', but the test checks only substrings that leave the name out. Step 4's 'a generic instance made on both sides is one type' isn't tested: there is no generic Class made in both binaries and checked with Is. Pair<T> only returns arrays. No test has a function with two parameters, a Struct Global, a beside library depending on another beside library's Global, or a beside library with a static native, which is how edge-1 through edge-5 got through.

Fix: Check that missing.output and mismatched.output contain engine.dll. Add a generic Class made in the library and in the program with Is and a Static, a two-parameter Function, Method, and Operator, a Struct Global, a beside library using another beside library's Global, and a static native.

Status: fixed: tests of the cases above, step 4's done-when (a generic instance made on both sides is one type, and a Class extending a library's), and each finding's program
