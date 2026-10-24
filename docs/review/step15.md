# The review of M6 step 15's std part: std.io's Vfs (mounts of folders, files in memory, and a program's own sources) and Preferences (a program's saved settings)

17 confirmed findings: 0 high, 6 medium, 11 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## prefs-1 (medium): A setting changed while SaveAsync is writing isn't saved, yet IsChanged says False

Status: fixed in ceb7beb5

Where: std/io/preferences.fb:128

SaveAsync takes its snapshot (TextOf) after its first Await, then Awaits the atomic write, then sets Changed = False no matter what. Other coroutines run while the write is under way. A Set made then is left out of the file, but its Changed = True is wiped when the write finishes. So IsChanged says there's nothing to save when there is. This breaks the usual autosave pattern: SaveAsync fired off, and the program checks IsChanged before quitting. The program printed the same thing in 5 debug and 3 release runs.

```
Import std.core
Import std.io

Local temp = Try TempDir("fb-prefs-race-")
Local root = temp.Path
Local p = Try Preferences("Game", root)
p.Set("volume", "0.8")

// A setting changed while SaveAsync's write is under way.
Async Function Later(p: Preferences, root: Path) Throws IOError
    Try Await Dir.CreateAllAsync(root)
    Try Await Dir.CreateAllAsync(root)
    p.Set("late", "yes")
End Function

Local saving = p.SaveAsync()
Local later = Later(p, root)
Try Await saving
Try Await later
Local saved = Try Preferences("Game", root)
Print("{p.Get("late") ?? "none"} {saved.Get("late") ?? "none"} {p.IsChanged()}")
Try temp.Close()
```

Expected: yes none True   ("late" isn't in the file, so there's still something to save)

Actual: yes none False

Suggested fix: Keep a change count: a Field, say Version: Int64, that Set, Remove, and Clear add 1 to whenever they set Changed. Save and SaveAsync read Version just before TextOf, and set Changed = False after the write only if Version hasn't moved since. Add this program, or one like it, to io_preferences.fb.

Skeptic: I ran the finding's program as written (rv-step15/prefs-refute/p1.fb). It printed "yes none False" in 3 debug and 2 release runs. In std/io/preferences.fb lines 125-129, SaveAsync takes TextOf(App, Values) after its first Await, then Awaits WriteTextAtomicAsync, then sets Changed = False unconditionally. A Set made by another coroutine during the write is left out of the file, and its Changed flag is still cleared. The class comment (line 22) and lane decision 7 both say IsChanged tells whether there's something to save, so this contradicts them.

## prefs-2 (medium): On Linux and macOS, a file where the app's folder goes makes Preferences/OpenAsync throw, so io_preferences.fb fails there

Status: fixed in a5bf91a0 (its test fails without it on Linux and macOS; Windows says NOT_FOUND there anyway)

Where: std/io/preferences.fb:159

Loaded and LoadedAsync treat only NOT_FOUND as 'no file yet'. Say <folder>/<app> is a file. On Windows, opening <folder>/<app>/preferences.txt gives ERROR_PATH_NOT_FOUND (NOT_FOUND), so no settings are read and only Save throws NOT_A_DIRECTORY. On POSIX, open(2) gives ENOTDIR (shown in WSL Ubuntu with a C program opening "Blocked/preferences.txt": -1, errno 20, "Not a directory"). libuv returns that as UV_ENOTDIR, which fb_io_error_kind maps to NOT_A_DIRECTORY, so the constructor and OpenAsync rethrow it. The doc comment says 'or none when there's no file yet', so the OSes disagree. The step's own test hits this: tests/programs/io_preferences.fb line 68, `Local blocked = Try Preferences("Blocked", root)`, throws at top level on Linux and macOS. Its .out ("NOT_A_DIRECTORY" then "1 True") can't match there, so CI fails on 2 of its 3 OSes. vfs.fb's Missing() already treats NOT_A_DIRECTORY as missing. The Windows output below is from running the program. The Linux/macOS output is inferred from the C check plus the code path, since there's no Linux fb build here.

```
// A file where the app's folder goes: there's no preferences file, so the docs say none are read.
Import std.core
Import std.io

Local temp = Try TempDir("fb-prefs-blocked-")
Local root = temp.Path
Try File.WriteText(root / "Blocked", "a file where the app's folder goes")
Try
    Local blocked = Try Preferences("Blocked", root)
    Print("read {blocked.Count()}")
Catch e As IOError
    Print("{e.Kind}")
End Try

Try
    Local opened = Try Await Preferences.OpenAsync("Blocked", root)
    Print("read {opened.Count()}")
Catch e As IOError
    Print("{e.Kind}")
End Try
Try temp.Close()
```

Expected: read 0
read 0   (on every OS, as on Windows)

Actual: Windows: read 0 / read 0. Linux and macOS: NOT_A_DIRECTORY / NOT_A_DIRECTORY (open(2) gives ENOTDIR), and io_preferences.fb fails at its line 68.

Suggested fix: In Loaded and LoadedAsync, also return an empty Map for IOErrorKind.NOT_A_DIRECTORY, since no file can be there. Save then throws NOT_A_DIRECTORY on every OS, as the test expects. Alternatively, keep the throw and change both the docs and the test, but then every OS must agree.

Skeptic: On Windows the program (p2.fb) printed "read 0 / read 0", and a direct File.ReadLines of Blocked/preferences.txt gave NOT_FOUND. On Linux (WSL Ubuntu, python3 os.open of <tmp>/Blocked/preferences.txt where Blocked is a file) I got "20 Not a directory". read_file (files.cpp line 201) calls uv_fs_open, which returns -errno (UV_ENOTDIR) on POSIX. fb_io_error_kind maps that to 5, which is NOT_A_DIRECTORY. Loaded and LoadedAsync (preferences.fb lines 159 and 168) only swallow NOT_FOUND, so they rethrow on POSIX. tests/programs/io_preferences.fb line 68 (`Local blocked = Try Preferences("Blocked", root)`) is a top-level Try outside any Try block, so the program ends there on Linux and macOS and can't match its .out. The OSes disagree, and the constructor's comment says none are read when there's no file.

## vfs-1 (medium): On Windows, a Vfs path whose first name below a folder mount is a drive ("C:") reads outside the mounted folder

Status: fixed in 22a11d9f

Where: std/io/vfs.fb:38

The docs promise that no Vfs path goes above its root (decision 3, stdlib.md: "none goes above its root"). NamesOf only blocks "..". FolderSource builds its disk path with `Folder / path` (Read line 38, KindAt 47, List 57, and ReadBytesAsync 199). Path.Join treats an `other` that starts with a drive as absolute and returns it unchanged. So on Windows, "data/C:/any/file" goes through a mount at "data" and reads, lists, or stats any file on the machine. This works in debug and release. Any path that comes from data (a mod's manifest, a save, the network) can read outside the sandbox. A backslash form like "\\.\C:\x" reaches the same place, because '.' is dropped and "C:" becomes the first name.

```
// A Vfs path whose first name below a folder's mount is a drive, like "C:", reads outside the
// folder on Windows.
Import std.collections
Import std.core
Import std.io

Local temp = Try TempDir("fb-vfs-rv-")
Local root = temp.Path
Try Dir.CreateAll(root / "game")
Try File.WriteText(root / "game" / "inside.txt", "inside")
Try File.WriteText(root / "secret.txt", "secret")

Local vfs = Vfs()
vfs.MountFolder("data", root / "game")

// "data/C:/Users/.../secret.txt": names in the Vfs, none of them "..".
Local outside = "data/" + (Try (root / "secret.txt").Absolute()).Text
Try
    Print("read: {Try vfs.ReadText(outside)}")
Catch e As IOError
    Print("read: {e.Kind}")
End Try

Print("exists: {vfs.Exists(outside)}")
Try temp.Close()
```

Expected: read: NOT_FOUND (or INVALID_PATH)
exists: False

Actual: read: secret
exists: True

Suggested fix: Keep a FolderSource's path inside its folder. Don't use Path.Join for the inner path: on Windows, refuse (give Null for) any name that holds ':', or check that the joined path still starts with Folder's text. Add a test that reads "data/C:/..." and expects it to fail.

Skeptic: Ran refute/v1.fb in debug and release. With a mount at data, the path data/C:/Users/Hunter/AppData/Local/Temp/fb-vfs-rv-.../secret.txt printed 'read: secret', 'exists: True', 'async: secret', and Entries("data/C:/") listed the drive's root (1 entry). Code: NamesOf (vfs.fb:377) only stops '..'. FolderSource builds Folder / path (lines 38, 47, 57, 199), and Path.Join (path.fb:73-77) returns `other` unchanged when it starts with a drive and a root. That breaks stdlib.md's "none goes above its root". Only Windows is affected, since '/' always splits names. Trailing-dot names like '.. ' and '...' don't escape (refute/v7b.fb: all NOT_FOUND).

## vfs-2 (medium): ReadBytesAsync panics with an index out of range when mounts are taken out while it waits on a folder's read

Status: fixed in 9c826d90

Where: std/io/vfs.fb:191

ReadBytesAsync loops `For i = Mounts.Count - 1 To 0 Step -1` and Awaits File.ReadBytesAsync inside the loop. Other code runs while it waits. If that code Unmounts, `Mounts[i]` on the next turn of the loop is out of range and std panics. This happens in debug and release. A Mount made during the wait is skipped, and an Unmount of a lower index shifts the mounts, so one is read twice or skipped. Toggling a mod while assets load async is a normal way to hit it.

```
// Unmounting while a ReadBytesAsync waits on a folder's read.
Import std.collections
Import std.core
Import std.io

Local temp = Try TempDir("fb-vfs-rv-")
Local root = temp.Path
Try Dir.CreateAll(root / "a")
Try Dir.CreateAll(root / "b")

Local vfs = Vfs()
vfs.MountEmbedded("", {"x.txt": "from memory".Bytes})
vfs.MountFolder("", root / "a")
vfs.MountFolder("", root / "b")

// It starts with b's read, the last mount's, and waits on it.
Local task = vfs.ReadTextAsync("x.txt")
// Meanwhile the program takes out the two folders.
Print("unmounted {vfs.Unmount("")} {vfs.Unmount("")}")
Try
    Print("read [{Try Await task}]")
Catch e As IOError
    Print("read {e.Kind}")
End Try

Try temp.Close()
```

Expected: unmounted True True
read [from memory]

Actual: unmounted True True
panic: index 1 is out of range for 1 element at std/io/vfs.fb:191:34

Suggested fix: Take a snapshot of the mounts at the start (`Local mounts = Mounts.Clone()`) and loop over the snapshot, so a read sees the mounts as they were when it began. Add a test that unmounts during a read.

Skeptic: Ran refute/v2.fb. Debug and release both printed 'unmounted True True' and then 'panic: index 1 is out of range for 1 element at std/io/vfs.fb:191:34'. ReadBytesAsync (vfs.fb:190-204) indexes Mounts[i] again after the Await at line 199, so an Unmount during the wait leaves i out of range. Nothing in the docs makes an Unmount during an Async read misuse.

## vfs-3 (medium): The Async twins skip an Override of FolderSource.Read, so ReadTextAsync and ReadText return different bytes

Status: fixed in 8ed08c93

Where: std/io/vfs.fb:193

FolderSource isn't Final, so a program can extend it and override Read, for example to decrypt or transform files. ReadBytesAsync tests `Mounts[i].Source As? FolderSource`, which also matches every subclass, then Awaits File.ReadBytesAsync(folder.Folder / inner) and never calls the override. ReadBytes, ReadText, and ReadLines use the override, but ReadBytesAsync, ReadTextAsync, and ReadLinesAsync return the raw file. Since Interfaces can't have Async Methods yet (decision 4), extending FolderSource is the natural way to customize a folder mount.

```
// A program's own FolderSource, whose Read changes what it reads, read both ways.
Import std.collections
Import std.core
Import std.io

// A folder whose files are kept with each byte one higher, as a simple scrambling.
Class Scrambled Extends FolderSource
    New(folder: Path)
        Super.New(folder)
    End New

    Override Method Read(path: String): UInt8[]? Throws IOError
        Local bytes = Try Super.Read(path)
        If bytes == Null Then Return Null
        For i = 0 Until bytes.Count
            bytes[i] = bytes[i] - 1
        End For

        Return bytes
    End Method
End Class

Local temp = Try TempDir("fb-vfs-rv-")
Local root = temp.Path
Try File.WriteText(root / "secret.txt", "Ifmmp")

Local vfs = Vfs()
vfs.Mount("", Scrambled(root))
Print("ReadText [{Try vfs.ReadText("secret.txt")}]")
Print("ReadTextAsync [{Try Await vfs.ReadTextAsync("secret.txt")}]")

Try temp.Close()
```

Expected: ReadText [Hello]
ReadTextAsync [Hello]

Actual: ReadText [Hello]
ReadTextAsync [Ifmmp]

Suggested fix: Make FolderSource Final, or take the async path only when the source's type is exactly FolderSource and call Read otherwise. Add a test with a subclass.

Skeptic: Ran refute/v3.fb in debug and release. It printed 'ReadText [Hello]' then 'ReadTextAsync [Ifmmp]'. decisions.md:107 says Methods are virtual unless Final, and FolderSource isn't Final, so the Override compiles. ReadBytesAsync's `As? FolderSource` (vfs.fb:193) matches the subclass and calls File.ReadBytesAsync directly, skipping its Read. The Async twin silently gives different bytes than ReadBytes. Decision 4 never meant twins to disagree.

## vfs-4 (medium): A folder mount over a missing folder is listed as a folder, but KindOf says nothing is there and Walk and Glob of the whole Vfs throw NOT_FOUND

Status: fixed in f734aa7e

Where: std/io/vfs.fb:320

VfsSource's contract says a source's root "is always a folder". FolderSource breaks it when its folder doesn't exist: KindAt("") and List("") return Null. Listed then lists the mount point as a DIRECTORY of its parent, because a mount below is a folder in it. But KindOf(mount point) returns Null, DirExists returns False, and Entries(mount point) throws NOT_FOUND. Walk() and Glob("", "**/...") descend into it and throw, so a mods folder the player hasn't made yet breaks a walk of the whole game. The test mounts its mod folder at the same place as the embedded files, so it never hits this.

```
// A mount whose folder isn't there yet, like a mods folder a player hasn't made.
Import std.collections
Import std.core
Import std.io

Local temp = Try TempDir("fb-vfs-rv-")
Local root = temp.Path

Local vfs = Vfs()
vfs.MountEmbedded("", {"readme.txt": "the game".Bytes})
vfs.MountFolder("mods", root / "mods")

For entry In Try vfs.Entries()
    Print("root has {entry.Name()} {entry.Kind}")
End For

Local kind = Try vfs.KindOf("mods")
Print("mods is {kind == Null ? "none" : "{kind}"}, DirExists {vfs.DirExists("mods")}")
Try
    Print("mods has {(Try vfs.Entries("mods")).Count}")
Catch e As IOError
    Print("mods: {e.Kind}")
End Try

Try
    Print("walked {(Try vfs.Walk()).Count}")
Catch e As IOError
    Print("walk: {e.Kind} {e.Message}")
End Try

Try temp.Close()
```

Expected: root has mods DIRECTORY
root has readme.txt FILE
mods is DIRECTORY, DirExists True
mods has 0
walked 2

Actual: root has mods DIRECTORY
root has readme.txt FILE
mods is none, DirExists False
mods: NOT_FOUND
walk: NOT_FOUND can't list mods: no mount of the Vfs has it

Suggested fix: Make a mount point a folder everywhere. Either FolderSource keeps the contract (KindAt("") is DIRECTORY, List("") is [] when the folder is missing), or the Vfs treats a mount exactly at `names` as found in Listed and as DIRECTORY in KindOf. Add a test with a missing folder mounted at its own place.

Skeptic: Ran refute/v4.fb in debug and release. Entries() listed 'mods DIRECTORY', yet it printed 'mods is none, DirExists False', 'mods: NOT_FOUND', and 'walk: NOT_FOUND can't list mods: no mount of the Vfs has it'. VfsSource's comment (vfs.fb:13-14) says the root "is always a folder". FolderSource.KindAt("") and List("") give Null for a missing folder. Under() (line 349) counts only mounts strictly below, so a whole-Vfs Walk fails on an entry Entries itself listed.

## prefs-3 (low): CheckApp lets through app names Windows can't use: a trailing '.' or ' ', NUL and the other device names, and names over 255

Status: fixed in eb41438b

Where: std/io/preferences.fb:141

The docs and the panic message say an app name that can't be a file's on every OS panics. CheckApp only rejects '', '.', '..', control characters, and / \ : * ? " < > |. On Windows (11 here), several other names don't panic. "Game." is silently stored in "Game"'s folder, because Windows drops a trailing dot, so Preferences("Game.") and Preferences("Game") share one file on Windows but not on Linux or macOS. "Game " (trailing space), "nul", and a 300-character name all read fine, but Save throws a misleading NOT_FOUND on Windows, while Linux and macOS save the first two. Reserved names (CON, PRN, AUX, NUL, COM0-9, LPT0-9, also with an extension) can't be used on older Windows either.

```
Import std.core
Import std.io

Local temp = Try TempDir("fb-prefs-names-")
Local root = temp.Path
Local dotted = Try Preferences("Game.", root)
dotted.Set("who", "Game.")
Try dotted.Save()
Print("{(Try Preferences("Game", root)).Get("who") ?? "none"}")
Local long = ""
For i = 1 To 300
    long += "g"
End For
For app In ["Game ", "nul", long]
    Local p = Try Preferences(app, root)
    p.Set("who", app)
    Try
        p.Save()
        Print("[{app.Bytes.Count}] saved")
    Catch e As IOError
        Print("[{app.Bytes.Count}] {e.Kind}")
    End Try
End For
Try temp.Close()
```

Expected: A panic at the Preferences("Game.") line, as for 'a:b': "Preferences takes an app's name that can be a file's on every OS, not 'Game.'" (and the same for "Game ", "nul", and the 300-character name)

Actual: On Windows: "Game." (Game's preferences hold what "Game." saved), then [5] NOT_FOUND, [3] NOT_FOUND, [300] NOT_FOUND

Suggested fix: In CheckApp, also panic for: a name ending in '.' or ' '; Windows' reserved device names in any case, alone or before a '.' extension (CON, PRN, AUX, NUL, COM0-COM9, LPT0-LPT9, and the superscript-digit COM/LPT forms); and a name over 255 bytes. Add these to panic_io_preferences_app or a test like it, and say so in the doc comment.

Skeptic: I ran p3.fb on Windows 11. Preferences("Game.") saved, and Preferences("Game") then read who="Game." (one file, because Windows drops the trailing dot). "Game ", "nul", and a 300-byte name each gave NOT_FOUND at Save instead of a panic. The folder ended up holding only "Game" and "con.txt". CheckApp (preferences.fb lines 141-152) rejects only '', '.', '..', control bytes, and / \ : * ? " < > |. stdlib.md says "An app's name that can't be a file's on every OS panics", and so do the constructor's comment and the panic message, so names that can't be files on Windows (and names over NAME_MAX on any OS) break that contract. Lane decision 9's list is incomplete against stdlib.md's wording. Low because app names are constants the programmer picks.

## prefs-4 (low): On Windows, File.Metadata's times after 2038-01-19 wrap to 1901-1970

Status: fixed in a7ea9aae

Where: runtime/src/files.cpp:889

fb_io_request_stat builds Modified, Accessed, and Created from libuv's uv_stat_t, whose uv_timespec_t has a `long tv_sec`. On Windows, long is 32 bits, and libuv's own header says it's not 2038-proof (libuv issue 3864). So a file changed in 2040 reads as 1904. Now that these are std.time Timestamps, which cover far wider dates, the wrap is a wrong Timestamp, not a lost range. Separately, on Linux and macOS, `tv_sec * 1'000'000'000` overflows Int64 (undefined behavior) for times past 2262 or before 1677, which Timestamp can hold. I didn't show that here, since there's no non-Windows fb build. io_files.fb only checks that the times are after 2020.

```
// A file last changed in 2040 (here set through the C runtime's _utime64, as `touch -d` would).
Import std.core
Import std.io
Import std.time

@CLayout Struct Times
    Field Accessed: Int64
    Field Modified: Int64
End Struct

Extern "C"
    Function SetTimes(path: CString, ByRef times: Times): Int32 = "_utime64"
End Extern

Local temp = Try TempDir("fb-times-")
Local file = temp.Path / "late.txt"
Try File.WriteText(file, "from the future")
// 2040-06-01T00:00:00Z when it was changed, and 2000-01-01T00:00:00Z when it was read.
Local times = Times(946684800, 2222121600)
Print("{SetTimes(file.Text, ByRef times)}")
Local found = Try File.Metadata(file)
Print("{found.Modified} {found.Accessed}")
Try temp.Close()
```

Expected: 0
2040-06-01T00:00:00Z 2000-01-01T00:00:00Z

Actual: 0
1904-04-25T17:31:44Z 2000-01-01T00:00:00Z   (debug and release; 2222121600 - 2^32 = -2072932096)

Suggested fix: On Windows, get the times from the FILETIMEs themselves instead of from uv_stat_t: 100 ns ticks since 1601, from GetFileInformationByHandle or GetFileAttributesExW for the path, through links unless asked. Have the runtime give each time as seconds and nanoseconds, not packed Int64 nanoseconds, so StatOf can use Timestamp.FromUnixSeconds(seconds, nanoseconds). That also avoids the Int64 overflow past 2262 on POSIX. Add a Metadata test with a time after 2038.

Skeptic: I ran p4.fb (sets mtime with _utime64) in debug and release. Both printed "0" then "1904-04-25T17:31:44Z 2000-01-01T00:00:00Z". To rule out the setter, I also ran `touch -d 2040-06-01T00:00:00Z late.txt`: PowerShell's LastWriteTimeUtc showed 2040-06-01T00:00:00Z, but File.Metadata gave 1904-04-25T17:31:44Z. libuv's uv.h line 364 declares uv_timespec_t's tv_sec as `long`, 32 bits on Windows, and fb_io_request_stat (files.cpp around line 889) builds the Timestamps from it. So any time after 2038-01-19 wraps. I didn't check the POSIX overflow past 2262. Low because few files carry dates after 2038 today, but the Timestamp is silently wrong.

## prefs-5 (low): A preferences file saved with a UTF-8 BOM loses its first setting

Status: fixed in 7cc1dbbf

Where: std/io/preferences.fb:174

The file is meant to be text a person can read and change. Windows Notepad's 'UTF-8 with BOM' (the default in older Notepad) and other editors put EF BB BF at the start of the file. File.ReadLines keeps the BOM, and ValuesOf doesn't skip it. The first line's key becomes U+FEFF + the name, so Get("volume") gives Null, and the next Save writes both "﻿volume" and any new "volume". If the first line is the comment, the BOM also stops it being recognized as a comment, but it's skipped anyway because it has no '='.

```
// A preferences file a person saved from an editor as "UTF-8 with BOM", as Windows' Notepad
// offers: EF BB BF before its first line.
Import std.collections
Import std.core
Import std.io

Local temp = Try TempDir("fb-prefs-bom-")
Local root = temp.Path
Try Dir.CreateAll(root / "Game")
Local bytes: UInt8[] = [239, 187, 191]
bytes.AddAll("volume=0.8\nname=x\n".Bytes)
Try File.WriteBytes(root / "Game" / "preferences.txt", bytes)
Local p = Try Preferences("Game", root)
Print("{p.Get("volume") ?? "none"} {p.Contains("volume")} {p.Keys()[0].Bytes.Count}")
Try temp.Close()
```

Expected: 0.8 True 6

Actual: none False 9   (debug and release)

Suggested fix: In ValuesOf, drop a leading EF BB BF from the first line before checking for '#' and splitting. Say so in the class's doc comment, and add a BOM'd file to io_preferences.fb's 'a file a person changed' part.

Skeptic: I ran p5.fb (EF BB BF, then volume=0.8 and name=x), extended to Set volume=0.5 and Save, in debug and release. Both printed "none False 9", and the saved file held "﻿volume=0.8" (13 bytes) plus a new "volume=0.5", so the setting is lost and then duplicated. ValuesOf (preferences.fb line 174) never strips a BOM. File.ReadLines keeps one by design (file.fb line 110, stdlib.md), so it's up to Preferences. stdlib.md says the file is text a person can read and change, the test's hand-edited part already accepts CRLF from Windows editors, and the project ignores a leading BOM elsewhere (decisions.md line 29, spec 01-lexical). Low because modern Notepad saves without a BOM by default.

## prefs-6 (low): Keys' comment says 'in the order they were first set', but a removed key set again goes last

Status: fixed in 36eeae95

Where: std/io/preferences.fb:102

Keys gives Map's order, the order entries were added. A key that's removed and set again is added again, so it comes after keys set since its first Set. Read as written, the comment promises the first-Set order.

```
// Keys' comment says "in the order they were first set".
Import std.core
Import std.io

Local temp = Try TempDir("fb-prefs-order-")
Local p = Try Preferences("Order", temp.Path)
p.Set("a", "1")
p.Set("b", "2")
p.Remove("a")
p.Set("a", "3")
Print("{p.Keys()}")
Try temp.Close()
```

Expected: [a, b]   (as the comment reads)

Actual: [b, a]

Suggested fix: Reword the comment, for example: "The settings' names, in the order they were set, where one that's removed and set again goes last, and as the file has them after a read."

Skeptic: I ran p6.fb (Set a, Set b, Remove a, Set a), which printed "[b, a]". Keys' comment (preferences.fb line 102) says "in the order they were first set", which would give [a, b]. Keys returns Map's insertion order, so a key that's removed and set again goes last, and after a read the order is the file's. The comment is inaccurate; the behavior itself is fine.

## vfs-10 (low): EmbeddedSource.New's comment says any key with ".." panics, but such keys are normalized

Status: fixed in 7aec992c

Where: std/io/vfs.fb:77

The comment says New panics for a path "with a \"..\" in it". NamesOf resolves ".." and returns Null only when it goes above the root. So "levels/../one.txt" is accepted as "one.txt" (which is what decision 5 intends), and "levels" isn't a folder.

```
// EmbeddedSource's New says a path with a ".." in it panics.
Import std.collections
Import std.core
Import std.io

Local source = EmbeddedSource({"levels/../one.txt": "one".Bytes})
Print("{(Try source.KindAt("one.txt")) ?? FileKind.OTHER} {(Try source.KindAt("levels")) ?? FileKind.OTHER}")
```

Expected: Per the comment: a panic at <program>.fb:6:14. Per decision 5, what it prints is right and the comment is wrong.

Actual: FILE OTHER   (OTHER here stands for Null)

Suggested fix: Change the comment to say it panics for a path that goes above its root, or one with no names left once '.' and '..' are taken out, like "", ".", or "a/..".

Skeptic: Ran refute/v10.fb. It printed 'FILE OTHER': 'levels/../one.txt' is accepted as one.txt with no panic. The comment at vfs.fb:77-78 says New panics for a path "with a '..' in it", but NamesOf returns Null only when '..' goes above the root. Normalizing such keys is decision 5, so the comment is what's wrong.

## vfs-11 (low): A FolderSource whose folder is a file makes its mount point a file: Exists is True but ReadBytes throws NOT_FOUND

Status: fixed in f734aa7e, with vfs-4

Where: std/io/vfs.fb:47

VfsSource's contract says the root "" is always a folder. FolderSource.KindAt("") returns File.Metadata(Folder).Kind, which is FILE when the folder given is a file. Read("") always returns Null. So KindOf("notes") returns FILE and Exists returns True, but ReadText throws NOT_FOUND with the message "no mount of the Vfs has it".

```
// A FolderSource's folder that's a file: its root is a file, which Exists finds and ReadBytes doesn't.
Import std.collections
Import std.core
Import std.io

Local temp = Try TempDir("fb-vfs-rv-")
Local root = temp.Path
Try File.WriteText(root / "notes.txt", "notes")

Local vfs = Vfs()
vfs.MountFolder("notes", root / "notes.txt")
Print("KindOf {(Try vfs.KindOf("notes")) ?? FileKind.OTHER}, Exists {vfs.Exists("notes")}")
Try
    Print("read {Try vfs.ReadText("notes")}")
Catch e As IOError
    Print("read {e.Kind} {e.Message}")
End Try

Try temp.Close()
```

Expected: The root is a folder, as VfsSource says:
KindOf DIRECTORY, Exists False
read IS_A_DIRECTORY can't read notes: it's a folder

Actual: KindOf FILE, Exists True
read NOT_FOUND can't read notes: no mount of the Vfs has it

Suggested fix: Make FolderSource.KindAt("") always DIRECTORY and List("") give [] when Folder isn't a folder. That shares a fix with vfs-4.

Skeptic: Ran refute/v11.fb in debug and release. It printed 'KindOf FILE, Exists True' and then 'read NOT_FOUND can't read notes: no mount of the Vfs has it'. FolderSource.KindAt("") returns File.Metadata(Folder).Kind (vfs.fb:47), which is FILE for a file, but Read("") always gives Null (line 36). VfsSource's own contract says the root is always a folder, so the Vfs says a file is there and then can't read it. Same root cause as vfs-4.

## vfs-12 (low): FolderSource throws INVALID_PATH for a name the disk can't have, which hides the file a lower mount has

Status: fixed in b910b1c3

Where: std/io/vfs.fb:370

Missing() treats only NOT_FOUND, IS_A_DIRECTORY, and NOT_A_DIRECTORY as "no file here". A folder can't hold a file whose name the OS rejects: a NUL character, or on Linux a name over 255 bytes (ENAMETOOLONG, which the runtime reports as INVALID_PATH). For such a name, a folder mounted over files in memory throws INVALID_PATH instead of falling through to the memory mount. Entries still lists the file, but Exists is False and ReadText throws.

```
// A folder mounted over files in memory, read at a name no disk can have.
Import std.collections
Import std.core
Import std.io

Local temp = Try TempDir("fb-vfs-rv-")
Local root = temp.Path

Local name = "a\0b.txt"
Local vfs = Vfs()
vfs.MountEmbedded("", {name: "in memory".Bytes})
vfs.MountFolder("", root)
Print("listed {(Try vfs.Entries()).Count}, Exists {vfs.Exists(name)}")
Try
    Print("read [{Try vfs.ReadText(name)}]")
Catch e As IOError
    Print("read {e.Kind}")
End Try

Try temp.Close()
```

Expected: listed 1, Exists True
read [in memory]

Actual: listed 1, Exists False
read INVALID_PATH

Suggested fix: In FolderSource, treat INVALID_PATH as missing too (add it to Missing, or check it in FolderSource only), so the mounts below it are asked.

Skeptic: Ran refute/v12.fb in debug and release. It printed 'listed 1, Exists False' and 'read INVALID_PATH'. runtime/src/files.cpp:939-941 maps UV_EINVAL (a NUL in the path) and UV_ENAMETOOLONG to INVALID_PATH. Missing() (vfs.fb:370) doesn't count that kind, so the folder mount throws instead of falling through to the memory mount below. That breaks VfsSource's "what isn't there is Null". Names Windows rejects ('what?.txt', 'a*b.txt', 'a|b.txt', and a 301-character name; refute/v12b.fb) fall through fine on Windows, because they map to NOT_FOUND. The case left is a NUL, or a name over 255 bytes on Linux, such as a long non-ASCII name embedded from Windows. Low.

## vfs-13 (low): Entries, Walk, and Glob say "can't read" for a path above the root, and their comments don't mention INVALID_PATH

Status: fixed in 6b58b14b

Where: std/io/vfs.fb:363

Checked's message is always "can't read {path}: it goes above the Vfs's root", even when Entries, Walk, or Glob call it. Entries' own errors say "can't list". The comments of Entries, Walk, and Glob list NOT_FOUND and NOT_A_DIRECTORY but not INVALID_PATH, which they also throw.

```
// Every call's error for a path above a Vfs's root.
Import std.collections
Import std.core
Import std.io

Local vfs = Vfs()
vfs.MountEmbedded("", {"a.txt": "a".Bytes})
Try
    vfs.Entries("..")
Catch e As IOError
    Print("Entries: {e.Kind} {e.Message}")
End Try

Try
    vfs.Walk("x/../..")
Catch e As IOError
    Print("Walk: {e.Kind} {e.Message}")
End Try

Try
    vfs.Glob("..", "*")
Catch e As IOError
    Print("Glob: {e.Kind} {e.Message}")
End Try
```

Expected: Entries: INVALID_PATH can't list ..: it goes above the Vfs's root
Walk: INVALID_PATH can't list x/../..: it goes above the Vfs's root
Glob: INVALID_PATH can't list ..: it goes above the Vfs's root

Actual: Entries: INVALID_PATH can't read ..: it goes above the Vfs's root
Walk: INVALID_PATH can't read x/../..: it goes above the Vfs's root
Glob: INVALID_PATH can't read ..: it goes above the Vfs's root

Suggested fix: Pass the verb ("read" or "list") to Checked, and add INVALID_PATH to the comments of Entries, Walk, and Glob.

Skeptic: Ran refute/v13.fb. It printed 'Entries: INVALID_PATH can't read ..: it goes above the Vfs's root', and the same 'can't read' for Walk and Glob. Checked (vfs.fb:363) always says "read", while Entries' own errors say "can't list" and Dir's Checked takes a verb. The comments of Entries, Walk, and Glob (lines 251-253, 279-280, 287-288) don't mention INVALID_PATH, though ReadBytes' comment does. The wrong message and missing docs are minor.

## vfs-5 (low): KindOf and Entries give different kinds for a later mount's file at a place where an earlier mount point makes a folder

Status: fixed in 99fd7895

Where: std/io/vfs.fb:231

Decision 2 says a name two mounts have takes the last mount's kind. Listed follows that, so Entries("") lists "a" as FILE when the later mount has a file "a". KindOf instead returns DIRECTORY through Under() before it looks at any mount, whatever their order. As a result Exists("a") is False and DirExists("a") is True, yet ReadText("a") reads the file, and Walk lists a:FILE, so it never reaches a/x/y.txt, which KindOf calls a folder's file.

```
// A file in a later mount where an earlier mount's mount point has a folder.
Import std.collections
Import std.core
Import std.io

Local vfs = Vfs()
vfs.MountEmbedded("a/x", {"y.txt": "y".Bytes})
vfs.MountEmbedded("", {"a": "a file".Bytes})

For entry In Try vfs.Entries()
    Print("root has {entry.Name()} {entry.Kind}")
End For

Print("KindOf(a) {(Try vfs.KindOf("a")) ?? FileKind.OTHER}, Exists {vfs.Exists("a")}, DirExists {vfs.DirExists("a")}")
Print("ReadText(a) {Try vfs.ReadText("a")}")
Local walked: String[] = []
For entry In Try vfs.Walk()
    walked.Add("{entry.Path}:{entry.Kind}")
End For
Print("walk {walked}")
```

Expected: KindOf agrees with Entries, using the last mount's kind:
root has a FILE
KindOf(a) FILE, Exists True, DirExists False
ReadText(a) a file
walk [a:FILE]

Actual: root has a FILE
KindOf(a) DIRECTORY, Exists False, DirExists True
ReadText(a) a file
walk [a:FILE]

Suggested fix: Use Listed's rule in KindOf: go through the mounts last first, give DIRECTORY for a mount below `names` when it's reached in that order, and otherwise use the mount's KindAt. Add a test for this shape.

Skeptic: Ran refute/v5.fb in debug and release. It printed 'root has a FILE', 'KindOf(a) DIRECTORY, Exists False, DirExists True', 'ReadText(a) a file', 'walk [a:FILE]', and 'ReadText(a/x/y.txt) y'. KindOf returns DIRECTORY from Under() (line 231) before it checks any mount. Listed (line 322) keeps the later mount's FILE. So KindOf and Entries disagree, and Walk never reaches a file that can be read. Low because it takes a file and a mount point at the same name.

## vfs-6 (low): Vfs.Glob doesn't accept '\' between a pattern's names, though every Vfs path does

Status: fixed in 3fcefabe

Where: std/io/vfs.fb:290

Decision 3 and stdlib.md say Vfs paths take '/' or '\' between names on every OS, and ReadText("levels\\one.txt") works. Glob calls `PatternNames(pattern, False)`, so the pattern splits only at '/'. "levels\\*.txt" becomes one name and matches nothing. Dir.Glob on Windows does accept '\', so the same pattern finds files on the disk but not in a Vfs.

```
// A Vfs path takes '\' between names on every OS; so should its Glob patterns.
Import std.collections
Import std.core
Import std.io

Local vfs = Vfs()
vfs.MountEmbedded("", {"levels/one.txt": "one".Bytes, "levels/two.txt": "two".Bytes})
Print("read {Try vfs.ReadText("levels\\one.txt")}")
For pattern In ["levels/*.txt", "levels\\*.txt", "**/*.txt", "**\\*.txt"]
    Local found: String[] = []
    For entry In Try vfs.Glob("", pattern)
        found.Add(entry.Relative.Text)
    End For
    Print("[{pattern}] {found}")
End For
```

Expected: read one
[levels/*.txt] [levels/one.txt, levels/two.txt]
[levels\*.txt] [levels/one.txt, levels/two.txt]
[**/*.txt] [levels/one.txt, levels/two.txt]
[**\*.txt] [levels/one.txt, levels/two.txt]

Actual: read one
[levels/*.txt] [levels/one.txt, levels/two.txt]
[levels\*.txt] []
[**/*.txt] [levels/one.txt, levels/two.txt]
[**\*.txt] []

Suggested fix: Call `PatternNames(pattern, True)` in Vfs.Glob, say so in its comment, and add a test with a backslash pattern.

Skeptic: Ran refute/v6.fb in debug and release. ReadText("levels\\one.txt") read 'one', but the patterns 'levels\\*.txt' and '**\\*.txt' gave [] where their '/' forms found both files. Vfs.Glob calls PatternNames(pattern, False) (vfs.fb:290), so '\\' never splits a pattern. No Vfs name can hold '\\', because NamesOf splits at it, so such a pattern can never match. Its comment says patterns match "as Dir.Glob's patterns match", and Dir.Glob splits at '\\' on Windows (dir.fb:151), so on Windows the two differ.

## vfs-8 (low): EmbeddedSource accepts keys that make one path both a file and a folder, then gives contradictory answers instead of panicking

Status: fixed in 7aec992c

Where: std/io/vfs.fb:93

With the keys "a" and "a/b.txt", EmbeddedSource stores "a" in both Files and Folders. KindAt("a") returns DIRECTORY, so Exists("a") is False and DirExists("a") is True, yet ReadText("a") returns the file's bytes and Entries("a") lists b.txt. EmbedFolder never produces such keys, but a program's own map can. Std panics at the caller for other bad keys (decision 5), so it should panic here too.

```
// An EmbeddedSource whose keys make "a" a file and a folder too.
Import std.collections
Import std.core
Import std.io

Local vfs = Vfs()
vfs.MountEmbedded("", {"a": "a file".Bytes, "a/b.txt": "b".Bytes})
Print("KindOf {(Try vfs.KindOf("a")) ?? FileKind.OTHER}, Exists {vfs.Exists("a")}, DirExists {vfs.DirExists("a")}")
Print("ReadText [{Try vfs.ReadText("a")}], Entries {(Try vfs.Entries("a")).Count}")
```

Expected: panic: 'a' is both a file and a folder in an EmbeddedSource at <program>.fb:7:1

Actual: KindOf DIRECTORY, Exists False, DirExists True
ReadText [a file], Entries 1

Suggested fix: In New, panic when a file's path is a folder of another key, or another key's file is one of its folders. Add a panic test for it.

Skeptic: Ran refute/v8.fb in debug and release. It printed 'KindOf DIRECTORY, Exists False, DirExists True' and 'ReadText [a file], Entries 1'. EmbeddedSource.New (vfs.fb:93-96) stores 'a' both in Files and as a folder. KindAt then says DIRECTORY while Read gives a file's bytes, which contradicts VfsSource.Read's "Null when there's no file there". The source answers inconsistently for input no disk could produce, where std panics at the caller for other bad keys.

## Refuted by the skeptics (no action)

- vfs-7: FolderSource follows Windows' name rules, so the same Vfs path gives different results from a folder and from memory, and on Windows and Linux. refute/v7.fb printed 'True True True True' for the folder, 'False False False False' for memory, and 'nul: []' on Windows, as the finding says. That's the disk's own name rules, which File.* follows too. stdlib.md says FolderSource "reads a folder on the disk as it's asked", and nothing promises a folder mount is case-sensitive or rejects Windows' device names. The done-when (the same file from a folder and from memory) passes for the file's own path. I checked whether trimmed names can escape: refute/v7b.fb read 'data/.. /secret.txt', 'data/.../secret.txt', and similar, and all gave NOT_FOUND. '.. ' only aliases the mounted folder. A design choice, not a contradiction.
- vfs-9: EmbeddedSource silently keeps one of several keys that normalize to the same path. refute/v9.fb printed 'third 1'. Map is insertion-ordered (map.fb:1), so the last of several keys for one normalized path wins every time, as a later assignment to a Map key does. Each key is a valid file path, so decision 5's panic for a key that isn't one doesn't cover it. The result is consistent: one file, one entry. No doc says duplicates must panic. It's a preference, not a bug.
- vfs-14: io_vfs.fb doesn't test a folder mount's error kinds, an Async read falling through from a folder to a lower mount, INVALID_PATH from listings and async reads, or Glob over a folder. refute/v14.fb printed every line the finding expects on Windows: IS_A_DIRECTORY, NOT_A_DIRECTORY, async IS_A_DIRECTORY, 'async from below: [in memory]', and 'glob [levels/one.txt, readme.txt]'. No wrong behavior shows, and the finding says as much. This is a coverage gap, not a defect. The error kinds come from the runtime's shared libuv mapping (files.cpp), which File.* uses as well. The tests that the confirmed fixes need (vfs-2 and vfs-4 among them) will cover the async fall-through and a folder mount's errors anyway.
