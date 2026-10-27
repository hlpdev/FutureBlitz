# The review of M6 step 15's compiler part: the Embed, EmbedText, and EmbedFolder built-ins that put files into the program at compile time

6 confirmed findings: 0 high, 5 medium, 1 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## codegen-1 (medium): An Embed passed to an overloaded call reads the file again for every overload tried, and nested calls multiply the reads

Status: fixed in dd496695 (the loader reads each file once and keeps its bytes, by its path in the program, as a shared_ptr<const std::string> that embedded_file_t holds, and sema's embed_index() gives each distinct file or folder one place in analysis_t::embeds, however many times an overloaded call checks the argument), with driver_tests' "an embedded file is read once, and its embeds share its bytes": F(F(F(Embed(...)))) over three overloads, and another Embed and an EmbedFolder of the file, give two places in analysis_t::embeds (127 before) with the same bytes. The finding's shape with an 8 MiB file and 16 more Embeds builds in 1.44 s (463 s before)

Where: compiler/src/sema.cpp:20755

check_embed calls m_embeds->file / m_embeds->folder, and pushes a new full copy of the bytes onto m_analysis.embeds, every time the call expression is checked. Overload resolution checks an argument once for each candidate, so `F(Embed("x"), ...)` with N overloads reads the file and copies its bytes N times. When overloaded calls are nested, the outer trials re-check the inner ones, so the count grows as N^depth. The object file still gets one copy, because static_string dedupes by text, so the output is right. The cost is compile time and compiler memory. Measured with fb.exe on Windows: the 1 MiB program below takes 13.9 s to build, and 0.54 s when the Embed is first put in a Local. The same shape with an 8 MiB file took 463 s. One level of 11 overloads on a 64 MiB file took 28.3 s and peaked at 1245 MB, against 2.4 s and 476 MB with a Local. No test calls an embed inside an overloaded call.

```
// one.txt beside it: 1 MiB of 'a' (head -c 1048576 /dev/zero | tr '\0' 'a' > one.txt)
Import std.core
Function F(a: UInt8[], b: Int8): UInt8[] => a
Function F(a: UInt8[], b: Int16): UInt8[] => a
Function F(a: UInt8[], b: Int32): UInt8[] => a
Function F(a: UInt8[], b: String): UInt8[] => a
Function F(a: UInt8[], b: Bool): UInt8[] => a
Function F(a: UInt8[], b: Int64): UInt8[] => a
Print(F(F(F(Embed("one.txt"), 1 As Int64), 1 As Int64), 1 As Int64).Count)
```

Expected: Prints 1048576 after a build about as fast as the same program with `Local bytes = Embed("one.txt")` passed in (0.54 s), with the file read once.

Actual: Prints 1048576, but `fb run` takes 13.9 s to build ("Finished debug build in 13.94s"). With an 8 MiB one.txt the build took 463 s.

Suggested fix: Read each resolved path once: cache the bytes in loader_t by its program path, for example as std::shared_ptr<const std::string>, and have embed_folder use the same cache. In check_embed, when info(id).embedded is already set, or when analysis.embeds already holds that path, reuse that index instead of reading and pushing again. Add a test with an Embed argument to an overloaded call, nested.

Skeptic: I put a 1 MiB one.txt in rv-embed/codegen/c1 and ran the finding's program with `fb run`. It prints 1048576, but the build says "Finished debug build in 13.94s". The same program with `Local bytes = Embed("one.txt")` passed in builds in 0.50s. Build time grows with nesting depth: one level of the overloaded F takes 0.68s, two take 2.15s, three take 13.94s, so the cost multiplies with each level. The code matches the finding. In sema.cpp, check_embed (lines 20687-20760) calls m_embeds->file or m_embeds->folder and pushes a full copy onto m_analysis.embeds every time the expression is checked. Nothing in it reuses info(id).embedded when it's already set, or an earlier read of the same path, so each overload trial reads the file and copies its bytes again. The output is correct; the cost is compile time and compiler memory.

## codegen-2 (medium): Each Embed call keeps its own copy of the file's bytes in the compiler, so memory grows with the number of calls

Status: fixed in dd496695 (as codegen-1, and lowering puts each file's bytes in one Global String, embedded_text(), which every embed of the file makes its UInt8[] from), with mir_tests' "an embedded file is one Global String, whose bytes that aren't UTF-8 print by value": two Embeds of the file, one as "./logo.bin", read @Embedded, which the module has once

Where: compiler/src/sema.cpp:20758

In the executable, one file's bytes are shared between calls, because codegen's m_strings dedupes them. In the compiler they are not. Each Embed or EmbedFolder call site reads the file again in driver.cpp's read_embedded, and pushes another full copy onto analysis.embeds. Lowering then makes yet another String for each call through make_string. The 64 MiB limit's stated reason is that "the compiler holds every embedded file at once", but nothing bounds how many copies it holds. Measured on Windows: one Embed of a 64 MiB file builds in 2.4 s with a 476 MB peak. Sixteen Embeds of that same file, in the program below, take 26.9 s with a 2461 MB peak, while the executable stays 70 MB because the bytes are shared there.

```
// big.bin beside it: 64 MiB (head -c 67108864 /dev/urandom > big.bin)
Import std.core
Local n = 0
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
n += Embed("big.bin").Count
Print(n)
```

Expected: Prints 1073741824, with compiler memory and time close to a single Embed of the file (about 476 MB and 2.4 s), since every call shares one file's bytes.

Actual: Prints 1073741824, but the build takes 26.9 s and the compiler peaks at 2461 MB, so memory grows linearly with the number of call sites.

Suggested fix: Key embedded bytes by resolved path: one read, held once and shared, for example through a shared_ptr<const std::string> in embedded_file_t. Make analysis.embeds hold one entry per distinct file or folder, so that calls naming the same path share an index. Add a test that several Embeds of one file read it once.

Skeptic: In rv-embed/codegen/c2 I made a 64 MiB big.bin, a program with one `n += Embed("big.bin").Count` and one with eight. I ran `fb build` on each and polled the process's PeakWorkingSet64. One call: 2.47s, 540 MB peak. Eight calls: 13.7s, 1436 MB peak, about 128 MB more per call. Both executables are about 70.6 MB (70630400 and 70631424 bytes), so the object file shares the bytes but the compiler doesn't. Each call goes through check_embed, which calls driver.cpp's read_embedded (line 573) and reads the file again with read_file, then pushes another copy onto analysis.embeds. read_embedded dedupes only the name in compilation_t::embedded, not the bytes. driver.cpp:43 justifies the 64 MiB cap because "the compiler holds every embedded file at once", but nothing limits how many copies of one file it holds.

## paths-1 (medium): EmbedFolder's 'holds the program's own code' check compares spellings, so other letter case or a junction inside the package gets the code in

Status: fixed in dd496695 (on disk, embed_folder() compares real_path() of each loaded file with the folder's, by is_under(), so letter case and links count as the disk has them), with driver_tests' "EmbedFolder on disk leaves hidden files out, and refuses code, links, and packages in its folder, wherever they really are": EmbedFolder("code") for a link to sub/, and on a disk that ignores letter cases, EmbedFolder("SUB"), each "holds the program's own code, app/sub/mod.fb"

Where: compiler/src/driver.cpp:388

embed_folder refuses a folder holding code with `loaded.path.starts_with(within(*path, ""))`. That compares the path as the program wrote it with the loaded files' paths. It never compares where they are on disk. On Windows (and on macOS's default disk), EmbedFolder("SUB") names the folder sub/ that holds an imported module, but "sub/mod.fb" doesn't start with "SUB/", so the module's source is embedded. The same happens through a junction or symlink inside the package that leads to a code folder: linked_away allows it because the target is still in the package, and the spelled prefix "data/" never matches "sub/mod.fb". Setup for the second case: sub/mod.fb holds `Public Function F(): Int32 => 1`, and data is a junction to sub (New-Item -ItemType Junction -Path data -Target sub). Both cases were run on Windows.

```
// main.fb, beside sub/mod.fb (Public Function F(): Int32 => 1) and data/a.txt
Import std.core
Import "./sub/mod"
Local m = EmbedFolder("SUB")
For p, b In m
    Print(p)
End For
```

Expected: error at "SUB": "SUB" holds the program's own code, sub/mod.fb; EmbedFolder puts a folder of data in the program (the same error for EmbedFolder("data") when data is a junction to sub)

Actual: Builds and prints `mod.fb`. With the junction, EmbedFolder("data") builds and prints `mod.fb 33`.

Suggested fix: Compare real locations: take real_path (or key_of) of each loaded file and of the folder, then test is_under. Do not compare the spelled prefixes. Add tests for another letter case and for a link inside the package.

Skeptic: Ran under rv-embed/paths-v/p1 on Windows, debug build. sub/mod.fb holds `Public Function F(): Int32 => 1`. main.fb imports "./sub/mod" and calls EmbedFolder("SUB"): it built and printed `mod.fb` and then 1. The same program with EmbedFolder("sub") gives the expected error at 3:23, '"sub" holds the program's own code, sub/mod.fb'. With a junction jdata -> sub inside the folder, EmbedFolder("jdata") built and printed `mod.fb`. The code at driver.cpp:389-390 is `loaded.path.starts_with(within(*path, ""))`, which compares the spelled paths only and never the real ones. That breaks spec 13: it's an error 'when EmbedFolder's folder holds the program's own code'.

## paths-2 (medium): EmbedFolder embeds the files of a nested package inside its folder, which Embed refuses

Status: fixed in dd496695 (EmbedFolder refuses a folder with another package's folder in it: "store" holds the package in 'app/store/inner', whose files only its own code embeds), in the same disk test

Where: compiler/src/driver.cpp:520

Spec 13 says an embed's path can't go 'into another package's folder inside it'. embedded_path checks nested_package only for the named path itself. embed_folder never checks the files it lists. So EmbedFolder("data") puts in data/inner/project.toml and data/inner/notes.txt, the files of the package in data/inner. Yet Embed("data/inner/notes.txt") is an error: "is in the package in 'data/inner', whose files only its own code embeds". Setup: app/project.toml ([project] name = "app", version = "1.0.0"), app/data/kept.txt, app/data/inner/project.toml ([project] name = "inner"), app/data/inner/notes.txt. Run with `fb run .` in app/.

```
// app/main.fb
Import std.core
For p, b In EmbedFolder("data")
    Print(p)
End For
```

Expected: Either an error at "data" saying it holds the package in 'data/inner', or only `kept.txt` (the nested package's files left out)

Actual: inner/notes.txt
inner/project.toml
kept.txt

Suggested fix: In embed_folder, check each listed file (or each subfolder while iterating) for a project.toml between it and the embedded folder. Either refuse it as embedded_path does, or skip that subtree. Add a driver test next to the existing 'into another package' one.

Skeptic: Built rv-embed/paths-v/p2/app with project.toml (app), data/kept.txt, data/inner/project.toml (inner) and data/inner/notes.txt. `fb run .` printed inner/notes.txt, inner/project.toml, kept.txt. EmbedText("data/inner/notes.txt") in the same package gives the error '"data/inner/notes.txt" is in the package in 'data/inner', whose files only its own code embeds'. embed_folder (driver.cpp:398-465) checks each listed file with linked_away only, and calls nested_package only through embedded_path for the folder itself. Spec 13 says the path can't go 'into another package's folder inside it', so the two embeds disagree.

## paths-3 (medium): On Windows, EmbedFolder follows junctions inside the folder: a cycle repeats files silently or hangs the compiler

Status: fixed in dd496695 (embed_folder()'s walk checks each entry with is_link() and doesn't go into one; a link in the folder is an error on every system, the one leading out with its message as before, and one inside: "again" in "loops" is a link, which EmbedFolder doesn't follow, so the program is the same wherever it's built), with the disk test's loops/again, a link to loops itself, and the earlier disk test's file link leading out. The same test also covers codegen-3's refuted gap: a folder on disk gives a.txt and deep/b.txt, by '/', in order, without .hidden or .drafts/

Where: compiler/src/driver.cpp:409

embed_folder walks the folder with std::filesystem::recursive_directory_iterator. It never checks whether an entry is a link. MinGW's libstdc++ treats a junction as a plain directory and recurses into it. With one junction back to the folder (data/loop -> data), the build succeeds, and the Map holds a.txt, loop/a.txt, loop/loop/a.txt and so on, 21 copies, until the path gets too long. No error is reported. With two such junctions (data/j1 and data/j2 -> data), the walk is exponential, and fb run sat at 100% CPU for more than 90 s until it was killed. Each per-file linked_away check passes, because every copy really is inside the package. Also, libstdc++ on Linux and macOS does not follow directory symlinks by default, so the same layout gives different Maps on different platforms. That last point is inferred from the iterator's documented default; it was not run.

```
// main.fb, beside data/a.txt ("a"), with junctions data/j1 -> data and data/j2 -> data
// (New-Item -ItemType Junction -Path data/j1 -Target data; the same for j2)
Import std.core
Print(EmbedFolder("data").Count)
```

Expected: 1 (links inside the folder skipped), or an error at "data" naming the link, the same on every OS

Actual: Doesn't finish (100% CPU for more than 90 s). With only data/loop -> data, it prints 21 and embeds loop/loop/.../a.txt.

Suggested fix: While iterating, check each directory entry with is_link (the existing helper, which compares real paths). Then either call disable_recursion_pending and skip it, or refuse it with an error at the path, so Windows, Linux, and macOS agree. Add a test with a junction or symlink cycle.

Skeptic: On Windows, with data/a.txt plus the junction data/loop -> data (rv-embed/paths-v/p3a), the build succeeded, but `Print(EmbedFolder("data").Count)` printed 0 and the For loop printed nothing. Without the junction (p3c) it prints 1 and a.txt. So the a.txt that should be there is silently dropped; the finder saw 21 repeated copies instead, probably because their path was shorter. With two junctions j1 and j2 -> data (p3b), `timeout 40 fb run main.fb` was killed and exited 124 without finishing. The walk at driver.cpp:409-418 uses recursive_directory_iterator and skips only names starting with '.'. It never calls is_link or disable_recursion_pending for a link or junction. The Linux and macOS difference wasn't run.

## paths-4 (low): An embed path in other letter cases than the disk's gets no warning, unlike a quoted Import

Status: fixed in dd496695 (warn_respelled_embed() checks the path's parts against the disk, as warn_respelled_proto() does, and warns at the path once, however many times analysis checks it; embed_reader_t now takes the path's span), with the disk test's Embed("DATA/A.TXT") and EmbedFolder("SUB") warning on a disk that ignores letter cases

Where: compiler/src/driver.cpp:474

Spec 11 says an Import naming a file in other letter cases than the disk has gets a warning ("the disk spells it ..., which only a disk that ignores them finds"). Spec 13 and the step's notes say an embed's path works 'as a quoted Import's'. embedded_path does no such check. EmbedText("DATA/A.TXT") builds silently on Windows and macOS, and is then an error on Linux. Setup: data/a.txt holds "hi". In the same folder, Import "./SUB/MOD" does warn.

```
// main.fb, beside data/a.txt ("hi")
Import std.core
Print(EmbedText("DATA/A.TXT"))
```

Expected: warning at "DATA/A.TXT": the disk spells it "data/a.txt", in other letter cases, which only a disk that ignores them finds; then prints hi

Actual: No warning; prints hi

Suggested fix: Run the same disk-spelling check that quoted Imports use on the resolved embed path, for the file or folder EmbedFolder names, and report it at the argument. Add a driver test.

Skeptic: In rv-embed/paths-v/p4 on Windows, `Print(EmbedText("DATA/A.TXT"))` beside data/a.txt built with no warning and printed hi. In the same folder, `Import "./SUB/MOD"` warns at 2:8: 'the disk spells it "./sub/mod", in other letter cases, which only a disk that ignores them finds'. embedded_path (driver.cpp:474-529) runs no disk-spelling check. Spec 13 doesn't require this warning for embeds; spec 11 asks it only of Imports. But the step must work on Windows, Linux, and macOS, including letter case, and this program builds silently on Windows and macOS and fails on Linux, which is exactly what the Import warning exists to catch. That is why it's confirmed, as low.

## Refuted by the skeptics (no action)

- codegen-3: No test runs EmbedFolder on a folder on disk, so its keys, hidden-file skipping, and order there are unchecked. The gap is real: grep finds EmbedFolder in only two test files. tests/programs/embed.fb runs through program_files(), which is the library branch, where m_files.root is null. The disk-based compile_file EmbedFolder test in driver_tests (around line 2228) checks only error messages, and ends with CHECK(app->embedded.empty()). The behavior is correct, though. In rv-embed/codegen/c3 I built levels/ with a.txt, sub/b.txt, an empty sub/deeper/c.txt, .hidden, .drafts/c.txt, and sub/.h, plus an empty empty/ folder, and ran the finding's program. Debug and --release both print "3 / a.txt 3 / sub/b.txt 3 / sub/deeper/c.txt 0 / True 0". That is '/' keys on Windows, hidden files and folders skipped at every depth, sorted order, and an empty folder giving an empty Map. I saw no wrong behavior, and the code doesn't contradict the spec, stdlib.md, or the milestones row, whose done-when is that an embedded file's bytes match the file's. So this is a missing test, not a bug. A compile_file test on a written nested folder would still be worth adding.
