# The review of M6 step 14 without streams: Path (with the String-to-Path conversion), File's one-call helpers with Async twins, atomic writes, FileLock, Metadata, line endings, MappedFile, Dir with Entries, Walk, and Glob, temporary files and folders, and FileWatcher (Open with options and the File stream wait for step 13)

27 confirmed findings: 1 high, 11 medium, 15 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S14-dir-2 (high): A folder watcher silently loses most events in a burst, and its known names go stale

Status: fixed in 30afd3d2, and on macOS, where FSEvents starts late, 25043b4b

Where: runtime/src/watcher.cpp:165

When ReadDirectoryChangesW's buffer overflows, libuv calls the callback with a NULL name, which tells the program to rescan. on_event turns that into the folder's own key "" and queues a CHANGED event for the folder itself. It never rescans, so every CREATED and REMOVED in the dropped batch is lost, and `known` stays wrong from then on. Files made during the overflow never get REMOVED later, and a name removed during it comes back as CHANGED instead of CREATED. On Windows, writing 200 small files into a watched folder gave only 22 to 40 CREATED events, a different number on each run. 3000 files gave 180 CREATED with 10 folder CHANGED events, and deleting all 3000 afterwards gave only 180 REMOVED. This is normal use: an asset export, a git checkout, or an unzip into a hot-reloaded folder. It also breaks the docs' claim that the watcher tells what came and went by the names it knows are there.

```
Import std.core
Import std.io

// Makes 200 files in a watched folder, then takes what comes within three seconds.
Local temp = Try TempDir("fb-overflow-")
Local root = temp.Path
Local watcher = Try FileWatcher(root)
For i = 0 Until 200
    Try File.WriteText(root / "file {i}.txt", "x")
End For

Local created = 0
For tries = 1 To 300
    Loop
        Local event = watcher.TryNext()
        If event == Null Then Break
        If event.Kind == FileEventKind.CREATED Then created += 1
    End Loop

    Await RealSeconds(0.01)
End For

Print("created {created} of 200")
watcher.Close()
Try temp.Close()
```

Expected: created 200 of 200

Actual: created 40 of 200 (debug), created 22 of 200 (debug, second run), created 29 of 200 (release); it differs on every run

Suggested fix: In on_event, when a folder watcher gets name == nullptr (libuv's overflow signal), rescan with names_in(watcher.path) and diff the result against `known`. Queue CREATED for each new name and REMOVED for each one that's gone, then replace `known` with the new set, so no come-or-go is lost. Add a test that makes a few hundred files at once and checks that every name comes as CREATED.

Skeptic: overflow.fb gave "created 14 of 200" and "created 58 of 200" in two debug runs, and "created 72 of 200" in release. A variant (overflow2.fb) also counted events whose path is the folder itself and then deleted all 200 files. It printed "created 42 of 200, folder's own 2" and "removed 42 of 200". The two folder-own events are libuv's overflow signal (name == nullptr on Windows when ReadDirectoryChangesW's buffer overflows). watcher.cpp:165 maps that to key "" and queues CHANGED for the folder without rescanning. So the dropped CREATEDs are lost, `known` goes stale, and later REMOVEDs are lost too, since only known names get one. This breaks stdlib.md's claim that the watcher "tells what came and went by the names it knows are there", in a burst of only 200 small files.

## S14-dir-3 (medium): On Windows, a case-only rename is CHANGED and CREATED, and the old name stays known for good

Status: fixed in 56ec3149

Where: runtime/src/watcher.cpp:172

on_event decides whether a name is there with lstat, which ignores case on Windows' and macOS's usual disks. After save.txt is renamed to SAVE.txt, the old name's event finds 'save.txt' still there, so it comes as CHANGED instead of REMOVED, and 'save.txt' stays in `known` beside 'SAVE.txt'. When SAVE.txt is deleted only that name goes. A brand-new save.txt made afterwards then comes as CHANGED and never as CREATED. The docs say a rename is the old name's REMOVED and the new one's CREATED.

```
Import std.core
Import std.io

Local temp = Try TempDir("fb-case-")
Local root = temp.Path
Try File.WriteText(root / "save.txt", "one")
Local watcher = Try FileWatcher(root)

// Prints the kinds of what comes for files within half a second, each kind once.
Async Function Kinds(watcher: FileWatcher)
    Local seen: String[] = []
    For tries = 1 To 50
        Loop
            Local event = watcher.TryNext()
            If event == Null Then Break
            Local text = "{event.Kind} {event.Path.FileName()}"
            If event.Path != watcher.Path && !seen.Contains(text) Then seen.Add(text)
        End Loop

        Await RealSeconds(0.01)
    End For

    Print("  {seen}")
End Function

Print("rename save.txt to SAVE.txt")
Try File.Move(root / "save.txt", root / "SAVE.txt")
Await Kinds(watcher)
Print("delete SAVE.txt")
Try File.Delete(root / "SAVE.txt")
Await Kinds(watcher)
Print("make a new save.txt")
Try File.WriteText(root / "save.txt", "two")
Await Kinds(watcher)
watcher.Close()
Try temp.Close()
```

Expected: rename save.txt to SAVE.txt
  [REMOVED save.txt, CREATED SAVE.txt]
delete SAVE.txt
  [REMOVED SAVE.txt]
make a new save.txt
  [CREATED save.txt, CHANGED save.txt]

Actual: rename save.txt to SAVE.txt
  [CHANGED save.txt, CREATED SAVE.txt]
delete SAVE.txt
  [REMOVED SAVE.txt]
make a new save.txt
  [CHANGED save.txt]

Suggested fix: Decide 'there' by the exact name. For a folder, check that a listing, or FindFirstFileW's real name on Windows, holds the name byte for byte. Alternatively, when a name comes as CREATED, take out of `known` any other name equal to it ignoring case whose exact spelling no longer lists, and give that one REMOVED.

Skeptic: case.fb printed exactly the reviewer's actual output in both debug and release: after the rename, "[CHANGED save.txt, CREATED SAVE.txt]"; after deleting SAVE.txt, "[REMOVED SAVE.txt]"; after making a new save.txt, "[CHANGED save.txt]" with no CREATED. The cause is watcher.cpp:172, which decides presence with lstat. Windows' file system ignores case, so lstat finds the old spelling, which stays in `known` for good. This contradicts stdlib.md and watcher.fb's comment that a rename is the old name's REMOVED and the new name's CREATED, and the stale entry hides later creations.

## S14-dir-4 (medium): A watcher on a path ending with a separator gives event paths with '//' that don't equal folder / name

Status: fixed in 0191612c

Where: runtime/src/watcher.cpp:168

on_event always appends '/' and the name to watcher.path. When the path ends with a separator, as "saves/", a Windows "C:\saves\" or any root like "C:/" or "/" does, the event path becomes "saves//a.txt". Path.Join gives "saves/a.txt" for the same parts, and Paths compare by text, so `event.Path == folder / name` is False and lookups keyed by Path miss.

```
Import std.core
Import std.io

// A folder watched by a path that ends with '/', as Dir.Temp() / "x/" or a root like "C:/" does.
Local temp = Try TempDir("fb-slash-")
Local folder = Path(temp.Path.Text + "/")
Local watcher = Try FileWatcher(folder)
Try File.WriteText(folder / "a.txt", "one")
Local event = watcher.Next()
If event != Null
    // The event's path should be folder / "a.txt", not the folder's text, '/', and the name.
    Print("{event.Path == folder / "a.txt"} {event.Path.Text == folder.Text + "/a.txt"}")
End If

watcher.Close()
Try temp.Close()
```

Expected: True False

Actual: False True

Suggested fix: In on_event, add the '/' only when watcher.path doesn't already end with a separator ('/', and '\' on Windows), as Path.Join does. Or build the event's Path in watcher.fb as `Path / name` from the name alone. Add a test case with a trailing separator.

Skeptic: slash.fb printed "False True", and the event path was "C:/Users/Hunter/AppData/Local/Temp/fb-slash-DRS8lL//a.txt" against folder / "a.txt" = ".../fb-slash-DRS8lL/a.txt". With a trailing backslash (slash2.fb) the event text was ".../fb-slash-MUB0M0\/\a.txt", which also isn't equal to folder / "a.txt". Only after Normalize() do the two compare equal. watcher.cpp:167-170 always appends '/' plus the name, where Path.Join adds one only when the path doesn't already end with a separator. Paths compare by text, so a watcher made from a configured "assets/" never matches folder / name, and lookups keyed by Path quietly miss.

## S14-dir-6 (medium): Glob lists every folder down to the pattern's depth, so an unrelated unreadable folder makes it throw

Status: fixed in 5e8ac7f4

Where: std/io/dir.fb:154

Glob walks the whole tree to DepthOf(names) levels, or all of it when the pattern has '**', then filters. It never uses the pattern's literal names to choose which folders to enter. So Dir.Glob("C:/", "Users/*") lists every top-level folder of C:. One of those, C:/PerfLogs, can't be listed by a user who isn't an administrator, and the whole Glob throws PERMISSION_DENIED, although C:/Users itself lists fine. It also does far more I/O than needed: "src/**/*.fb" walks every folder beside src too, .git and the like.

```
Import std.core
Import std.io

// The pattern names only C:/Users, which can be listed, but C:/PerfLogs, which can't by a user
// who isn't an administrator, is listed too.
Print("{(Try Dir.Entries("C:/Users")).Count > 0}")
Try
    Print("{(Try Dir.Glob("C:/", "Users/*")).Count > 0}")
Catch e As IOError
    Print("{e.Kind} {e.Message}")
End Try
```

Expected: True
True

Actual: True
PERMISSION_DENIED can't list C:/: operation not permitted

Suggested fix: Walk by the pattern: list a folder only when the pattern's names so far can match its path. Enter a child only when it matches the next name, and list everything below only from a '**' on. That way folders the pattern rules out are never listed, and their errors can't surface. Add a test where a folder beside the matched one can't be listed (POSIX mode 000), or at least one that checks that unmatched folders aren't entered.

Skeptic: glob_perm.fb printed "True" then "PERMISSION_DENIED can't list C:/: operation not permitted". C:/PerfLogs can't be listed (`ls /c/PerfLogs` gives Permission denied), and Dir.Entries("C:/Users") works. dir.fb:154 asks the runtime for a full listing to DepthOf(names) levels, or the whole tree for "**", and only then filters with Matching. walk() in files.cpp:563-591 stops at the first folder it can't list. So a folder the pattern rules out (Users/* never names PerfLogs) makes the whole Glob throw. It also means "src/**/*.fb" walks every folder beside src.

## S14-dir-7 (medium): Glob's '*' matching backtracks exponentially

Status: fixed in 5e8ac7f4

Where: std/io/dir.fb:291

NameMatches handles each '*' by recursing over every split of the rest of the name, so a pattern with k stars costs about C(n, k) on a name of length n that fails late. For one entry named 60 'a's and the pattern "*a*a*a*a*a*a*a*b", Glob took 12.9 s in a release build. A pattern taken from a user or a config file can stall a program. Linear-time wildcard matching, which backtracks only to the last '*', gives the same results.

```
Import std.core
Import std.io

Local temp = Try TempDir("fb-glob-")
Local root = temp.Path
Local name = ""
For i = 1 To 60
    name += "a"
End For

Try File.WriteText(root / name, "")
Print("{(Try Dir.Glob(root, "*a*a*a*a*a*a*a*b")).Count}")
Try temp.Close()
```

Expected: 0, printed at once (one entry and one 60-character name to match)

Actual: 0, printed after 12.9 s (`time ./slow.exe` on a --release build: real 0m12.856s)

Suggested fix: Replace the recursion with the standard iterative matcher. On '*', remember the pattern position after it and the name position. On a mismatch, go back to the last '*' and move its name position on by one; fail only when there's no '*' to go back to. Treat '?' and sets as single-character matches as now. Add a test with such a pattern.

Skeptic: slow.fb in release printed 0 after 11.3 s wall time. The same program with the pattern "*b" (fast.fb, release) took 0.8 s, compile included, so about 10.5 s went to matching one 60-character name. The cause is NameMatches in dir.fb:289-295, which recurses over every split for each '*' with no memoization, giving roughly C(n, k) work. A pattern taken from a user or a config file can stall the program.

## S14-dir-8 (medium): A TempDir or TempFile whose Close fails is marked closed, so it's never removed

Status: fixed in ea8ab692

Where: std/io/temp.fb:29

Close sets Closed = True before calling Dir.RemoveAll (File.Delete for a TempFile). When the removal fails, as with BUSY on Windows while the folder is the current one or a file in it is in use, the object counts as closed. A second Close after the cause is gone does nothing, and so does the last reference's Destroy, so the folder is left behind for good. The docs say Close, or its last reference, removes it with what's in it, and the step's done-when is that nothing's left behind.

```
Import std.core
Import std.io

Local before = Try Dir.Current()
Local temp = Try TempDir("fb-busy-")
Local root = temp.Path
Try Dir.SetCurrent(root)
Try
    temp.Close()
    Print("closed")
Catch e As IOError
    Print("{e.Kind}")
End Try

Try Dir.SetCurrent(before)
Try temp.Close()
Print("left {Dir.Exists(root)}")
Try Dir.RemoveAll(root)
Print("left {Dir.Exists(root)}")
```

Expected: BUSY
left False
left False

Actual: BUSY
left True
left False

Suggested fix: Set Closed only after the removal succeeds (`Try Dir.RemoveAll(Path)` then `Closed = True`, and the same around File.Delete in TempFile). A failed Close can then be retried, and Destroy tries again. Add a test that fails a Close, removes the cause, and closes again.

Skeptic: busy.fb printed "BUSY", "left True", "left False": after the failed Close, a second Close did nothing and the folder stayed until an explicit Dir.RemoveAll. temp.fb:28-30 (TempDir) and 51-53 (TempFile) set Closed = True before the removal, so a failed removal can't be retried by Close or by Destroy. That contradicts stdlib.md's "which Close, or its last reference, removes with what's in it" and the done-when's "nothing's left behind". A FileLock held on a file in the folder (busyfile.fb) didn't make the removal fail, so the BUSY case needs something like the current folder.

## S14-file-1 (medium): ReadBytes/ReadText/ReadLines of a file bigger than memory crash with std::bad_alloc instead of throwing an IOError

Status: fixed in 34ed30d1

Where: runtime/src/files.cpp:224

read_file sizes its buffer from fstat (`bytes.resize(stat.st_size + 1)`) before it reads, with nothing catching std::bad_alloc. A file bigger than memory makes that throw through the extern "C" entry point, so the program terminates. file.fb's comment on ReadBytes says a file over 2^31 - 1 bytes "throws an IOError of OTHER", but BytesOf only checks the count after the whole file is in memory. So a 3 GiB file is read in full before it's refused, and a 200 GiB one kills the program and leaves its temporary folder behind. ReadText and ReadLines use the same read_file, so they crash the same way.

```
// A file far bigger than memory, sparse so it takes no disk, read whole.
Import std.core
Import std.io

Extern "C"
    Function system(command: CString): Int32
End Extern

Local temp = Try TempDir("fb-big-")
Local big = temp.Path / "big.bin"
Try File.WriteBytes(big, [])
// Windows: mark it sparse and make it 200 GiB long (Linux: truncate -s 200G).
system("fsutil sparse setflag \"{big.Text}\" > NUL && pwsh -NoProfile -Command \"$s = [IO.File]::Open('{big.Text}', 'Open'); $s.SetLength(200GB); $s.Close()\"")
Print("{Try File.Size(big)}")
Try
    Print("{(File.ReadBytes(big)).Count}")
Catch e As IOError
    Print("{e.Kind}")
End Try
Try temp.Close()
Print("left {Dir.Exists(temp.Path)}")
```

Expected: 214748364800
OTHER
left False

Actual: terminate called after throwing an instance of 'std::bad_alloc'
error: f_big.exe exited with code 0xC0000409
(nothing printed is flushed, and the fb-big-XXXXXX folder stays in %TEMP%)

Suggested fix: Give read_file a limit. ReadBytes passes 2^31 - 1, so a file whose fstat size is over it returns an error before anything is allocated, and the buffer can't grow past it while reading. Also catch std::bad_alloc around the resizes and return UV_ENOMEM, so ReadText/ReadLines throw an IOError instead of terminating. Add a test with a file over the limit (a sparse one, or a limit the test can lower).

Skeptic: I ran file-refute/f1.fb, which makes a 200 GiB sparse file with fsutil and pwsh SetLength (this machine has 50 GB of RAM), in debug. It printed 'terminate called after throwing an instance of std::bad_alloc' and 'f1.exe exited with code 0xC0000409'. Nothing it printed got out, and the fb-big-fhfC9I folder with its sparse big.bin stayed in %TEMP% (I removed it by hand). In files.cpp, read_file line 224 does bytes.resize(st_size + 1) with nothing catching bad_alloc, so the exception goes out through the extern "C" fb_io_file_read. file.fb's ReadBytes comment promises an IOError of OTHER for a file over 2^31 - 1 bytes. BytesOf only checks the count after the whole file has been read, and ReadText/ReadLines use the same read_file.

## S14-file-2 (medium): Waiting AcquireAsync/AcquireSharedAsync calls hold libuv's thread-pool threads, so every other Async file call stalls

Status: fixed in 06855968

Where: runtime/src/files.cpp:790

A waiting AcquireAsync runs acquire_lock with wait=1 on libuv's thread pool, through run_async and uv_queue_work. There it blocks in LockFileEx/flock for as long as another holder keeps the lock. The pool has 4 threads (UV_THREADPOOL_SIZE isn't set), so four waiting Async locks use all of them. Every other Async twin (ReadTextAsync, WriteTextAsync, MetadataAsync, Dir's, ...) then queues behind them and doesn't run until a lock comes free. If another process holds the lock, that can be forever, even for files no lock touches. With three waiters the same read finishes at once. Nothing in the docs says a lock wait blocks unrelated I/O.

```
Import std.core
Import std.io

Extern "C"
    Function Sleep(ms: UInt32)
End Extern

Function Slept(): Int
    Sleep(2000)
    Return 0
End Function

Async Function ReadAndSay(path: Path)
    Local text = (Try? Await File.ReadTextAsync(path)) ?? "none"
    Print("read [{text}]")
End Function

Local temp = Try TempDir("fb-rev-")
Local root = temp.Path
Try File.WriteText(root / "other.txt", "other")
Local held = Try FileLock.Acquire(root / "lock")
// Shared locks wait for the one held, which this program lets go two seconds on.
Local waits = [FileLock.AcquireSharedAsync(root / "lock")]
For i = 1 Until 4
    waits.Add(FileLock.AcquireSharedAsync(root / "lock"))
End For

// A read of another file, which no lock touches.
Local read = ReadAndSay(root / "other.txt")
Await Spawn(Function() => Slept())
Print("two seconds on, letting go")
held.Close()
Await read
For w In waits
    Local lock = Try Await w
    lock.Close()
End For
Try temp.Close()
Print("left {Dir.Exists(root)}")
```

Expected: read [other]
two seconds on, letting go
left False

Actual: two seconds on, letting go
read [other]
left False
(with `Until 3`, i.e. three waiters, it prints the expected order)

Suggested fix: Don't block a pool thread for an unbounded lock wait. For example, run each waiting Async acquire on a thread of its own, or try the lock without blocking (LOCK_NB / LOCKFILE_FAIL_IMMEDIATELY) and retry on a uv_timer on the I/O thread. Add a test like this one: N waiting AcquireAsync calls plus an unrelated ReadTextAsync that finishes before the lock is let go.

Skeptic: f2.fb, with 4 waiting AcquireSharedAsync calls, printed 'two seconds on, letting go' before 'read [other]'. f2b.fb, with 3 waiters, printed the read first. I also wrote f2c.fb: it holds a lock, starts 4 AcquireSharedAsync waiters, then does 'Await File.WriteTextAsync(save.txt)' before held.Close(). It hung until timeout 15 killed it (exit 124), and save.txt was never written. That's a deadlock inside one program, from its own locks alone. The cause: acquire_lock with wait=1 blocks in LockFileEx inside run_async/uv_queue_work, on libuv's default pool of 4 threads, so every other Async file call queues behind the waits. stdlib.md only says a lock waiting on one this program holds waits forever, not that unrelated I/O stops.

## S14-file-3 (medium): File.Move of a folder onto an existing file deletes the file on Windows; POSIX refuses it

Status: fixed in 215dd216

Where: runtime/src/files.cpp:384

move_request calls uv_fs_rename as is. On Windows that's MoveFileExW(MOVEFILE_REPLACE_EXISTING), which replaces a file with a folder: the file's contents are lost and the folder takes its name. POSIX's rename(dir, file) fails with ENOTDIR, so the same program throws NOT_A_DIRECTORY there and keeps the file. The other cases differ too: on Windows a folder onto an empty folder throws PERMISSION_DENIED, where POSIX replaces it. The comments say Move replaces "a file there", and std is meant to behave the same on every OS (std-cross-platform), but here Windows destroys data where POSIX refuses.

```
Import std.core
Import std.io

Local temp = Try TempDir("fb-rev-")
Local root = temp.Path
Try File.WriteText(root / "save.dat", "precious")
Try Dir.Create(root / "folder")
Try File.WriteText(root / "folder" / "inside.txt", "inside")
Try
    File.Move(root / "folder", root / "save.dat")
    Print("moved")
Catch e As IOError
    Print("{e.Kind}")
End Try
Print("{File.Exists(root / "save.dat")} {Dir.Exists(root / "save.dat")} {Dir.Exists(root / "folder")}")
Try temp.Close()
Print("left {Dir.Exists(root)}")
```

Expected: NOT_A_DIRECTORY
True False True
left False
(as POSIX's rename gives: the file is kept)

Actual: moved
False True False
left False
(Windows: save.dat's contents are gone, replaced by the folder)

Suggested fix: In move_request, before renaming, lstat `to`. If `from` is a folder and `to` is something else, return UV_ENOTDIR. If `from` isn't a folder and `to` is one, return UV_EISDIR. Decide whether a folder may replace an empty folder, and make both OSes do the same. Test each case in io_files.fb.

Skeptic: f3.fb in debug and in --release printed 'moved' then 'False True False': save.dat's contents were gone and the folder had taken its name. In WSL, python3 os.rename(folder, save.dat) raises NotADirectoryError (errno 20) and keeps the file. f3b.fb (a folder onto an empty folder) throws PERMISSION_DENIED on Windows, where POSIX rename succeeds (also checked in WSL). move_request (files.cpp:384) calls uv_fs_rename as it is, so Move does opposite things on the two OSes, and on Windows it destroys a file where POSIX refuses.

## S14-path-1 (medium): A share with no separator after it is a different anchor from the same share with one, so RelativeTo gives Null and Normalize keeps two texts for one place

Status: fixed in 93e343ec

Where: std/io/path.fb:299

AnchorOf gives "//server/share" an anchor of just its prefix (14 bytes) and "//server/share/x" or "//server/share/" the prefix plus the separator (15 bytes). Both name the share's root: a UNC share is always rooted. Python's PureWindowsPath, which the design follows, gives every UNC drive a root. Because of the split, Normalize leaves "//server/share" and "//server/share/" unequal. Parent of "//server/share/x" is "//server/share/", which isn't equal to "//server/share". RelativeTo compares the anchors' lengths (line 209), so it gives Null for a path inside a share measured from the share. The Windows tests only use "//server/share" on its own, or with names after it, and never compare the two forms.

```
Import std.core
Import std.io

Function W(text: String): Path => Path(text, PathStyle.WINDOWS)

Function Rel(to: Path, from: Path): String
    Local r = to.RelativeTo(from)
    Return r == Null ? "none" : r.Text
End Function

Print(Rel(W("//server/share/x"), W("//server/share")))
Print(Rel(W("//server/share"), W("//server/share/")))
Print(W("//server/share").Normalize() == W("//server/share/").Normalize())
Print(W("//server/share/x").Parent() == W("//server/share"))
```

Expected: x
.
True
True

Actual: none
none
False
False
(the same in debug and release)

Suggested fix: Treat a share as rooted whether or not a separator follows it. One way: AnchorOf counts a share's anchor as rooted, and AnchorText/Normalize/Parent always write "//server/share/" (or never write the trailing '/'), so both forms normalize to one text. Then RelativeTo's anchor comparison matches them. Add both forms to io_path.fb's Normalize, Parent, and RelativeTo cases.

Skeptic: Ran the finding's program (pathref/p1.fb), debug and release, and got the same output both times: none / none / False / False. Also printed: W("//server/share/x").Parent() = "//server/share/", W("//server/share").Normalize() = "//server/share", W("//server/share/").Normalize() = "//server/share/". IsAbsolute is True for both forms. The cause is AnchorOf (path.fb:299): the anchor is `at` without a trailing separator and at+1 with one. RelativeTo (line 209) needs equal anchor lengths, so a path inside a share measured from the bare share gives Null. RelativeTo's own doc only allows Null for absolute vs relative, different drives, or a base that goes above. Python's PureWindowsPath, which the lane follows, gives every UNC drive a root, so '//server/share' == '//server/share/' and relative_to gives 'x'. The tests in io_path.fb never compare the two forms.

## S14-path-2 (medium): WithExtension panics on a path ending in "." even though FileName gives a name for it

Status: fixed in 47247d05

Where: std/io/path.fb:150

FileName, Stem, and Extension go through NamesOf, which skips "." names, so "a/b/." has the file name "b" (as Python's PurePath does). WithExtension instead takes the raw last component between separators, finds ".", and panics "needs a path with a file name". So a path that FileName says has a name can't be given an extension, and a program that checks FileName() != "" first still panics. Python gives PurePosixPath('a/b/.').with_suffix('.txt') == 'a/b.txt'. The same happens for "a/./" and "C:\\x\\.".

```
Import std.core
Import std.io

Local p = Path("a/b/.", PathStyle.POSIX)
Print(p.FileName())
Print(p.WithExtension(".txt"))
```

Expected: b
a/b.txt

Actual: b
panic: WithExtension needs a path with a file name, not 'a/b/.' at f2.fb:6:7

Suggested fix: Find the name WithExtension changes the way FileName does: skip trailing "." components and separators before taking the last name. Or rebuild from NamesOf the way Parent does. Panic only when FileName() would be "" or "..". Add "a/b/." to io_path.fb's WithExtension line.

Skeptic: pathref/p2.fb, debug and release: printed 'b', then 'panic: WithExtension needs a path with a file name, not 'a/b/.' at p2.fb:6:7'. In pathref/p9.fb, FileName of 'a/./' is 'a' and of 'C:\\x\\.' is 'x', yet WithExtension on 'C:\\x\\.' panics the same way. WithExtension (path.fb:140-152) takes the raw last component, '.', and IsDots rejects it. FileName goes through NamesOf, which skips '.'. WithExtension's doc says it panics only when the path has no file name, 'as a root or ".." doesn't'. FileName says these paths have one, so a program that checks FileName() first still crashes.

## S14-path-3 (medium): On Windows a zero Path (a Struct field with no default) has POSIX rules, not the target's

Status: fixed in a69db26e

Where: std/io/path.fb:18

PathStyle's first member is POSIX, so a Path's zero is ("", POSIX) on every OS. Spec 06 says a Struct's zero is everyone's: in Fields, Globals, and arrays. So on Windows, a Struct holding a Path field that's filled in later starts with a POSIX path. It isn't equal to Path(""), which is WINDOWS there, and Join keeps the left side's style, so every path built from it reads '\\' as part of a name. FileName and Parent then give wrong answers for ordinary Windows paths. This contradicts the design's "the target's unless made with the other". It only shows on Windows, where POSIX isn't native.

```
Import std.core
Import std.io

Struct Save
    Field Spot: Path
End Struct

Local s = Save()
Print("{s.Spot.Style} {Path("").Style} {s.Spot == Path("")}")
Local q = s.Spot / "games\\save.dat"
Print("{q} {q.FileName()} {q.Parent()}")
```

Expected: On Windows:
WINDOWS WINDOWS True
games\save.dat save.dat games

Actual: POSIX WINDOWS False
games\save.dat games\save.dat .
(debug and release alike)

Suggested fix: Make the zero mean the target's style. For example, keep the style in a private field where 0 means "the target's", expose Style as a Property that resolves it with NativeStyle(), and compare and hash by the resolved style (an Operator == and Hash of Path's own). Or make PathStyle's first member a NATIVE that every method resolves. Add a zero-Path case to io_path.fb.

Skeptic: pathref/p3.fb on Windows, debug and release (I rewrote it with the Write tool, because a heredoc turned the \\\\ into a single backslash): printed 'POSIX WINDOWS False' then 'games\\save.dat games\\save.dat .'. PathStyle's first member is POSIX (path.fb:18-21), and spec 06 says an Enum's zero is its first member. A Struct's zero is everyone's 'in Fields, Globals, and arrays', and spec 06 says a type whose values come from its own code 'makes its zero mean nothing'. A zero Path instead means a POSIX path on Windows, which contradicts stdlib.md's 'the target's unless asked for'. A Path field left at its zero and then joined reads '\\' as part of a name, so FileName and Parent are wrong for ordinary Windows paths.

## S14-dir-10 (low): docs/stdlib.md and path.fb say Paths sort by their texts' bytes, but '<' compares the style first

Status: fixed in a69db26e, with S14-path-6

Where: docs/stdlib.md:458

Path's Operator < compares Style first and the text only within a style; the lane intends this. docs/stdlib.md line 458 ('sort by their texts' bytes') and path.fb line 29 say only bytes, so a POSIX "b" sorts before a WINDOWS "a", which the docs don't allow for.

```
Import std.core
Import std.io

// docs/stdlib.md: Paths "sort by their texts' bytes".
Local b = Path("b", PathStyle.POSIX)
Local a = Path("a", PathStyle.WINDOWS)
Print("{b < a} {a < b}")
```

Expected: False True (by the docs: "a" < "b" by bytes)

Actual: True False

Suggested fix: Say "sort by their styles, POSIX first, then their texts' bytes" in docs/stdlib.md line 458 and in path.fb line 29's comment.

Skeptic: sort.fb printed "True False": a POSIX "b" sorts before a WINDOWS "a". path.fb:54-57's Operator < compares Style first, which is the lane's intended rule. But stdlib.md line 458 ("sort by their texts' bytes") and path.fb's comment at lines 28-29 ("they sort by their texts' bytes") say only bytes. A doc fix only.

## S14-dir-11 (low): Glob's docs say '**' is any number of folders, but a final '**' matches files and the folder itself

Status: fixed in a9e04cd3

Where: docs/stdlib.md:489

docs/stdlib.md and dir.fb's comment both say a name of "**" matches any number of folders. When it's last, it matches any names, files included: "**" gives every file, and "sub/**" gives sub itself and the files in it. io_files.out expects exactly that, so the code is intended and the docs are wrong. The docs also don't say that on Windows a '\' in a pattern separates names too (PatternNames). dir.fb line 147, in the same comment, is a botched reflow 143 characters wide; std's other .fb files keep to 100.

```
Import std.core
Import std.io

// docs/stdlib.md and dir.fb: "a name of `**` for any number of folders".
Local temp = Try TempDir("fb-star-")
Local root = temp.Path
Try Dir.Create(root / "sub")
Try File.WriteText(root / "file.txt", "")
Try File.WriteText(root / "sub" / "inner.txt", "")
For pattern In ["**", "sub/**"]
    Local matched: String[] = []
    For entry In Try Dir.Glob(root, pattern)
        matched.Add("{entry.Relative} {entry.Kind}")
    End For

    Print("{pattern}: {matched}")
End For

Try temp.Close()
```

Expected: By the docs, folders only:
**: [sub DIRECTORY]
sub/**: [sub DIRECTORY]

Actual: **: [file.txt FILE, sub DIRECTORY, sub/inner.txt FILE]
sub/**: [sub DIRECTORY, sub/inner.txt FILE]

Suggested fix: Reword docs/stdlib.md lines 487-490 and dir.fb's comment: "**" matches any number of folders, none too, and when it's last, everything below, files too, and the folder before it. Mention that '\' separates names in a Windows-style path's pattern. Reflow dir.fb lines 143-149 to 100 columns.

Skeptic: star.fb printed "**: [file.txt FILE, sub DIRECTORY, sub/inner.txt FILE]", "sub/**: [sub DIRECTORY, sub/inner.txt FILE]", and "sub\\*: [sub/inner.txt FILE]". So a final "**" matches files and the folder before it, and on Windows '\\' separates a pattern's names (PatternNames, dir.fb:221). io_files.out:55 and :63 expect that, so the code is intended. But stdlib.md ("a name of `**` for any number of folders") and dir.fb's comment describe only folders and only '/'. dir.fb line 147 is a comment line 143 characters wide (awk length), and the only other comment line over 100 in all of std is in std/core/interfaces.fb.

## S14-dir-12 (low): The std.io docs don't say what step 14 built, and the milestones row's console is neither built nor deferred

Status: fixed in e352b602

Where: docs/stdlib.md:446

std.math, std.concurrent, and std.events each open with 'Step N built ...; the rest is planned', but std.io doesn't. Its bullets mix what's built (Path, File, FileLock, MappedFile, Dir, TempFile/TempDir, FileWatcher) with what isn't (streams, the VFS, Embed, Preferences, Console) and give no status. Milestones row 14 lists 'the console' in the step's scope, but nothing in std/io provides it: IsTerminal and TerminalSize aren't declared. No Deferred work row records that it moved, as the stdlib.md text does for File.Open. stdlib.md line 4 still says 'Steps 1 to 4 are built'.

```
Import std.core
Import std.io

// The milestones row puts the console in step 14; docs/stdlib.md lists it under std.io.
Print("{IsTerminal()} {TerminalSize()}")
```

Expected: Either the console exists, or docs/stdlib.md and milestones.md say it's deferred and where. In that case this compile error is the documented state.

Actual: console.fb:5:9: error: 'IsTerminal' isn't declared
console.fb:5:24: error: 'TerminalSize' isn't declared
error: couldn't build console.fb because of 2 errors

Suggested fix: Add a status line under '#### std.io' ("Step 14 built Path, File's one-call helpers, FileLock, MappedFile, Dir, TempFile and TempDir, and FileWatcher; File.Open waits for step 13's streams; the rest is planned"). Add a Deferred work row for the console naming the step that takes it, or build it. Update line 4's status.

Skeptic: console.fb fails to compile with "'IsTerminal' isn't declared" and "'TerminalSize' isn't declared", and grep finds no IsTerminal or TerminalSize in std/ or runtime/src. milestones.md row 14 lists "the console" in the step's scope. The Deferred work section has no row for it, and none of the change's 20 commits mentions it. stdlib.md's std.io section has no "Step 14 built ...; the rest is planned" status line, unlike std.math (line 309), std.concurrent (537), and std.events (590), so built items (Path, File, Dir...) sit beside planned ones (streams, the VFS, Embed, Preferences, Console) with nothing telling them apart. Line 4's stale "Steps 1 to 4 are built" is already there in 1d32958, so it isn't this change's.

## S14-dir-13 (low): The tests don't cover Dir.Temp, TempDir's default prefix, or a TempDir going with its last reference

Status: fixed in 0c3cf5e1

Where: tests/programs/io_files.fb:249

No io_*.fb test calls Dir.Temp(), makes a TempDir with its default prefix, or drops a TempDir with a file in it so that its Destroy removes it. Only TempFile's drop is checked, and a TempDir that's in Dir.Temp() isn't checked at all. These are what the done-when's 'nothing's left behind' rests on. They work today, as the program shows, but nothing guards them. The failing-Close and burst-of-events cases above (S14-dir-2, S14-dir-8) have no tests either.

```
Import std.core
Import std.io

Local temp = Try TempDir()
Local root = temp.Path
Try File.WriteText(root / "a.txt", "x")
Print("{root.Parent() == Try Dir.Temp()} {root.FileName().Bytes.Count} {root.FileName().Bytes[0..3] == "fb-".Bytes}")
temp = Try TempDir()
Print("{Dir.Exists(root)}")
Try temp.Close()
```

Expected: A test in tests/programs prints this. The program's output is
True 9 True
False

Actual: True 9 True
False (correct, but no test checks it)

Suggested fix: Add these lines to io_files.fb's temporary-files part, or to a new io_temp.fb, with their .out lines.

Skeptic: tempdef.fb printed "True 9 True" and "False", so the behaviour is right. But grep over tests/ finds no Dir.Temp() call and no TempDir() with the default prefix, and io_files.fb:249-264 checks a dropped TempFile but never a dropped TempDir with a file in it. stdlib.md promises TempDir's "Close, or its last reference, removes with what's in it", and `Temp`, the OS's temporary folder. m6-plan.md has a step build tests for what stdlib.md adds, so these are untested claims. S14-dir-2 and S14-dir-8's cases have no tests either.

## S14-dir-5 (low): A watcher made with a relative path drops every event once the current folder changes

Status: fixed in 0191612c

Where: runtime/src/watcher.cpp:253

The watcher keeps the path's text as given and runs lstat on watcher.path + '/' + name for each event. libuv opened the folder when the watch started, so events keep coming after Dir.SetCurrent. The lstat, though, now resolves against the new current folder, finds nothing, and the event is dropped as 'came and went'. Without the second SetCurrent, the same program sees CREATED and CHANGED. Dir.SetCurrent applies to every thread, and a watcher is long-lived.

```
Import std.core
Import std.io

// Watches "watched" from the folder it's in, then goes to another folder, as a program may.
Local before = Try Dir.Current()
Local temp = Try TempDir("fb-cwd-")
Local root = temp.Path
Try Dir.CreateAll(root / "one" / "watched")
Try Dir.CreateAll(root / "two")
Try Dir.SetCurrent(root / "one")
Local watcher = Try FileWatcher("watched")
Try Dir.SetCurrent(root / "two")
Try File.WriteText(root / "one" / "watched" / "a.txt", "x")
Local kinds: String[] = []
For tries = 1 To 50
    Loop
        Local event = watcher.TryNext()
        If event == Null Then Break
        kinds.Add("{event.Kind} {event.Path}")
    End Loop

    Await RealSeconds(0.01)
End For

Print("{kinds.Count > 0} {kinds}")
watcher.Close()
Try Dir.SetCurrent(before)
Try temp.Close()
```

Expected: True [CREATED watched/a.txt, CHANGED watched/a.txt, CHANGED watched/a.txt]
(as printed when the line `Try Dir.SetCurrent(root / "two")` is left out)

Actual: False []

Suggested fix: In fb_io_watcher_new, resolve the path to an absolute one (the current folder joined with it, or uv_fs_realpath) and use that for the exists() checks and names_in. Keep the text the program gave for the events' paths.

Skeptic: cwd.fb printed "False []". The same program without the second SetCurrent (cwd_same.fb) printed "True [CREATED watched/a.txt, CHANGED watched/a.txt, CHANGED watched/a.txt]". watcher.cpp keeps the relative text and lstats watcher.path + '/' + name (line 172). After Dir.SetCurrent, that resolves against the new folder, so every event is dropped as 'came and went' while libuv's handle is still watching the old folder. That contradicts 'watches ... until it's closed'. I rate it low because it needs a relative watcher followed by a change of current folder, and the events' relative paths would point at the wrong folder after the change anyway.

## S14-dir-9 (low): Walk's and Glob's IOError names the folder listed, not the one that failed

Status: fixed in 5e8ac7f4

Where: runtime/src/files.cpp:578

walk() returns a nested folder's error code alone, and dir.fb's Checked builds the IOError from the top path. Dir.Walk("C:/") fails on C:/PerfLogs, but its Path is "C:/" and its message is "can't list C:/", which points at a folder that lists fine. Dir.Entries on the folder itself names it correctly.

```
Import std.core
Import std.io

// C:/PerfLogs can't be listed by a user who isn't an administrator.
Try
    Dir.Entries("C:/PerfLogs")
Catch e As IOError
    Print("entries: {e.Kind} [{e.Path ?? ""}]")
End Try

Try
    Dir.Walk("C:/")
Catch e As IOError
    Print("walk: {e.Kind} [{e.Path ?? ""}] {e.Message}")
End Try
```

Expected: entries: PERMISSION_DENIED [C:/PerfLogs]
walk: PERMISSION_DENIED [C:/PerfLogs] can't list C:/PerfLogs: operation not permitted

Actual: entries: PERMISSION_DENIED [C:/PerfLogs]
walk: PERMISSION_DENIED [C:/] can't list C:/: operation not permitted

Suggested fix: Have walk() record the folder whose listing failed in the request, for example in request.other with '/' separators, and have Walk, Glob, and their Async twins build the IOError with that path when it's set.

Skeptic: walk_perm.fb printed "entries: PERMISSION_DENIED [C:/PerfLogs]" and "walk: PERMISSION_DENIED [C:/] can't list C:/: operation not permitted". In files.cpp:583-586, walk() returns only the nested folder's error code, and dir.fb's Checked builds the IOError from the top path. So Path and Message name a folder that lists fine, not the one that failed. Glob over C:/ gave the same misleading message (S14-dir-6's output).

## S14-file-4 (low): WriteTextAtomic/WriteBytesAtomic to a link replace the link with a plain file and leave the linked file unchanged

Status: fixed in 5be9faaa

Where: runtime/src/files.cpp:280

write_atomic makes its temporary beside the link and renames it over the link's own path, so the link becomes a regular file and the file it pointed to keeps its old contents. WriteText to the same path writes through the link, so the two writes now disagree about which file they change. Breaking a symlinked config or save file is a well-known atomic-write bug. write_atomic itself follows the link when it copies permissions (`stat_path(path, true, replaced)`), so it takes the target's mode but replaces the link. And because the temporary sits in the link's folder, a target on another disk would get UV_EXDEV from the rename. The tests never write through a link.

```
// An atomic write to a link, beside a plain write to it.
Import std.core
Import std.io

Extern "C"
    Function system(command: CString): Int32
End Extern

Local temp = Try TempDir("fb-link-")
Local target = temp.Path / "target.txt"
Local link = temp.Path / "link.txt"
Try File.WriteText(target, "target")
// Windows, with Developer Mode or as an administrator (POSIX: ln -s target.txt link.txt).
system("pwsh -NoProfile -Command \"New-Item -ItemType SymbolicLink -Path '{link.Text}' -Target '{target.Text}'\" > NUL")
Print("{(Try File.Metadata(link, False)).Kind}")
Try File.WriteText(link, "plain")
Print("[{Try File.ReadText(target)}] {(Try File.Metadata(link, False)).Kind}")
Try File.WriteTextAtomic(link, "atomic")
Print("[{Try File.ReadText(target)}] {(Try File.Metadata(link, False)).Kind}")
Try temp.Close()
Print("left {Dir.Exists(temp.Path)}")
```

Expected: LINK
[plain] LINK
[atomic] LINK
left False

Actual: LINK
[plain] LINK
[plain] FILE
left False

Suggested fix: In write_atomic, when lstat says the path is a link, resolve it first with uv_fs_realpath, then make the temporary beside the resolved file and rename onto that path. If links are meant to be replaced instead, say so in file.fb and docs/stdlib.md. Either way, test it.

Skeptic: f4.fb makes a symlink with pwsh New-Item, which works here. It printed 'LINK', '[plain] LINK', then '[plain] [atomic] FILE': WriteText went through the link to the target, but WriteTextAtomic replaced the link with a regular file and left the target unchanged. write_atomic renames its temporary over the path itself, while File's other calls follow links (ReadText, Exists, Metadata by default), and its doc says 'so the file holds either all of them or what it held before'. The target never gets the new bytes, though the write succeeds. No data is lost, so low. The finding's EXDEV point doesn't hold for the current code: the temporary sits beside the link, so the rename stays on one disk.

## S14-file-5 (low): Atomic writes fail for long file names that plain writes accept

Status: fixed in 5ea48589

Where: runtime/src/files.cpp:273

temporary_beside names the temporary "." + the whole file name + ".<pid>-<n>-<time>.tmp", about 20 to 25 bytes longer than the file's own path. A path within ~25 bytes of the OS's limit therefore takes WriteText but fails WriteTextAtomic. On Windows that limit is MAX_PATH 260, where the error is NOT_FOUND for a folder that's there. On Linux and macOS it's NAME_MAX 255 bytes for the name, which gives INVALID_PATH. Python's tempfile, for example, uses a short random name in the same folder.

```
// An atomic write to a path a plain write takes, whose name is long.
Import std.core
Import std.io

Local temp = Try TempDir("fb-long-")
Local name = ""
// 249 bytes of path on Windows' C:/Users/<name>/AppData/Local/Temp, under its limit of 260.
While (temp.Path / (name + "n")).Text.Bytes.Count < 250
    name = name + "n"
End While

Local path = temp.Path / name
Try File.WriteText(path, "plain")
Print("plain [{Try File.ReadText(path)}]")
Try
    File.WriteTextAtomic(path, "atomic")
    Print("atomic [{Try File.ReadText(path)}]")
Catch e As IOError
    Print("atomic {e.Kind}")
End Try
Try temp.Close()
Print("left {Dir.Exists(temp.Path)}")
```

Expected: plain [plain]
atomic [atomic]
left False

Actual: plain [plain]
atomic NOT_FOUND
left False

Suggested fix: Name the temporary with a short fixed-length form that doesn't include the file's name, such as ".fb-" plus 12 random letters and digits plus ".tmp", which still gets O_EXCL and retries. On Windows a path near MAX_PATH can still fail, but no longer because of the name std adds. Add a test with a name of ~240 bytes in a short folder.

Skeptic: f5.fb with a 249-byte path printed 'plain [plain]' then 'atomic NOT_FOUND', in debug and in --release. f5b.fb with a 219-byte path printed 'atomic [atomic]', so the failure comes from the length. temporary_beside (files.cpp:273) adds '.' + name + '.<pid>-<n>-<time>.tmp', about 20 bytes, which takes the path past MAX_PATH. On POSIX the same scheme makes a 240-byte name go over NAME_MAX. Names that long are rare, so low.

## S14-file-6 (low): FileLock can't lock a file that's there but read-only

Status: fixed in f0b6c0ef

Where: runtime/src/files.cpp:703

acquire_lock always opens the file with UV_FS_O_RDWR | UV_FS_O_CREAT, so an existing read-only file throws PERMISSION_DENIED from Acquire, AcquireShared, TryAcquire, and the Async twins. A lock doesn't need write access: flock works on a read-only descriptor, and LockFileEx needs only GENERIC_READ or GENERIC_WRITE. docs/stdlib.md only says the file is "made when it isn't there". Locking a read-only data file to coordinate readers, with AcquireShared, is a natural use.

```
// Locking a file that's there but read-only: a lock needs no writing.
Import std.core
Import std.io

Local temp = Try TempDir("fb-lockro-")
Local path = temp.Path / "data.txt"
Try File.WriteText(path, "data")
Try File.SetReadOnly(path, True)
Try
    Local lock = FileLock.AcquireShared(path)
    Print("locked {lock.IsHeld()}")
    lock.Close()
Catch e As IOError
    Print("{e.Kind}: {e.Message}")
End Try
Try File.SetReadOnly(path, False)
Try temp.Close()
Print("left {Dir.Exists(temp.Path)}")
```

Expected: locked True
left False

Actual: PERMISSION_DENIED: can't lock C:/Users/Hunter/AppData/Local/Temp/fb-lockro-dCxYt8/data.txt: operation not permitted
left False

Suggested fix: When the read-write open fails with UV_EACCES or UV_EPERM, open the existing file again with UV_FS_O_RDONLY and lock that. Keep O_CREAT only on the first try. Add the read-only case to io_files.fb's locks.

Skeptic: f6.fb printed 'PERMISSION_DENIED: can't lock .../data.txt: operation not permitted' for AcquireShared on a file it had made read-only with SetReadOnly. acquire_lock (files.cpp:703) always opens with UV_FS_O_RDWR | UV_FS_O_CREAT. The lock is on a byte past the end, and neither LockFileEx (GENERIC_READ is enough) nor flock needs write access, so a shared lock on a file the program can only read is refused for no reason. stdlib.md doesn't require the file to be writable. Low.

## S14-file-7 (low): On Windows, Copy/Move onto a folder throw PERMISSION_DENIED and FileLock on a folder throws INVALID_PATH, not IS_A_DIRECTORY

Status: fixed in 215dd216

Where: runtime/src/files.cpp:362

WriteText and WriteTextAtomic onto a folder throw IS_A_DIRECTORY, the first by libuv's translation and the second by write_atomic's own check. Other calls pass Windows' errors through unchanged: CopyFileW and MoveFileExW onto a folder give ERROR_ACCESS_DENIED, so PERMISSION_DENIED, and opening a folder read-write for a lock gives EINVAL, so INVALID_PATH. On POSIX all three give EISDIR (copyfile's and acquire_lock's opens of the folder, and rename(file, dir)), so IS_A_DIRECTORY. The done-when says errors come as IOError Kinds, and here the Kind depends on the call and the OS. The tests only cover copying *from* a folder.

```
// What each call throws when its path, or its destination, is a folder.
Import std.core
Import std.io

Local temp = Try TempDir("fb-kinds-")
Local root = temp.Path
Try File.WriteText(root / "a.txt", "a")
Try Dir.Create(root / "folder")

Function Failure(what: String, a: Path, b: Path): String
    Try
        Select what
            Case "write"
                File.WriteText(b, "x")
            Case "atomic"
                File.WriteTextAtomic(b, "x")
            Case "copy"
                File.Copy(a, b)
            Case "move"
                File.Move(a, b)
            Case "lock"
                FileLock.Acquire(b)
        End Select
    Catch e As IOError
        Return "{what} {e.Kind}"
    End Try
    Return "{what} fine"
End Function

For what In ["write", "atomic", "copy", "move", "lock"]
    Print(Failure(what, root / "a.txt", root / "folder"))
End For
Try temp.Close()
Print("left {Dir.Exists(root)}")
```

Expected: write IS_A_DIRECTORY
atomic IS_A_DIRECTORY
copy IS_A_DIRECTORY
move IS_A_DIRECTORY
lock IS_A_DIRECTORY
left False

Actual: write IS_A_DIRECTORY
atomic IS_A_DIRECTORY
copy PERMISSION_DENIED
move PERMISSION_DENIED
lock INVALID_PATH
left False
(the same in --release)

Suggested fix: Check the destination as write_atomic does. In copy_request, return UV_EISDIR when `to` stats as a folder. In move_request, do the same when `from` isn't a folder and `to` is one (see S14-file-3). In acquire_lock, stat the path, or the opened file, and return UV_EISDIR for a folder. Add these cases to io_files.fb's errors.

Skeptic: f7.fb printed 'write IS_A_DIRECTORY', 'atomic IS_A_DIRECTORY', 'copy PERMISSION_DENIED', 'move PERMISSION_DENIED', 'lock INVALID_PATH', the same in --release. In WSL, rename(file, dir) gives IsADirectoryError (21), so POSIX reports IS_A_DIRECTORY there. error.fb's IOErrorKind documents IS_A_DIRECTORY as a folder where a file should be and INVALID_PATH as a path no file can have, so PERMISSION_DENIED and INVALID_PATH misreport these cases on Windows. write_atomic already turns this case into EISDIR ('A rename over a folder fails differently on each OS'), but copy_request, move_request, and acquire_lock don't.

## S14-file-8 (low): MappedFile's comment says a mapped file can't be deleted on Windows; it can, and writing it fails with OTHER "unknown error"

Status: fixed in e1ec26d7

Where: std/io/mapped.fb:25

The class comment says "On Windows, a mapped file can't be deleted until it's closed." With libuv 1.52, which deletes with POSIX semantics, File.Delete of a mapped file returns True: the name goes at once and the map stays readable. What does fail on Windows is File.WriteText, which truncates the file. That fails with ERROR_USER_MAPPED_FILE, which libuv doesn't translate, so the IOError's Kind is OTHER and its message is "unknown error" instead of BUSY, the Kind for "a file that's in use or locked".

```
// MappedFile's comment says that on Windows a mapped file can't be deleted until it's closed.
Import std.core
Import std.io

Local temp = Try TempDir("fb-mapdoc-")
Local path = temp.Path / "m.bin"
Try File.WriteBytes(path, [1, 2, 3, 4])
Local map = Try MappedFile(path, True)
Try
    File.WriteText(path, "x")
    Print("write fine")
Catch e As IOError
    Print("write {e.Kind}: {e.Message}")
End Try
Try
    Print("deleted {File.Delete(path)}, there {File.Exists(path)}, still mapped {map.IsOpen()} {map[3]}")
Catch e As IOError
    Print("delete {e.Kind}")
End Try
map.Close()
Try temp.Close()
Print("left {Dir.Exists(temp.Path)}")
```

Expected: Per the comment, the delete throws on Windows. With a comment that matches the code: write BUSY: can't write ...: <a message saying it's in use>, then deleted True, there False, still mapped True 4, then left False

Actual: write OTHER: can't write C:/Users/Hunter/AppData/Local/Temp/fb-mapdoc-MbexBI/m.bin: unknown error
deleted True, there False, still mapped True 4
left False

Suggested fix: Change the comment to say that on Windows a mapped file can't be replaced or shortened (WriteText, WriteBytes, a truncating write) until it's closed. In files.cpp's write_file, map the sys error ERROR_USER_MAPPED_FILE (uv_fs_t's sys_errno_ on Windows, or a check after UV_UNKNOWN) to UV_EBUSY so the Kind is BUSY. Add a Windows-only line to io_files.fb's maps, or a test that tolerates either OS.

Skeptic: f8.fb printed 'write OTHER: can't write .../m.bin: unknown error' and then 'deleted True, there False, still mapped True 4'. mapped.fb's class comment says 'On Windows, a mapped file can't be deleted until it's closed', but here, with libuv 1.52.1 on Windows 11, the delete succeeded at once and the map stayed readable. What does fail is a truncating write, as ERROR_USER_MAPPED_FILE, which libuv turns into UV_UNKNOWN, so the Kind is OTHER. BUSY is documented as 'a file that's in use or locked', which fits better. The comment is wrong and the Kind is a misreport, so low.

## S14-path-4 (low): Joining a bare same-drive "C:" adds a trailing '/' instead of adding nothing

Status: fixed in dafac5e3

Where: std/io/path.fb:79

Join removes other's drive prefix when the drives match, then sends the rest to Joined even when the rest is empty. Joined then appends "/" + "". Join's own doc says an empty one adds nothing, and Python gives PureWindowsPath('C:/a') / 'C:' == 'C:/a'. The result has a trailing separator and isn't equal to the path it started from. The same happens with a share: "//server/share/x" / "//server/share" gives "//server/share/x/".

```
Import std.core
Import std.io

Function W(text: String): Path => Path(text, PathStyle.WINDOWS)

Print(W("C:/a") / "C:")
Print(W("C:games") / "c:")
Print((W("C:/a") / "C:") == W("C:/a"))
```

Expected: C:/a
C:games
True

Actual: C:/a/
C:games/
False

Suggested fix: At line 79, return Self when added[addedPrefix..] is empty, the same as the empty-other case at line 68.

Skeptic: pathref/p4.fb, debug and release: 'C:/a/', 'C:games/', 'False'. The share case W("//server/share/x") / "//server/share" gives '//server/share/x/'. At path.fb:79, Join passes an empty rest to Joined, and Joined appends '/' + ''. Join's doc says 'an empty one adds nothing', and Python gives PureWindowsPath('C:/a') / 'C:' == 'C:/a'. The share case is worse: IsAbsolute says '//server/share' is absolute, and Join's doc says an absolute other 'is where it says', yet the result is a place inside x. That part comes from the same anchor split as S14-path-1.

## S14-path-5 (low): Absolute of a non-native-style path isn't absolute by its own rules, and calling it again nests the current folder

Status: fixed in dafac5e3

Where: std/io/path.fb:239

Absolute reads the current folder ("C:/..." on Windows, "/..." elsewhere) with the path's own Style. On Windows, a POSIX-style path gets "C:/Users/.../a", which IsAbsolute says isn't absolute under POSIX rules. Calling Absolute on that result prepends the current folder a second time. The design says both rule sets are usable on any machine, and the doc says Absolute gives "the path from the current folder's root" and the path itself when it's already absolute. Neither holds here. The mirror case, a WINDOWS path on Linux or macOS, behaves the same way.

```
Import std.core
Import std.io

Local a = Try Path("a", PathStyle.POSIX).Absolute()
Print(a.IsAbsolute())
Local b = Try a.Absolute()
Print(b == a)
Print(b)
```

Expected: On Windows: True and True (or Absolute refusing a non-native style, as documented)

Actual: False
False
C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s14/path/C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s14/path/a

Suggested fix: Pick a behavior and document it. Either Absolute works only for the target's style (panic at the caller, or throw IOError INVALID_PATH, for the other), or it gives its result in NativeStyle(). Add a test.

Skeptic: pathref/p5.fb on Windows, debug and release: 'False', 'False', then 'C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s14/pathref/C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s14/pathref/a'. Absolute (path.fb:239) reads the native current folder with the path's own Style. Under POSIX rules 'C:/...' isn't absolute, so the result fails its own IsAbsolute, and a second Absolute nests the current folder. The doc says Absolute gives 'the path from the current folder's root ... the path itself when it's absolute', which the result plainly isn't. It only shows with a non-native style, so the severity is low.

## S14-path-6 (low): Docs say Paths sort by their texts' bytes, but '<' compares Style first

Status: fixed in a69db26e, with S14-dir-10

Where: docs/stdlib.md:458

docs/stdlib.md ("sort by their texts' bytes") and path.fb's comment on Path (line 29, "they sort by their texts' bytes") both say ordering is by text. Operator < (path.fb line 55) orders by Style first, then by text, which is the intended rule. So a POSIX "b" sorts before a WINDOWS "a".

```
Import std.core
Import std.io

Print(Path("b", PathStyle.POSIX) < Path("a", PathStyle.WINDOWS))
```

Expected: False, if the docs are right ("b" doesn't sort before "a" by bytes)

Actual: True

Suggested fix: Change both texts to say Paths sort by style (POSIX first) and then by their texts' bytes.

Skeptic: pathref/p6.fb, debug and release: 'True' for Path("b", POSIX) < Path("a", WINDOWS). Operator < (path.fb:54-57) compares Style first, which is the intended lane rule. stdlib.md:457 ('sort by their texts' bytes') and the comment on Path at path.fb:29 both say ordering is by text alone, so the docs contradict the code. The fix is to the docs.

## S14-path-7 (low): WithExtension(".") accepts an extension the result then doesn't have

Status: fixed in 47247d05

Where: std/io/path.fb:156

WithExtension promises "the path with its file name's extension `extension`". "." is accepted and gives "save.", but by Path's own rule a name that ends with a dot has no extension, so Extension() of the result is "", not ".". Python's with_suffix rejects '.' as an invalid suffix. The same happens with "..".

```
Import std.core
Import std.io

Local p = Path("save.dat", PathStyle.POSIX).WithExtension(".")
Print("{p} [{p.Extension()}]")
```

Expected: A panic at the caller for an extension that isn't one (like the separator panic), or a result whose Extension() is "."

Actual: save. []

Suggested fix: Panic at the caller when the extension, with its dot added, ends with '.', the same way as for a separator. Add a panic program for it.

Skeptic: pathref/p7.fb, debug and release: 'save. []' for WithExtension(".") and 'save.. []' for WithExtension(".."). WithExtension's doc promises 'the path with its file name's extension `extension`'. By Path's own rule (ExtensionAt, path.fb:409) a name ending in a dot has no extension, so the result's Extension() is '' and not the extension given. WithExtension only checks for a separator (line 133-135). Python's with_suffix rejects '.'.

## Refuted by the skeptics (no action)

- S14-path-8: WithExtension's no-file-name panic has no panic program. pathref/p8.fb, debug and release: 'panic: WithExtension needs a path with a file name, not '/' at p8.fb:4:7', which is the right message at the caller's line. The behavior is correct. It's true that no program checks it (grep: the only path panic program, panic_io_path_extension.fb, tests the separator panic), but that's a coverage gap, not wrong behavior, and no spec, stdlib.md, or milestones text requires a program for every panic. The fix for S14-path-2 should add this case to its tests anyway.
- S14-file-9: Copy/Move IOError.Path names the source even when the destination is what failed. f9.fb printed 'NOT_FOUND here.txt True' for both Copy and Move into a missing folder, as reported. The message names both paths ('can't copy .../here.txt to .../missing/copy.txt: no such file or directory'). This is a deliberate choice, not a defect: ErrorOf in request.fb always passes the first path as IOError.Path, by design, and IOError has a single Path field that error.fb documents only as 'the file's, when there is one'. Neither the spec nor stdlib.md says which path a two-path call should report, so I'm refuting it as a design choice.
- S14-dir-1: A pending FileWatcher.NextAsync keeps the program from ever ending. I ran hang.fb under `timeout 30` in debug. It printed "end" and then waited until timeout killed it (exit 124), so the behaviour the reviewer describes is real. It isn't a bug by the spec, though. Spec 10 says the main scheduler ticks once top-level code ends "until no coroutine waits on it". The only coroutines it destroys are ones that can never go on, because they wait for a Task on a Scheduler nothing ticks any more (decisions.md line 338 says the same: no Task done, no timer left, no pool work). A coroutine awaiting NextAsync can go on: any change to the folder, made by this program or another, finishes its Task. That makes it like InputAsync, which io.cpp:455-458 also registers with start_request and which also waits for something from outside the program for as long as it takes, or like a long RealSeconds timer, which also keeps the program running. A hot-reload loop that keeps the program alive is the expected behaviour for a pending external wait, as in Node.js. internal.hpp's comment "which it finishes in time" is loose wording, and InputAsync stretches it the same way, but the code matches the spec.
