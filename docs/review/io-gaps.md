# The review of M6 step 32 audit gaps in std.io (step 14/15 follow-ups): File.Open's FileStream, MappedFile's Buffer, zips mounted in a Vfs, typed Preferences, and std.log's file sinks

8 confirmed findings: 0 high, 3 medium, 5 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## mapped-1 (medium): Buffer's view New is Protected, so safe code in any file can extend Buffer and view memory it doesn't own

Status: fixed in fab0c626

Where: std/ffi/buffer.fb:47

This change made `New(first: Ptr<T>, count: Int64, viewed: Buffer<T>?)` Protected so that mapped.fb's MappedBytes could extend Buffer<UInt8>. Spec 06 says Protected reaches the code of every Class extending Buffer, in any file. So a user Class in plain safe code can now build a Buffer from any Ptr and any count, with no check. A Null or C-returned Ptr can come from safe code: calling an Extern isn't Unsafe, and only dereferencing its pointers is. Buffer's [] and []= then dereference `First[index]` inside std's own Unsafe block, so safe code reads and writes arbitrary memory. Spec 12-ffi says only Unsafe code makes a Buffer view of memory it doesn't own (`Buffer<T>.View` is `Unsafe` and checks the count and Null). The Protected New skips both rules. Builds differ too: with a Null pointer, debug panics with "the Ptr is Null", but release dies with an access violation. A second program (malloc(4) viewed as 4294967296 bytes, then `wide[3000000000] = 1`) crashes with 0xC0000005 in both builds, still without any Unsafe.

```
// A Class extending Buffer<UInt8>, in safe code, gives a view of memory nothing keeps.
Import std.core
Import std.ffi

Class Wild Extends Buffer<UInt8>
    New()
        Super.New(Null, 1000000, Null)
    End New
End Class

Local wild: Buffer<UInt8> = Wild()
Print("{wild.Count}")
Print("{wild[500000]}")
Print("not reached")
```

Expected: A compile error at `Super.New(Null, 1000000, Null)`: safe code outside std can't reach Buffer's raw view constructor, just as it can't call `Buffer<T>.View` outside Unsafe.

Actual: It compiles. Debug prints "1000000", then "panic: the Ptr is Null at evil.fb:13:13". Release (`fb run --release`) prints nothing and exits with "error: evil.exe exited with code 0xC0000005".

Suggested fix: Make the view constructor `Package New(...)` instead of Protected. std is one package (the lane already uses Package for vfs.fb's NamesOf, which std.compress calls), so MappedBytes can still call it and user Classes can't. If it must stay reachable from outside std, make it Unsafe, with View's checks (count >= 0, no Null when count > 0, no Int64 overflow of the byte count). Add a compile-fail test showing a user Class can't call it.

Skeptic: Ran the reviewer's evil.fb (rv-iogaps/mapped-refute/evil.fb) with no Unsafe anywhere. It compiles. Debug prints "1000000", then "panic: the Ptr is Null at evil.fb:13:13" (exit 101). --release prints nothing and dies with "exited with code 0xC0000005". In rev-4, std/ffi/buffer.fb:47 is now `Protected New(first: Ptr<T>, count: Int64, viewed: Buffer<T>?)`, the diff changes it from file-private, and it checks nothing. Spec 06 line 449/473 says Protected reaches the code of a Class extending it, in any file. Spec 12 line 128 says only Unsafe code makes `Buffer<T>.View(pointer, count)`, a view of memory C owns. View (buffer.fb) is Unsafe and checks for a negative count, a Null pointer with count > 0, and Int64 overflow. Through the Protected New, safe user code skips all of that and gets out-of-bounds memory access, and debug and release behave differently. That plainly contradicts the spec. The lane made New Protected only so MappedBytes, inside std, could extend Buffer. Making it Package keeps that working.

## stream-1 (medium): File.Create (Create + Truncate) is PERMISSION_DENIED on an existing hidden file on Windows

Status: fixed in 5f499702 (a write's replacing in File.WriteText and WriteBytes too, which now looks for a folder first, as Open does)

Where: runtime/src/files.cpp:1058

open_request adds UV_FS_O_TRUNC whenever Truncate comes with Create or CreateNew, and libuv turns O_CREAT|O_TRUNC into CreateFileW's CREATE_ALWAYS. Windows refuses CREATE_ALWAYS on a file with the hidden (or system) attribute unless the call asks for those same attributes, so File.Create, File.CreateAsync, and Open(Write, Create, Truncate) all fail with PERMISSION_DENIED on a hidden file that exists. Write + Truncate without Create works on the same file, because the lane already empties that case with ftruncate after opening it. Rust's File::create used to have the same bug and now opens the file and then truncates it. File.WriteText has the same CREATE_ALWAYS behaviour from before this change.

```
Import std.core
Import std.ffi
Import std.io
Import std.text

Extern "C"
    Function system(command: CString): Int32
End Extern

// A hidden file, as Windows' attrib +h (or Explorer) makes one, is emptied and written by
// File.Create, as it is by Open with Write and Truncate.
Local temp = Try TempDir("fb-p9-")
Local p = temp.Path / "settings.txt"
Try File.WriteText(p, "old")
system("attrib +h \"{p.Text.Replace("/", "\\")}\"")
Try
    Local s = Try File.Open(p, OpenOptions(Write: True, Truncate: True))
    Try s.WriteText("one")
    s.Close()
    Print("Write+Truncate: {Try File.ReadText(p)}")
Catch e As IOError
    Print("Write+Truncate: {e.Kind}")
End Try

Try
    Local s = Try File.Create(p)
    Try s.WriteText("two")
    s.Close()
    Print("Create: {Try File.ReadText(p)}")
Catch e As IOError
    Print("Create: {e.Kind}")
End Try

Try temp.Close()
```

Expected: Write+Truncate: one
Create: two

Actual: Write+Truncate: one
Create: PERMISSION_DENIED   (debug and release alike)

Suggested fix: Never pass UV_FS_O_TRUNC. Empty every Truncate with ftruncate once the file is open, as the no-Create case already does. With CreateNew the file is new and empty, so it needs no ftruncate. Add a test (Windows only, or one that sets the hidden attribute) for File.Create on a hidden file. The same change would fix File.WriteText's open.

Skeptic: Ran the program (plus a File.WriteText line) at scratchpad/rv-iogaps/stream/p9.fb, debug and release: 'Write+Truncate: one', 'Create: PERMISSION_DENIED', 'WriteText: PERMISSION_DENIED'. runtime/src/files.cpp open_request (about line 1056) adds UV_FS_O_TRUNC when truncates && creates, which libuv turns into CREATE_ALWAYS, and Windows refuses that on a hidden file. The no-Create path uses ftruncate after opening (line 1076) and works on the same file. stdlib.md says File.Create gives a file 'made or emptied', so a hidden settings file that exists can't be recreated through File.Create, CreateAsync, or Open(Write, Create, Truncate). The fix is the one suggested: always ftruncate after the open.

## vfs-1 (medium): RotatingFileSink deletes the old files on every retry while the current file can't be moved aside

Status: fixed in 1c2bf18e (a folder where an old file goes is found before anything moves, and a failed move is tried again once another maxBytes are in the file)

Where: std/log/files.fb:116

Rotate deletes Old(Keep) and shifts each .i to .i+1 before it tries File.Move(Location, Old(1)). If that last move fails (the case the docs say is handled: the file open in another program on Windows, or something at <path>.1), Add sets Unmoved but leaves state.Size alone. So every later line calls Rotate again, and each call deletes the new oldest file and shifts the rest. After keep lines, every old generation is gone, though no rotation ever happened. The docs promise that a file that can't be moved aside gets the lines meanwhile. They don't say its old files get wiped. log_files' 'stuck' case uses keep 1 with a folder at .1, so its Delete fails first and the loss never shows up.

```
Import std.core
Import std.io
Import std.log
Import std.time

Const at = Timestamp.FromUnixMilliseconds(1792800000123)
Function Line(message: String): LogRecord => LogRecord(at, LogLevel.INFO, "app", message)
Function Show(path: Path): String
    If !File.Exists(path) Then Return "none"
    Return "{(Try? File.ReadLines(path)) ?? []}"
End Function

Local temp = Try TempDir("fb-rot-")
Local before = Try Dir.Current()
Try Dir.SetCurrent(temp.Path)
// Old generations from earlier rotations, and a .1 the current file can't be moved onto.
Try File.WriteText("app.log", "current\n")
Try File.WriteText("app.log.2", "two\n")
Try File.WriteText("app.log.3", "three\n")
Try Dir.Create("app.log.1")
Local sink = RotatingFileSink("app.log", 10, 3)
For i = 1 To 3
    sink(Line("line {i}"))
End For
Print("{Show("app.log.2")} {Show("app.log.3")}")
Try Dir.SetCurrent(before)
Try temp.Close()
```

Expected: std.log: can't move app.log to app.log.1: ... (once on stderr), then
[two] [three]
(nothing was moved aside, so the old files stay)

Actual: std.log: can't move app.log to app.log.1: illegal operation on a directory, so lines go on being added to app.log until it can be moved aside
none none
(the same in debug and --release)

Suggested fix: Check that the move can happen before deleting or shifting anything, or move Location to a temporary name beside it first and shift the old files only after that works. Also stop retrying on every line once a move has failed (back off, or retry only after another maxBytes). Add a log_files case with keep >= 2 and old files present while the move fails.

Skeptic: I ran the finding's program, extended to also show app.log.4 and app.log, in rv-iogaps/logcheck/v1.fb. Debug and --release both printed the stderr line once ('std.log: can't move app.log to app.log.1: illegal operation on a directory, ...') and then 'none none none [current, ...line 1, ...line 2, ...line 3]'. So app.log.2 and app.log.3 were destroyed even though nothing was ever moved aside. The cause is in std/log/files.fb's Rotate (lines 116-121): it runs File.Delete(Old(Keep)) and shifts each Old(i) to Old(i+1) before it tries File.Move(Location, Old(1)). In Add (lines 74-88), a failed move only sets Unmoved and leaves state.Size alone, so every later line calls Rotate again and loses another generation. That contradicts stdlib.md's promise that 'a file that can't be moved aside gets the lines meanwhile', and the lane note that the old ones stay. log_files' 'stuck' case uses keep 1, so it can't show this.

## mapped-2 (low): Bytes' comment says writing a read-only map's Buffer changes only it and later Buffers, but earlier ones change too

Status: fixed in ceb054d7

Where: std/io/mapped.fb:108

MappedFile.Bytes' doc comment says a read-only map's Buffer "copies a page of the file's as it's first written, so writing it changes only that Buffer, and the Buffers this gives after, never the file". The runtime makes one copy-on-write view per map (`map_t::copied`, made on the first fb_io_map_hold) and returns it to every Bytes() call. So every Buffer from that map, earlier or later, shares the same memory, and writing any one of them changes all the others. Writing the second Buffer changes the first, which the comment rules out. The behavior is reasonable; the comment is wrong. docs/stdlib.md's wording ("a copy-on-write view of its own") is ambiguous but not wrong.

```
// A read-only MappedFile's Buffers: writing a later one changes an earlier one too.
Import std.core
Import std.ffi
Import std.io

Local temp = Try TempDir("fb-two-")
Try File.WriteBytes(temp.Path / "three.bin", [1, 2, 3])
Local map = Try MappedFile(temp.Path / "three.bin")
Local first = Try map.Bytes()
Local second = Try map.Bytes()
second[0] = 99
Print("{first[0]} {second[0]} {map[0]}")
first = Buffer<UInt8>(0)
second = Buffer<UInt8>(0)
map.Close()
Try temp.Close()
Print("{Dir.Exists(temp.Path)}")
```

Expected: Going by the comment, writing `second` changes only it and later Buffers, so `first[0]` stays 1: "1 99 1", then "False".

Actual: "99 99 1", then "False" (debug and release): `first` sees what `second` wrote.

Suggested fix: Reword the comment to match the code, for example: "A read-only one's Buffers share a view of their own, which copies a page of the file's as it's first written, so writing one changes every Buffer this gives, never the file or the MappedFile's []". Optionally add the two-Buffer case to tests/programs/io_mapped_buffer.fb.

Skeptic: Ran the reviewer's two.fb (rv-iogaps/mapped-refute/two.fb). Debug and release both print "99 99 1" then "False": writing the second Buffer from Bytes() changed the first one. The doc comment on MappedFile.Bytes in std/io/mapped.fb (around lines 105-111) says writing a read-only map's Buffer "changes only that Buffer, and the Buffers this gives after, never the file". That rules out an earlier Buffer seeing the write. The code returns one shared copy-on-write view per map, so the comment is wrong. The behavior itself is reasonable, and stdlib.md line 746 ("a copy-on-write view of its own") is consistent with it. Only the comment is wrong.

## stream-2 (low): Reading near Int64's top, or writing past the file system's limit, throws an IOError of INVALID_PATH

Status: fixed in c26adf1f

Where: runtime/src/files.cpp:1091

Seek accepts any place an Int64 can count, and the FileStream docs and test say a read past the end gives nothing, which is what MemoryStream does. When the place plus the Buffer's Count passes Int64's top, read_at hands libuv an offset that the OS rejects with EINVAL, and Read throws instead of returning 0. A write at a place the file system can't hold (1 << 62 on NTFS) is EINVAL too. fb_io_error_kind maps EINVAL to INVALID_PATH, so either failure is reported as a bad path, and the message reads "can't read <path>: invalid argument", though the path is fine. On POSIX the same write is EFBIG, which maps to NO_SPACE, so the Kind also differs between OSes.

```
Import std.core
Import std.ffi
Import std.io

// A Seek takes a FileStream anywhere an Int64 counts, and past the end a read gives nothing, as a
// MemoryStream's does; a write the file system can't put there isn't a path's fault.
Local temp = Try TempDir("fb-p12-")
Local p = temp.Path / "a.txt"
Try File.WriteText(p, "hello")
Local s = Try File.Open(p, OpenOptions(Read: True, Write: True))
Local memory = MemoryStream()
Local top: Int64 = 9223372036854775807
Try s.Seek(top - 1, SeekOrigin.START)
Try memory.Seek(top - 1, SeekOrigin.START)
Print("memory {Try memory.Read(Buffer<UInt8>(4))}")
Try
    Print("file {Try s.Read(Buffer<UInt8>(4))}")
Catch e As IOError
    Print("file {e.Kind}: {e.Message}")
End Try

Local far: Int64 = 1
Try s.Seek(far << 62, SeekOrigin.START)
Try
    Try s.WriteText("x")
    Print("wrote")
Catch e As IOError
    Print("write {e.Kind}")
End Try

s.Close()
Try temp.Close()
```

Expected: memory 0
file 0
write NO_SPACE (or another Kind that isn't a path's, the same on every OS)

Actual: memory 0
file INVALID_PATH: can't read C:/Users/Hunter/AppData/Local/Temp/fb-p12-.../a.txt: invalid argument
write INVALID_PATH   (debug and release alike)

Suggested fix: In fb_io_stream_read and stream_read, clamp count to INT64_MAX - offset, and return 0 when that leaves nothing to read. In the stream's write paths, report an offset EINVAL as UV_EFBIG (NO_SPACE, as POSIX does), or as some other non-path Kind, rather than as INVALID_PATH. Add both cases to io_file_stream.

Skeptic: Ran scratchpad/rv-iogaps/stream/p12.fb, debug and release: 'memory 0', then 'file INVALID_PATH: can't read C:/.../fb-p12-.../a.txt: invalid argument', then 'write INVALID_PATH'. io_file_stream.fb line 48 says 'At the end, and past it, a read gives nothing', and Seek accepts top-1. But read_at passes offset+count past INT64_MAX to uv_fs_read, which gives EINVAL, and fb_io_error_kind maps UV_EINVAL to 10 (INVALID_PATH) while UV_EFBIG maps to 9 (NO_SPACE), so the same write gets a different Kind on POSIX. This is an edge case, so low.

## stream-4 (low): Tests don't check File.Create emptying an existing file, the Create-without-a-write panic, or Close while an Async call is under way

Status: fixed in 5f499702 (File.Create on a file that's there) and 8f84e855

Where: tests/programs/io_file_stream.fb:34

Every File.Create and CreateAsync in io_file_stream makes a new file, so the Create + Truncate path on a file that already exists, which is File.Create's documented 'emptied when it is', is never tested; only Truncate without Create is (line 90). Of OpenOptions.Check's three panics, the panic_io_stream_ tests cover 'none of Read, Write, Append' and 'Truncate with Append'. 'Create, CreateNew, or Truncate without a write' ("only a file opened to write or append to is made or emptied") is untested, as are Close's 'can't close while a call is on it' panic and an @PanicsAtCaller panic from OpenAsync. No test in tests/ contains either panic message. The program shows that the behaviour is right today; nothing keeps it right.

```
Import std.core
Import std.io

// File.Create empties a file that's there, which io_file_stream never checks (each of its Creates
// makes a new file), and Create without a write panics, which no panic_io_stream_ test checks.
Local temp = Try TempDir("fb-p11-")
Local p = temp.Path / "a.txt"
Try File.WriteText(p, "hello world")
Local made = Try File.Create(p)
Print("{Try made.Size()}")
made.Close()
Try temp.Close()
Try File.Open("missing.txt", OpenOptions(Read: True, Create: True))
```

Expected: 0
panic: only a file opened to write or append to is made or emptied at p11.fb:13:5 (with both covered by tests in tests/programs)

Actual: 0
panic: only a file opened to write or append to is made or emptied at p11.fb:13:5. Neither is checked by any test: grep for 'made or emptied' or "can't close while" in tests/ finds nothing, and io_file_stream never calls File.Create on an existing file.

Suggested fix: In io_file_stream, call File.Create on an existing file and print its Size (0) and contents. Add panic_io_stream_create.fb (Read: True, Create: True) and panic_io_stream_close_busy.fb (Close while a WriteAsync's Task is held and not yet awaited). Optionally add an Await File.OpenAsync with bad options to confirm the panic is reported at the caller.

Skeptic: Ran scratchpad/rv-iogaps/stream/p11.fb, debug and release: prints '0', then 'panic: only a file opened to write or append to is made or emptied at p11.fb:11:5', so the behaviour is right today. grep -rl for 'made or emptied' and for "can't close while" in tests/ finds nothing. panic_io_stream_options.fb uses OpenOptions(Create: True) with no Read, Write, or Append, so it hits the first check ('none of'), not the 'made or emptied' one. Every File.Create and CreateAsync in io_file_stream.fb (lines 34, 111, 124, 131, 140) makes a new file, and line 90 is Truncate without Create. So the Create+Truncate-on-an-existing-file path, a separate code path (O_TRUNC) that stream-1 shows can break, is untested.

## vfs-2 (low): FileSink and RotatingFileSink re-resolve a relative path against the current folder on every line

Status: fixed in 0be7ea30 (its messages name the path as the program gave it, with the error's Kind)

Where: std/log/files.fb:91

LogFile keeps the Path as it was given, and each line opens it fresh with File.AppendText(Location, ...). A relative path like FileSink("app.log") is therefore resolved against whatever the current folder is when each line is written, not the folder when the sink was made. Once a program calls Dir.SetCurrent, its log quietly moves to another file. For a rotating sink, state.Size still counts the old file, and Rotate moves the files in the new folder. The docs and the comment say the lines go to the end of 'the file at `path`'. log_files sets the current folder before making any sink, so it never sees this.

```
Import std.core
Import std.io
Import std.log
Import std.time

Const at = Timestamp.FromUnixMilliseconds(1792800000123)
Function Line(message: String): LogRecord => LogRecord(at, LogLevel.INFO, "app", message)

Local temp = Try TempDir("fb-cwd-")
Local before = Try Dir.Current()
Try Dir.CreateAll(temp.Path / "one")
Try Dir.CreateAll(temp.Path / "two")
Try Dir.SetCurrent(temp.Path / "one")
Local sink = FileSink("app.log")
sink(Line("first"))
Try Dir.SetCurrent(temp.Path / "two")
sink(Line("second"))
Print("{(Try File.ReadLines(temp.Path / "one" / "app.log")).Count} {File.Exists(temp.Path / "two" / "app.log")}")
Try Dir.SetCurrent(before)
Try temp.Close()
```

Expected: 2 False

Actual: 1 True

Suggested fix: Make the path absolute when FileSink or RotatingFileSink is called (Dir.Current() / path when it isn't IsAbsolute), and keep that in LogFile.Location. Add a log_files case that changes the current folder after making a sink.

Skeptic: I ran the finding's program (rv-iogaps/logcheck/v2.fb). Debug and --release both printed '1 True': after Dir.SetCurrent, the second line went to two/app.log rather than to the file the sink was made for. LogFile keeps the Path as given (files.fb:57-58), and each line calls File.AppendText(Location, line) (line 91), so a relative path is resolved again against the current folder on every write. For a RotatingFileSink, state.Size (Known is set once) then counts the old file while Rotate moves files in the new folder. That contradicts 'the file at `path`' in the doc comment and in stdlib.md. I set it to low because it needs a program that changes its current folder after making a sink. Still, the log goes to a different file without any warning.

## vfs-3 (low): RotatingFileSink's panics are untested, and stdlib.md leaves out them and keep's default

Status: fixed in 736e6f74

Where: docs/stdlib.md:930

RotatingFileSink panics at the caller when maxBytes <= 0 or keep < 0, and keep defaults to 5. A keep of 0 deletes the old lines. No tests/programs/panic_* program covers either panic, though std's other panics each have one (this step added panic_io_stream_* for FileStream). stdlib.md's std.log bullet writes it as `RotatingFileSink(path, maxBytes, keep)` and mentions none of the panics, the default of 5, or what a keep of 0 does.

```
Import std.core
Import std.log

Local sink = RotatingFileSink("x.log", 0)
```

Expected: panic: a rotating file holds a most of bytes above 0, not 0 at panic1.fb:4:14 (covered by a panic_log_rotating_*.fb/.out test, with keep -1 too, and described in stdlib.md)

Actual: It panics as expected, but no test program covers this or the keep -1 panic, and stdlib.md says nothing about either, the default of 5, or keep 0.

Suggested fix: Add panic_log_rotating_bytes and panic_log_rotating_keep programs and their .out files. In stdlib.md, write `RotatingFileSink(path, maxBytes, keep = 5)`, say that maxBytes must be above 0 and keep not negative or it panics, and say that a keep of 0 drops the old lines.

Skeptic: I ran the finding's program (rv-iogaps/logcheck/v3.fb). Debug and release both print 'panic: a rotating file holds a most of bytes above 0, not 0 at v3.fb:4:14', exit 101. So the panic itself works. In tests/programs there is no panic_log_* program; grep for RotatingFileSink in tests/ finds only log_files.fb, which never reaches either panic. The same step added a panic_io_stream_* program for each FileStream panic, and std.compress has one per panic. The milestones row 32 says 'every module in docs/stdlib.md exists, documented and tested'. stdlib.md:930 writes `RotatingFileSink(path, maxBytes, keep)` and leaves out the default of 5, both panics, and what a keep of 0 does. In the same step it does document FileStream's option panics (stdlib.md:733). The test gap and the doc gap are both real, and both are minor.

## Refuted by the skeptics (no action)

- stream-3: CreateNew on a path that's a folder gives IS_A_DIRECTORY, not ALREADY_EXISTS as OpenOptions.CreateNew documents. Reproduced: scratchpad/rv-iogaps/stream/p8.fb prints IS_A_DIRECTORY in debug and release. But it isn't a contradiction. CreateNew's doc and stdlib.md say it refuses 'a file that's there', and a folder isn't a file. open_request deliberately checks for a folder before opening anything (files.cpp line 1060) so every OS gives IS_A_DIRECTORY for a folder, whatever the options, which is consistent with io_file_stream's folder cases (Read and Write on a folder). The more specific Kind for a folder is a reasonable lane choice, and the doc doesn't plainly require ALREADY_EXISTS for one. Unsure, so refuted.
