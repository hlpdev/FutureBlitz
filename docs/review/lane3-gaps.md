# The review of M6 step 32's audit gaps in std.text and std.compress: StringBuilder as a Writer, Humanize.Relative of a Duration, FormatBytes and FormatDuration, every codec's Compressor and Decompressor as a Coder with CoderWriter and CoderReader over std.io's streams, and Zip and Tar entries' Modified and Mode

5 confirmed findings: 0 high, 2 medium, 3 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## compress-1 (medium): CoderReader gives bytes it already gave again after its Coder throws

Status: fixed in 0b3e25c (a coderreader whose coder throws doesn't give what it gave again), with compress_streams cases: each format's stream cut 6 bytes short, read five times, gives its first bytes and then UNEXPECTED_END each time, and Gzip's past a limit of 100000 gives 65506 bytes and then OTHER each time, where before it gave the first bytes again; the Coder's bytes replace Coded, and At goes back to 0, only once the Coder gives them

Where: std/compress/stream.fb:72

CoderReader.Read sets At = 0 before it calls Through.Add or Through.Finish. When either call throws (a limit, broken data, or data that ends too soon), Coded still holds the previous chunk's output, which was already handed out, and At now points back at its start. The next Read skips the refill loop because At != Coded.Count and copies those old bytes out again. A caller that catches the IOError and keeps reading gets duplicated data instead of the error. With a truncated stream the reader cycles forever: stale bytes, UNEXPECTED_END, stale bytes, and so on. All six formats do this (I checked Deflate, Zlib, Gzip, Lz4, and XZ with a truncated stream; see the second program in the actual field).

```
// A CoderReader whose Coder throws, read again: it throws again, or ends, but doesn't give the
// bytes it gave before a second time.
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.compress

// Bytes that don't compress, so each 64 KiB chunk of the gzip is about 64 KiB of them.
Local data: UInt8[] = []
Local state: UInt32 = 12345
For i = 1 To 200000
    state = state.WrappingMultiply(1664525).WrappingAdd(1013904223)
    data.Add((state >> 24) As UInt8)
End For

Local reader = CoderReader(MemoryStream(Gzip.Compress(data)), GzipDecompressor(100000))
Local into = Buffer<UInt8>(1000000)
Local first: UInt8[] = []
For i = 1 To 3
    Try
        Local bytes = into.Slice(0, Try reader.Read(into)).ToArray()
        Print("read", bytes.Count, "bytes, the same as the first Read's:", bytes == first)
        If i == 1 Then first = bytes
    Catch e As IOError
        Print(e.Kind, e.Message)
    End Try
End For
```

Expected: read 65506 bytes, the same as the first Read's: False
OTHER the Gzip data comes to more than 100000 bytes
OTHER the Gzip data comes to more than 100000 bytes   (or a Read of 0; anything except the first chunk's bytes again)

Actual: read 65506 bytes, the same as the first Read's: False
OTHER the Gzip data comes to more than 100000 bytes
read 65506 bytes, the same as the first Read's: True
(Debug and release print the same.) A Gzip.Compress(...) cut 6 bytes short and read 5 times through CoderReader(..., GzipDecompressor()) gives [3300, UNEXPECTED_END, 3300, UNEXPECTED_END, 3300], and Deflate, Zlib, Lz4, and XZ give the same pattern.

Suggested fix: Reset At only after the Coder succeeds: `Local coded = Try Through.Add(...)` (or Finish), then `Coded = coded` and `At = 0`. Alternatively, clear Coded (Coded = [], At = 0) before calling the Coder. Add a compress_streams case that catches the error and reads again.

Skeptic: Ran the finding's program, plus a truncated-gzip case (I copied the bytes in a loop because UInt8[] has no Slice). Debug and release both printed: read 65506 (False), OTHER 'the Gzip data comes to more than 100000 bytes', then read 65506 'the same as the first Read's: True'. The truncated stream gave read 3300 / UNEXPECTED_END / read 3300 / UNEXPECTED_END / read 3300. Cause: std/compress/stream.fb:72 sets `At = 0` before `Coded = Try Through.Add(...)` / `Try Through.Finish()` (lines 74, 77). When the Coder throws, Coded keeps the chunk that was already handed out, and the next Read copies it out again.

## compress-3 (medium): A GNU-format tar's pre-1970 mtime (negative base-256) reads as Modified Null

Status: fixed in 128768a (a gnu tar's time before 1970, a negative base-256 number, is its entry's), with compress_entry_times cases of a tar whose header's time is GNU's negative base-256: -1000000000 is 1938-04-24T22:13:20Z and all 0xff a second before 1970, one an Int64 can't hold is none, and a positive one still reads; signed_number_of() reads the header's time, and stdlib.md and open()'s comment say a header's time can be before 1970

Where: runtime/src/tar.cpp:73

number_of accepts only positive base-256 numbers (first byte 0x80) and gives nothing for 0xff, which is how GNU tar and Python's tarfile (GNU_FORMAT) write a negative number. The new code reads an entry's time from number_of(header, time_field), so any entry in a GNU-format tar dated before 1970 gets Modified = Null, even though the archive states its time. docs/stdlib.md says tar reads GNU's base-256 numbers, and the runtime comment claims a header's time 'can be neither' before 1970 nor fractional, which is not true for GNU tars. I checked the bytes in the program against Python's tarfile: GNU_FORMAT with mtime -1000000000 writes exactly ff ff ff ff ff ff ff ff c4 65 36 00 at offset 136.

```
// A GNU tar's entry last changed before 1970, whose header's mtime is GNU's base-256, negative,
// as GNU tar and Python's tarfile write one: 0xff, then -1000000000 in two's complement, which is
// 1938-04-24T22:13:20Z.
Import std.core
Import std.collections
Import std.time
Import std.compress

Local writer = TarWriter()
writer.Add("a.txt", "a".Bytes)
Local bytes = writer.Finish()
Local field: UInt8[] = [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc4, 0x65, 0x36, 0x00]
For i = 0 Until 12
    bytes[136 + i] = field[i]
End For

// The checksum again: each byte's sum, with the checksum's own as spaces.
Local sum = 0
For i = 0 Until 512
    sum += i >= 148 && i < 156 ? 32 : bytes[i] As Int
End For

Local digits = "{sum:06o}".Bytes
For i = 0 Until 6
    bytes[148 + i] = digits[i]
End For
bytes[154] = 0
bytes[155] = 32

Local entry = (Try Tar.Read(bytes)).Entries[0]
Print(entry.Name, entry.Modified?.ToString() ?? "-")
```

Expected: a.txt 1938-04-24T22:13:20Z

Actual: a.txt -

Suggested fix: Give the time field its own reader, or a signed variant of number_of, that takes a 0xff-led base-256 field as a two's-complement negative number (sign-extend the bytes after the first). Fix the comment in open() that says a header's time can't be before 1970, and add a GNU-format pre-1970 entry to compress_archives (make_archives.py can write one with tarfile.GNU_FORMAT).

Skeptic: Ran the program and it printed 'a.txt -' instead of 1938-04-24T22:13:20Z. runtime/src/tar.cpp:72-73: number_of accepts base-256 only when the first byte is 0x80 and rejects 0xff (GNU's negative form). Line 455 then reads the header time with number_of(header, time_field), so a pre-1970 GNU-format time becomes Null. docs/stdlib.md says tar reads GNU's base-256 numbers, and the comment at tar.cpp:451 wrongly says a header time can't be before 1970.

## compress-2 (low): Zip DOS dates go through local time, so an hour skipped for summer time reads and writes an hour early, depending on the machine's zone

Status: fixed in b0efe68 (a zip's dos dates are read and written as utc without local time, in an hour summer time skips too), with a runtime test in EST5EDT of 2107-03-13T02:30:00Z, which only a DOS date holds, in the hour US summer time skips: a zip made in UTC0 reads back right in EST5EDT, and one made in EST5EDT is the same bytes and reads back right too, where each was an hour early; the reader takes the DOS date from the central directory record, and the writer patches each entry's, made from UTC, into the finished zip, so no local time is involved

Where: runtime/src/zip.cpp:561

The docs say a zip entry's DOS date is read as UTC and that the bytes are the same everywhere. The reader, though, takes minizip-ng's time_t (which minizip-ng made with mktime in local time) and turns it back into a DOS date with mz_zip_time_t_to_dos_date (localtime). For a wall-clock time inside the machine's spring-forward gap, mktime normalizes it, so the round trip gives a different DOS date. On this machine (Eastern, US rules) a foreign zip dated 2026-03-08 02:30 with no extra-field time reads as 01:30Z. Under TZ=JST-9 or UTC0 the same bytes read as 02:30Z. dos_date_of has the same problem on the writer side: a ZipWriter time past 2106, which only a DOS date holds, in that hour reads back as 01:30. The runtime comment mentions the writer's hour, but docs/stdlib.md doesn't, and nothing documents the reader's dependence on the zone. Most zips from Windows Explorer, Java, and similar tools carry only DOS dates.

```
// A zip whose entry has only a DOS date, 2026-03-08 02:30:00, inside the hour US summer time
// skips: read as UTC, its time is that, whatever the machine's zone.
Import std.core
Import std.collections
Import std.time
Import std.compress

Local writer = ZipWriter()
writer.Add("a.txt", "a".Bytes, ZipMethod.STORE)
Local bytes = writer.Finish()
// 02:30:00 is 0x13c0, and 2026-03-08 is 0x5c68, in the local header and the central directory.
For i = 0 Until bytes.Count - 4
    If bytes[i] == 0x50 && bytes[i + 1] == 0x4b && bytes[i + 2] == 3 && bytes[i + 3] == 4
        bytes[i + 10] = 0xc0
        bytes[i + 11] = 0x13
        bytes[i + 12] = 0x68
        bytes[i + 13] = 0x5c
    ElseIf bytes[i] == 0x50 && bytes[i + 1] == 0x4b && bytes[i + 2] == 1 && bytes[i + 3] == 2
        bytes[i + 12] = 0xc0
        bytes[i + 13] = 0x13
        bytes[i + 14] = 0x68
        bytes[i + 15] = 0x5c
    End If
End For

Local entry = (Try Zip.Read(bytes)).Entries[0]
Print(entry.Modified?.ToString() ?? "-")

// And a ZipWriter's time past 2106, which only a DOS date holds, in the hour skipped in 2107.
Local later = ZipWriter()
later.Add("b.txt", "b".Bytes, modified: Timestamp.Parse("2107-03-13T02:30:00Z"))
Print((Try Zip.Read(later.Finish())).Entries[0].Modified?.ToString() ?? "-")
```

Expected: 2026-03-08T02:30:00Z
2107-03-13T02:30:00Z
(in every time zone)

Actual: 2026-03-08T01:30:00Z
2107-03-13T01:30:00Z
(this machine's Eastern zone, and also under TZ=PST8PDT or TZ=EST5EDT; TZ=JST-9 and TZ=UTC0 print the expected output)

Suggested fix: Reader: read the raw DOS time and date from the entry's central-directory record (time at offset 12 and date at 14 from entry.position) and pass them to dos_time, instead of round-tripping through mz_zip_time_t_to_dos_date. Writer: either find a time_t whose localtime gives the wanted fields (adjust and retry when mktime normalizes them), or patch the DOS fields in the finished bytes. Add a test that runs in a zone with summer time, or document the hour in docs/stdlib.md if it stays.

Skeptic: Ran the program. On this machine (Eastern) it printed 2026-03-08T01:30:00Z and 2107-03-13T01:30:00Z. Under TZ=UTC0 it printed 02:30:00Z for both. docs/stdlib.md:1002-1003 says a DOS date is 'read as UTC, as nothing says its zone'. runtime/src/zip.cpp:556-561 instead goes through minizip-ng's mktime'd time_t and back with mz_zip_time_t_to_dos_date, so a time in the skipped spring-forward hour depends on the machine's zone. Real, but it only hits one hour a year on DOS-date-only zips, so low.

## compress-4 (low): No test checks NTFS or PKWARE Unix extra-field times, a DOS-date-only foreign zip's time, or a non-Unix zip's Mode Null

Status: fixed in dea3f5f (compress_entry_times reads zips made by hand with ntfs's, info-zip's, and pkware's times, and one with none), with the finding's zips made by hand in compress_entry_times: NTFS's time to 100 ns, Info-ZIP's extended timestamp, PKWARE's Unix field, NTFS's taking precedence over UT, and an MS-DOS zip with no extra field, read from its DOS date as UTC, with Mode Null; the code was right, so the cases pass with it as it was

Where: tests/programs/compress_tools.fb:40

The new extra_time code parses three fields: NTFS 0x000a, Info-ZIP UT 0x5455, and PKWARE Unix 0x000d, with NTFS taking precedence. The only foreign zip whose time a test prints is bsdtar's sample.zip, which has just 0x7875 and 0x5455 and was made by Unix. ZipWriter's own zips carry UT or the fixed 1980 date. So the NTFS parse (100 ns precision, the 1601 epoch), the PKWARE Unix field, the precedence order, the DOS-date fallback on a foreign zip, and the documented 'a zip made on Windows has no mode' (Mode Null for a non-Unix host) all go untested. compress_archives' Python-made zips (cp437.zip and others) exist but their Modified and Mode aren't printed. They are correct today (the program below checks them by hand), but the step's done-when asks for them to be tested.

```
// Zips made by hand, each of one stored entry with an extra field, which says when it was last
// changed in UTC: NTFS's, Info-ZIP's extended timestamp, and PKWARE's Unix field.
Import std.core
Import std.collections
Import std.time
Import std.crypto
Import std.compress

Function Put(into: UInt8[], value: UInt64, size: Int)
    For i = 0 Until size
        into.Add(((value >> (8 * i)) & 0xff) As UInt8)
    End For
End Function

// A zip of a.txt, made by `host`, with `extra`, external attributes `attributes`, and a DOS date of
// 1980-01-01 00:00.
Function Zipped(extra: UInt8[], host: Int, attributes: UInt64): UInt8[]
    Local data = "a".Bytes
    Local crc = Crc32.Hash(data) As UInt64
    Local name = "a.txt".Bytes
    Local zip: UInt8[] = []
    Put(zip, 0x04034b50, 4)
    Put(zip, 20, 2)
    Put(zip, 0, 2)
    Put(zip, 0, 2)
    Put(zip, 0, 2)
    Put(zip, 0x21, 2)
    Put(zip, crc, 4)
    Put(zip, 1, 4)
    Put(zip, 1, 4)
    Put(zip, name.Count As UInt64, 2)
    Put(zip, extra.Count As UInt64, 2)
    zip.AddAll(name)
    zip.AddAll(extra)
    zip.AddAll(data)
    Local central = zip.Count
    Put(zip, 0x02014b50, 4)
    Put(zip, ((host As UInt64) << 8) | 20, 2)
    Put(zip, 20, 2)
    Put(zip, 0, 2)
    Put(zip, 0, 2)
    Put(zip, 0, 2)
    Put(zip, 0x21, 2)
    Put(zip, crc, 4)
    Put(zip, 1, 4)
    Put(zip, 1, 4)
    Put(zip, name.Count As UInt64, 2)
    Put(zip, extra.Count As UInt64, 2)
    Put(zip, 0, 2)
    Put(zip, 0, 2)
    Put(zip, 0, 2)
    Put(zip, attributes, 4)
    Put(zip, 0, 4)
    zip.AddAll(name)
    zip.AddAll(extra)
    Local size = zip.Count - central
    Put(zip, 0x06054b50, 4)
    Put(zip, 0, 2)
    Put(zip, 0, 2)
    Put(zip, 1, 2)
    Put(zip, 1, 2)
    Put(zip, size As UInt64, 4)
    Put(zip, central As UInt64, 4)
    Put(zip, 0, 2)
    Return zip
End Function

Function Show(label: String, zip: UInt8[])
    Try
        Local entry = (Try Zip.Read(zip)).Entries[0]
        Local mode = entry.Mode
        Print(label, entry.Modified?.ToString() ?? "-", mode == Null ? "-" : "0o{mode:o}")
    Catch e As IOError
        Print(label, e.Kind, e.Message)
    End Try
End Function

Local ntfs: UInt8[] = []
Put(ntfs, 0x000a, 2)
Put(ntfs, 32, 2)
Put(ntfs, 0, 4)
Put(ntfs, 1, 2)
Put(ntfs, 24, 2)
Put(ntfs, 134116992001234567, 8)
Put(ntfs, 0, 8)
Put(ntfs, 0, 8)

Local extended: UInt8[] = []
Put(extended, 0x5455, 2)
Put(extended, 5, 2)
Put(extended, 1, 1)
Put(extended, 1767225600, 4)

Local unix: UInt8[] = []
Put(unix, 0x000d, 2)
Put(unix, 12, 2)
Put(unix, 0, 4)
Put(unix, 1767225660, 4)
Put(unix, 0, 4)

Local both: UInt8[] = []
both.AddAll(extended)
both.AddAll(ntfs)

Show("ntfs", Zipped(ntfs, 3, 0o100755 << 16))
Show("extended", Zipped(extended, 3, 0o100600 << 16))
Show("unix", Zipped(unix, 3, 0o120777 << 16))
Show("both", Zipped(both, 0, 0x20))
Show("none", Zipped([], 0, 0x20))
```

Expected: ntfs 2026-01-01T00:00:00.1234567Z 0o755
extended 2026-01-01T00:00:00Z 0o600
unix 2026-01-01T00:01:00Z 0o777
both 2026-01-01T00:00:00.1234567Z -
none 1980-01-01T00:00:00Z -
(and a test in tests/programs that checks this)

Actual: It prints the expected lines, but no program under tests/ covers any of these cases: grep finds no NTFS, 0x000a, or 0x000d use, and compress_archives' ZipList doesn't print Modified or Mode.

Suggested fix: Add these handmade zips, or ones from make_archives.py (Python zipfile with create_system = 0, and extra fields added by hand), to compress_entry_times.fb, printing Modified and Mode. Also print Modified and Mode for compress_archives' cp437.zip and link.zip.

Skeptic: Ran the handmade-zip program. It prints the expected lines, so the code is right today: ntfs .1234567Z 0o755, extended, unix 00:01:00Z 0o777, both prefers NTFS with Mode '-', none 1980 '-'. But grep for ntfs/NTFS/0x000a/0x000d under tests/ finds nothing. compress_archives.fb doesn't print Modified or Mode. compress_tools.out only covers bsdtar's Unix-made sample.zip (UT field). So the NTFS and PKWARE Unix parses, their precedence, the DOS-date fallback on a foreign zip, and a non-Unix zip's Mode Null are untested. The step-32 row's done-when asks for every module to be 'documented and tested'.

## text-1 (low): StringBuilder.Flush panics past the cap instead of throwing IOError, and no test checks the cap

Status: fixed in 9a68656 (a stringbuilder's flush throws past a string's most bytes, as its write does, and keeps what it holds), with release_text_builder_cap, built in release for the 2 GiB it fills: a Write past the cap throws and adds nothing, a Flush whose U+FFFD would pass it throws OTHER and keeps the unfinished character, and the rest of it then makes a 3-byte character, not two U+FFFDs, where Flush panicked inside std; Write and Flush share CheckRoomFor()

Where: std/text/builder.fb:136

Write throws IOError OTHER past the most a String holds and adds nothing, as the lane intended. But Flush, which the Writer interface declares Throws IOError, calls Settle. Settle uses CheckRoom, and CheckRoom panics. So a builder within 3 bytes of 2147483647 that holds a pending lead byte panics in Flush (std.io's Copy calls it too) where Write would have thrown. The panic is reported at builder.fb:137, inside std, because Flush isn't @PanicsAtCaller. Neither the cap throw nor this case has a test in tests/programs/text_builder_writer.fb. The other paths were checked and are right: the fuzz of 300655 three-way splits of bad and partial UTF-8, after Flush, matched StringWriter's text exactly. Humanize's Relative(Duration), FormatBytes, and FormatDuration were also right at every unit edge, at Int64's and Duration's ends, and in debug and release.

```
Import std.core
Import std.io
Import std.text

Local builder = StringBuilder()
builder.Reserve(2147483647)
Local chunk = "x".Repeated(1048576)
For i = 1 To 2047
    builder.Append(chunk)
End For
builder.Append("x".Repeated(1048574))
Print(builder.Length)

Try
    builder.WriteBytes([0x41, 0x42])
Catch e As IOError
    Print("Write past the cap:", e.Kind, e.Message)
End Try

Try
    builder.WriteBytes([0xE2])
    Print("pending written", builder.Length)
    builder.Flush()
    Print("flushed", builder.Length)
Catch e As IOError
    Print("Flush past the cap:", e.Kind, e.Message)
End Try
```

Expected: 2147483646
Write past the cap: OTHER a StringBuilder holds up to 2147483647 bytes, not 2147483648
pending written 2147483646
Flush past the cap: OTHER a StringBuilder holds up to 2147483647 bytes, not 2147483649

Actual: 2147483646
Write past the cap: OTHER a StringBuilder holds up to 2147483647 bytes, not 2147483648
pending written 2147483646
panic: a String holds at most 2147483647 bytes at std/text/builder.fb:137:9
(exit code 101)

Suggested fix: In Flush, check the room for Pending's text before calling Settle (fb_string_from_array(Pending).Length against 2147483647 - Bytes.Count). Past the cap, throw IOError(IOErrorKind.OTHER, ...) as Write does, and keep Pending. Add the cap cases to text_builder_writer.fb or a new release-only test: Write past the cap throws and adds nothing, and Flush with a pending byte at the cap throws.

Skeptic: I ran C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-gaps/text1/cap.fb (the finding's program, unchanged) in debug and in release. Both builds printed the same thing: '2147483646', 'Write past the cap: OTHER a StringBuilder holds up to 2147483647 bytes, not 2147483648', 'pending written 2147483646', then 'panic: a String holds at most 2147483647 bytes at std/text/builder.fb:137:9', exit 101. The code agrees with this. In std/text/builder.fb, Write (lines 112-133) checks only the finished text against the cap, so it keeps the lone 0xE2 in Pending without throwing. Flush (136-138) calls Settle (187-193), which turns Pending into U+FFFD (3 bytes), and then CheckRoom (197-199) calls Panic. Writer.Flush is declared 'Throws IOError' (std/io/streams.fb:37), and Write's own doc comment says the cap gives an IOError, so a Writer user such as Copy gets a panic where Write would have thrown. Flush has no @PanicsAtCaller, so the panic is reported inside std. tests/programs/text_builder_writer.fb has no case for the cap (a grep for 2147483647 or 'cap' finds nothing). I rate it low rather than medium because it only happens when a pending partial character meets a builder within 3 bytes of 2 GiB. It still contradicts the Writer contract and the lane's own cap behavior.
