# The review of M7 step 5 (dynamic natives)

12 confirmed findings, each with its Status.

## link-1 (medium): On Linux, a dynamic native that only another dynamic native uses is dropped by --as-needed, and $ORIGIN in DT_RUNPATH doesn't cover it, so the program won't start

On Linux, link_program puts the dynamic natives on the command line as -l:<beside> and adds -Wl,-rpath,$ORIGIN. CI and Ubuntu link through Ubuntu's g++ (g++-14 on ubuntu-24.04). Its spec passes --as-needed by default, so a dynamic native the program's own code never calls gets no DT_NEEDED entry in the executable. lld writes the rpath as DT_RUNPATH by default (new dtags), and ld.so uses an executable's DT_RUNPATH only for that executable's own direct dependencies. Take a package that ships libengine.so, which NEEDs libphysics.so (a second dynamic native beside it), where the program calls only engine's functions. Both files are copied beside the executable, but at load time libphysics.so isn't found. Spec 11 says the executable finds every dynamic library the build copies beside it through $ORIGIN on Linux. Windows (the application folder) and macOS (dyld also searches the main executable's LC_RPATH) don't have this problem.

Fix: On Linux, wrap the -l: entries in -Wl,--push-state,--no-as-needed ... -Wl,--pop-state, so every dynamic native is recorded (this also matches rule 3: every one is the program's). Another option is -Wl,--disable-new-dtags, so $ORIGIN is a DT_RPATH, which also applies to the libraries' own dependencies. Add a driver or CLI test with two dynamic natives where the program calls only the one that depends on the other.

Status: fixed: on Linux the dynamic natives link inside --push-state,--no-as-needed, so the executable records each; checked by hand in WSL with a library that only another uses (dropped without it, runs with it), and the CLI test checks an uncalled one is recorded

## link-2 (low): The letter-case check for two dynamic natives sharing a name beside the executable folds only ASCII, so Édc.dll and édc.dll collide silently on Windows

read_dynamic compares the names beside the executable using std::tolower on each byte, which folds only ASCII letters. NTFS and APFS compare names ignoring case for non-ASCII letters too. Two packages whose DLLs differ only in the case of a non-ASCII letter get past the check (rule 7: an error at the second one's project.toml line). Both are copied to the same file, the second overwrites the first, and fb build prints a Copied line for each. The program then loads whichever library was copied last under both names.

Fix: Fold case with Unicode simple case folding of the UTF-8 name (the runtime or std text code probably already has a case fold to reuse), not byte-wise tolower. Add a test with a non-ASCII pair.

Status: fixed: names beside the executable compare case-folded and NFC-composed, through utf8proc, which the compiler now links; tested with élan and Élan

## link-3 (low): -o naming a dynamic native's file name: copy_beside overwrites the new executable with the DLL and fb build still reports success

copy_beside copies each dynamic native to output.parent_path()/beside without checking whether that path is the executable just linked. If -o names a file whose name matches a dynamic native's name beside the executable, in any letter case, the executable is replaced by the library. fb build already refuses an -o that would overwrite one of the program's own files. This case has no such check.

Fix: In fb build's -o checks (or in copy_beside before copying), fail when output's file name equals, ignoring letter case, the beside name of any of compilation.natives.dynamic. Add a CLI test.

Status: fixed: build_program stops before linking when the executable's name is one a dynamic library goes beside it as, in any letter case; tested

## link-4 (low): No test checks that a stale copy beside the executable is replaced, or that a dynamic native the program never calls is still copied

Rule 3 (every dynamic native of the program's packages is copied, even one the program never calls, and overwritten on each build) isn't tested. The driver test builds twice, but the second build copies over an identical file, so it would pass even if copy_beside skipped files that already exist. The CLI test's program calls every function it declares. The behavior is correct today: by hand, replacing app/build/dyncalc.dll with a text file and rebuilding restores the real DLL.

Fix: Between the driver test's two builds, write different bytes to the copy beside the program and check them after the second build. Add a second, uncalled dynamic native to a test package and check its file and Copied line.

Status: fixed: the driver test writes over the copy between its two builds, and checks the second build puts the library back

## load-1 (low): The fix suggested for a dynamic path with an extension keeps the 'lib' prefix and swaps the folder, so following it fails again

read_dynamic's 'leaves out its file's "lib" and extension' error, and report_no_archive's 'is a dynamic library, which [native] takes as ...' error, both build their example as "native/{target}/" + written.stem(). The stem keeps a 'lib' prefix, so the advice breaks the rule the same message states. It also throws away the folder the user wrote. On Linux and macOS, writing the file's real name (libengine.so, libengine.dylib) is the obvious first try, and that is exactly where the suggestion is wrong. Following it gives a second error: on Windows it looks for libengine.dll, on Linux for liblibengine.so.

Fix: Build the example from the written path: keep parent_path() (with {target} put back, or native.path's own parent), and take the stem with a leading 'lib' removed when the extension is .so or .dylib, or when the stem starts with 'lib'. Use one helper for both messages, and add tests for libengine.so and libs/x.dylib.

Status: fixed: the path it says to write keeps the folder written and drops the file's extension, and its "lib" unless it's a DLL's or a .lib's, in both messages; tested with libengine.so, libdyncalc.dll, libs/libengine.dylib, and libs/dyncalc.so

## load-2 (low): A static [native] path naming a DLL or .so that exists gets the plain 'isn't a static library' error, not the dynamic hint a missing one gets

report_no_archive's 'is a dynamic library, which [native] takes as x = { dynamic = ... }' hint runs only when find_archive finds nothing. find_archive accepts the written file when it exists, whatever its extension, so a .dll/.so/.dylib that is really there goes to the is_archive branch. That branch only says the file doesn't start as an archive does. So the less helpful message appears exactly when the user points at a real dynamic library.

Fix: In load's static branch, check extension_is(.dll/.so/.dylib) before find_archive, or before the is_archive failure, and report it as report_no_archive does. Add a test with a real .dll or .so at the static path.

Status: fixed: a static path whose file is a .dll, .so, or .dylib gets the dynamic hint whether it's there or not; tested with an existing real.dll

## load-3 (low): A dynamic path that names the import library or a folder gets 'there's no <path>.dll', with no hint

read_dynamic rejects only the .dll, .so, and .dylib extensions. A path ending in the import library's name (dyncalc.lib, libdyncalc.dll.a), which is a natural mistake on Windows, gets '.dll' added to it. A path naming an existing folder gets the same treatment, while the static branch says '... is a folder, not a static library' for that case. In both cases the message names a file nobody would write.

Fix: In read_dynamic, add .lib and .a (and .dll.a) to the extension rejection, with a message saying the path names the library without its import library's name. Check is_directory(root / written) before looking for the file and report it as a folder. Add tests for both.

Status: fixed: a dynamic path ending in .lib, .a, or .dll.a gets the same hint, and one naming a folder is a folder, not a dynamic library; tested

## load-4 (low): An executable given as a dynamic native is said to 'not start as a DLL does', which isn't true

dynamic_library_name returns NOT_A_LIBRARY for a PE file without IMAGE_FILE_DLL, and for an ELF PIE (DF_1_PIE). Both start exactly as a DLL or shared object does. read_dynamic turns every NOT_A_LIBRARY into 'it doesn't start as {kind} does', so for an executable the reason given is false and points the user away from the real problem. The PIE case is call (8)'s check, so Linux users hit it as soon as they pass an executable by mistake. The Linux PIE path was reviewed by reading, since it shares this message.

Fix: Add a library_problem_t value such as EXECUTABLE, returned for a PE without IMAGE_FILE_DLL, an ELF of type EXEC, a PIE, or a Mach-O MH_EXECUTE, and give it its own message in read_dynamic. Add a test using an executable image.

Status: fixed: the reader tells an executable (a PE without IMAGE_FILE_DLL, an ELF executable or PIE, a Mach-O MH_EXECUTE) from what isn't a library, and the driver says it names an executable; tested in the reader and the driver

## load-5 (low): No test checks call (3): a dynamic native the program never calls is still copied

Every test that builds (the driver's 'goes beside the program' test and the CLI test) calls the DLL's functions. The clash test never builds. So nothing checks that a package's dynamic native is copied when the program uses none of it. On Windows such a program has no import of the DLL at all, so a future change that copies only what the executable imports, or that reads the import table, would pass every test. The behavior works now: review5/load/unused, a program depending on calc that only does Print("hi"), builds, prints 'Copied dyncalc.dll beside the executable, which needs it', and gets build/dyncalc.dll, although objdump -p unused/build/unused.exe shows no dyncalc.dll import.

Fix: Add a driver test, or extend write_calc, with a second program that imports nothing from calc. Check that natives.dynamic still has dyncalc and that build_program puts the file beside the executable.

Status: fixed: the CLI test builds a program that calls none of its package's dynamic native, which is copied beside it and, off Windows, recorded by the executable

## read-1 (low): A 32-bit or big-endian shared object (or thin 32-bit Mach-O) is called 'not a dynamic library' instead of 'for another CPU', unlike a 32-bit DLL

elf_name rejects anything that isn't ELFCLASS64 + ELFDATA2LSB with NOT_A_LIBRARY, before the machine check, and mach_o_name does the same for any thin magic other than 0xFEEDFACF. So the most common wrong-binary mistake on Linux, shipping a 32-bit .so (i386 or armhf), gets the driver's 'isn't a dynamic library: it doesn't start as a shared object does'. The file is a shared object, just for another CPU. pe_name, meanwhile, reports a 32-bit DLL as OTHER_TARGET ('a DLL for another CPU than windows-x64's'), and so does an i386-only universal Mach-O. The header's own doc says OTHER_TARGET means 'It is one, but for another CPU', and the code contradicts it. No test covers a 32-bit file on any OS.

Fix: After the \x7FELF magic, treat EI_CLASS != 2 or EI_DATA != 1 as OTHER_TARGET, reading e_type from the right offset for that class (or simply returning OTHER_TARGET for any ELF with a different class or byte order). In mach_o_name, return OTHER_TARGET for MH_MAGIC (0xFEEDFACE) and the byte-swapped magics. Add tests for a 32-bit ELF, a 32-bit Mach-O, and a 32-bit PE.

Status: fixed: a 32-bit or big-endian ELF, a 32-bit or byte-swapped thin Mach-O, and a 32-bit PE are another target's; tested

## read-2 (low): In a universal dylib, the reader takes the first arm64-CPU slice, arm64e included, but lld links the arm64 slice, so the install names can disagree

mach_o_name matches universal slices by cputype only (*kind == cpu) and ignores cpusubtype. arm64e slices have the same cputype as arm64 (0x0100000C) with subtype 2. When a universal file lists an arm64e slice before the arm64 one, the reader returns the arm64e slice's install name. ld64.lld, building for arm64, skips arm64e slices and records the arm64 slice's install name. So fb's check, and the name it copies the dylib under, can describe a slice that isn't the one linked. If the slices' names differ, fb either refuses a library that links fine or copies it under a name the executable doesn't record.

Fix: In the universal loop, skip slices whose (cpusubtype & 0x00FFFFFF) is CPU_SUBTYPE_ARM64E (2) when the target is arm64, or prefer an exact arm64 (subtype 0) match before any other slice of that CPU. Add a universal test with an arm64e slice listed first.

Status: fixed: an arm64 build skips arm64e slices, as the linker does; tested with an arm64e slice first, and alone

## read-3 (low): A SONAME of '.' or '..', or an install name of '@loader_path/.' or '@loader_path/..', passes as a name beside the executable and fails later with a linker or copy error

read_dynamic accepts a SONAME with no '/' (line 2091) and an install name of '@loader_path/' plus any rest with no '/' (line 2081) as the file name beside the executable. '.' and '..' pass both checks, and so beside becomes '.' or '..'. On Linux, link.cpp then symlinks link-folder/dynamic/.. (failing silently) and passes -l:.., which ld.lld rejects with 'cannot open .../dynamic/..: is a directory'. On macOS, copy_beside copies onto build/.., which is a folder. Spec 11 says such a build 'stops with an error naming the package and the file'. Here it gets a linker or filesystem error that names neither the package nor the SONAME or install name.

Fix: Reject a beside name that is empty, '.', or '..' (in both the SONAME and the @loader_path branches) with the existing package-and-file messages, and add driver tests built from binary_images.hpp's elf_library and mach_o_library.

Status: fixed: a SONAME, or an @loader_path install name's file, of "." or ".." is an error naming the package and the file; tested on Linux and macOS

## Refuted

- read-4: The 64-bit universal header (0xCAFEBABF, fat_arch_64) is never tested.
