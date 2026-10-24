# The review of M6 step 13, std.io's streams: Async Methods in Interfaces (required, default, Static), Reader, Writer, and Seeker with Async twins, buffered, memory, and string streams, BinaryReader and BinaryWriter, and Copy, ReadAll, and ReadExact (it sits on step 12; review only step 13's change)

13 confirmed findings: 0 high, 3 medium, 10 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S13-async-1 (medium): 'Async Static Method' or 'Async Final Method' after a required Interface Method is parsed as that Method's body

Status: open

Where: compiler/src/parser.cpp:1135

next_line_is_body() treats a line as a new member only when Async comes right before Method. Modifiers can come in any order (parse_modified says so, and 'Async Static Method' parses fine in a Struct). So a required Method followed by 'Async Static Method', 'Async Final Method' or 'Async ByRef Method' takes that line as the start of its body. The Interface then breaks with errors that point somewhere else. 'Static Async' and 'Final Async' work only because KW_STATIC and KW_FINAL are already in the not-a-body list. The parser test only covers 'Async Method' directly.

```
Import std.core

Interface Maker
    Method Size(): Int
    Async Static Method MakeAsync(): Int
End Interface

Struct Seven Implements Maker
    Field unused: Int

    Method Size(): Int => 3

    Async Static Method MakeAsync(): Int
        Await NextFrame()
        Return 7
    End Method
End Struct

Async Function Made<M: Maker>(): Int => Await M.MakeAsync()

Print("{Await Made<Seven>()}")
```

Expected: 7 (the same program with 'Static Async Method' in the Interface prints 7)

Actual: f1.fb:4:12: error: 'Size' doesn't return a value on every path
f1.fb:5:25: error: Methods can only be declared in a Struct or Class
f1.fb:6:1: error: expected 'End Method', found 'End Interface'
f1.fb:19:49: error: M has no member named 'MakeAsync'
error: couldn't build f1.fb because of 4 errors

Suggested fix: In the KW_ASYNC case, look past the other modifiers, as parse_modified does: return !modifiers_lead_to(after, token_kind_t::KW_METHOD), and also treat 'ByRef Method' after them as a member, so sema can report 'a ByRef Method can't be Async'. Add parser tests for 'Async Static Method' and 'Async Final Method' after a required Method.

Skeptic: Ran f1.fb (an Interface with 'Method Size(): Int' then 'Async Static Method MakeAsync(): Int') in debug. It failed with exactly the reported errors: "'Size' doesn't return a value on every path", "Methods can only be declared in a Struct or Class" at 5:25, "expected 'End Method', found 'End Interface'", and "M has no member named 'MakeAsync'". With 'Static Async Method' in the Interface only (f1e.fb), it prints 7. 'Async Final Method TwiceAsync(): Int => 2 * Size()' after a required Method (f1c.fb) fails the same way, and 'Final Async Method' (f1d.fb) prints 10. The cause is in compiler/src/parser.cpp next_line_is_body(), around line 1135: the new KW_ASYNC case counts the line as a member only when the very next token is KW_METHOD (`m_tokens[after].kind != token_kind_t::KW_METHOD`). It doesn't skip modifiers the way modifiers_lead_to() and parse_modified() do, which take modifiers in any order (spec 06:453 also says modifiers go in any order). The new parser test covers only 'Async Method'.

## S13-buffered-binary-1 (medium): BufferedReader gives old bytes again after the wrapped Reader's read throws

Status: open

Where: std/io/buffered.fb:79

Fill (and FillAsync, line 84) sets Start = 0 before it calls Inner.Read(Held), and only sets Finish if that read returns. When the read throws, for example an INTERRUPTED IOError that a caller retries, Finish keeps the old buffer's end. The next ReadByte, Read, ReadAsync, ReadLine or ReadLineAsync then sees Start < Finish and gives the bytes it already gave, as if they were new. ReadLine shows the same thing: with "x\nab", then an error, then "cd\n", it gives [x] (INTERRUPTED) [x] [abcd], so the line x comes back a second time. The behavior is the same in debug and release, and through the Async path.

```
Import std.core
Import std.ffi
Import std.io

// Gives "abcd", then is interrupted once, then gives "efgh", then ends.
Class Flaky Implements Reader
    Field Calls: Int

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Calls += 1
        If Calls == 2 Then Throw IOError(IOErrorKind.INTERRUPTED, "interrupted")
        Local bytes = (Calls == 1 ? "abcd" : (Calls == 3 ? "efgh" : "")).Bytes
        For i = 0 Until bytes.Count
            into[i] = bytes[i]
        End For
        Return bytes.Count
    End Method
End Class

// Each byte, read one at a time, and each error's kind, retrying after it.
Local reader = BufferedReader(Flaky(), 4)
Local got = ""
Loop
    Try
        Local b = Try reader.ReadByte()
        If b == Null Then Break
        got += "{b} "
    Catch e As IOError
        got += "({e.Kind}) "
    End Try
End Loop
Print(got)

// The same through ReadAsync.
Local later = BufferedReader(Flaky(), 4)
Local one = Buffer<UInt8>(1)
Local gotAsync = ""
Loop
    Try
        Local n = Try Await later.ReadAsync(one)
        If n == 0 Then Break
        gotAsync += "{one[0]} "
    Catch e As IOError
        gotAsync += "({e.Kind}) "
    End Try
End Loop
Print(gotAsync)
```

Expected: 97 98 99 100 (INTERRUPTED) 101 102 103 104 
97 98 99 100 (INTERRUPTED) 101 102 103 104 

Actual: 97 98 99 100 (INTERRUPTED) 97 98 99 100 101 102 103 104 
97 98 99 100 (INTERRUPTED) 97 98 99 100 101 102 103 104 

Suggested fix: In Fill and FillAsync, empty the buffer before reading: `Start = 0`, `Finish = 0`, then `Finish = Try Inner.Read(Held)` (and the same with Await). A test should use a Reader that throws once, then retry with ReadByte, Read, and ReadLine. Separately, ReadLine drops the part of a line it had already taken when a Fill throws, because that part is in a Local. Keeping it in a Field would let a retry finish the line.

Skeptic: I ran the finding's program (f1.fb) in debug and release. Both builds printed `97 98 99 100 (INTERRUPTED) 97 98 99 100 101 102 103 104`, for ReadByte and for ReadAsync alike, so abcd came back a second time after the error. I also ran a ReadLine version (f1b.fb: "x\nab", then an error, then "cd\n"). In debug and release it printed `[x] (INTERRUPTED) [x] [abcd]`, so the line x is given twice. The cause is in std/io/buffered.fb lines 79-87: Fill and FillAsync set `Start = 0` before `Finish = Try Inner.Read(Held)`. When that read throws, Finish keeps the old end and Start < Finish again. This also contradicts Fill's own comment, which says Held 'has nothing left'. A retry after an IOError silently gives stale bytes as new ones, so medium is right.

## S13-streams-1 (medium): ReadAll, ReadAllAsync, and ReadAllText take quadratic time: Add reserves exactly one more chunk on every read

Status: open

Where: std/io/streams.fb:167

The Add helper that ReadAll, ReadAllAsync, and (through ReadAll) ReadAllText share calls all.Reserve((all.Count + count) As Int) on every chunk. fb_array_reserve (runtime/src/array.cpp) sets capacity to exactly the amount asked for, so every 64 KiB chunk reallocates and copies everything read so far. Reading n bytes therefore costs O(n^2 / 65536). I timed release builds through a Reader that only hands out counts: ReadAll of 16 MB took 0.46 s, 64 MB took 5.2 s, and 256 MB took 74 s. ReadAllAsync of 64 MB took 5.1 s. The same loop without the Reserve call (array Add doubles its capacity) read 256 MB in 2.2 s. Copy into a MemoryStream does not have this problem (256 MB in 1.9 s), because Resize grows through Add.

```
Import std.core
Import std.ffi
Import std.io

// Gives `count` bytes as fast as a Reader can, without memory of its own.
Class Zeros Implements Reader
    Field Left: Int64

    Public New(count: Int64)
        Left = count
    End New

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local count = Min(into.Count, Left)
        Left -= count
        Return count
    End Method
End Class

// Time with: fb build --release this.fb, then time ./this.exe
Local all = Try ReadAll(Zeros(256 * 1048576))
Print("256 MB: {all.Count}")
```

Expected: Prints "256 MB: 268435456" in about 2 s in release, which is what the same loop takes without the per-chunk exact Reserve.

Actual: Prints "256 MB: 268435456" after 74 s in release. 64 MB takes 5.2 s and 16 MB takes 0.46 s, so the time grows quadratically.

Suggested fix: Remove the exact Reserve from Add and let all.Add's doubling grow the array, or reserve geometrically, for example Max(all.Count * 2, all.Count + count) capped at 2147483647. A bulk copy (Resize, then one Mem.Copy from chunk.Pointer()) would also replace the per-byte Add loop.

Skeptic: I built the reviewer's Zeros program in release with fb build --release and timed it. ReadAll took 1.2 s for 16 MB, 8.0 s for 64 MB and 23.7 s for 128 MB, so time grows roughly with the square of the size. The same loop without the Reserve line (my ReadAllPlain, noreserve64.fb) read 64 MB in 1.4 s. The cause is in std/io/streams.fb:167: Add calls all.Reserve((all.Count + count) As Int) on every 64 KiB chunk. fb_array_reserve (runtime/src/array.cpp:131-141) calls resize_buffer to exactly the requested capacity whenever it is above the current one, so each chunk reallocates and copies everything read so far. Add's own reserve_one doubles the capacity instead. ReadAllAsync and ReadAllText share the same Add.

## S13-async-2 (low): An extending Interface can redeclare an Async member as plain, or the reverse, with no error at the Interface

Status: open

Where: compiler/src/sema.cpp:8762

check_refinements() reports a member of an extending Interface that takes the parameters of a member it extends but gives a different result. It never compares Async-ness. So 'Method LoadAsync(): Int' redeclaring Loader's 'Async Method LoadAsync(): Int' is accepted at the Interface. redeclares() also hides Loader's member, so generic code over Fast sees a plain LoadAsync. No type can ever implement Fast: a Class listing it gets 'implements Loader's, which is Async, so it has to be Async too', and one with its own Async LoadAsync gets 'is Async, so it can't implement Fast's'. A library that declares only the Interface never sees an error. The step's decision is that implementing matches Async-ness both ways, as Overrides do.

```
Import std.core

Interface Loader
    Async Method LoadAsync(): Int
    Method Size(): Int
End Interface

Interface Fast Extends Loader
    Method LoadAsync(): Int => 1
    Async Method Size(): Int => 2
End Interface

Function Use<F: Fast>(f: F): Int => f.LoadAsync()

Print("ok")
```

Expected: Errors at Fast's LoadAsync and Size, the way a different result is reported there ("'Name' takes Loader's parameters, so it gives what that one does, String, not Int32"): Loader's LoadAsync is Async, so Fast's has to be too, and Loader's Size isn't, so Fast's can't be.

Actual: Compiles and prints: ok

Suggested fix: In check_refinements(), when refined_result() matches but symbol(near).modifiers.is_async != symbol(far).modifiers.is_async, report it at the near member with a 'declared here' label and drop it, as for a different result. Make redeclares() require the same Async-ness. Add a sema test.

Skeptic: f2.fb (Fast Extends Loader, redeclaring Async LoadAsync as plain and plain Size as Async) compiles in debug and prints ok, with no error at Fast. f2b.fb shows that no Class can implement Fast. 'Class A Implements Fast' with no members of its own gets "'LoadAsync' implements Loader's, which is Async, so it has to be Async too" and "'Size' is Async, so it can't implement Loader's". A Class with its own Async LoadAsync and plain Size gets "'LoadAsync' is Async, so it can't implement Fast's" and "'Size' implements Fast's, which is Async...". f2c.fb, with only LoadAsync, also fails for every implementer. In sema.cpp, refined_result() (around line 8676) compares name, kind, Static, and parameters but never is_async. check_refinements() (around line 8728) reports only a different result, and redeclares() treats the pair as one member. Spec 06:151-154 says a redeclaring member is one member with the other, and the step's own rule (spec 10) matches Async-ness both ways. So an Interface that can never be implemented should be reported at the Interface, as a different result already is.

## S13-async-3 (low): An Async Method giving an Iterator's Struct or Class for a plain requirement is reported as a signature mismatch, not as Async

Status: open

Where: compiler/src/sema.cpp:4972

matches_required() turns off Iterator<T> boxing when either the candidate or the requirement is Async. For an Async candidate and a plain requirement whose result is Iterator<T>, the candidate stops matching altogether. The user then gets "doesn't match ..., which takes () and returns Iterator<Int32>" for a result a plain Method may give, instead of the Async error check_implementation gives a neighbouring member. The same applies the other way round: a plain candidate giving a Struct for an Async requirement gets 'doesn't match' instead of 'has to be Async too'.

```
Import std.core

Class Once Implements Iterator<Int>
    Field done: Bool
    Public Method Next(): Int?
        If done Then Return Null
        done = True
        Return 1
    End Method
End Class

Interface Lister
    Method Items(): Iterator<Int>
    Method Count(): Int
End Interface

Class C Implements Lister
    Async Method Items(): Once => Once()
    Async Method Count(): Int => 1
End Class

Print("ok")
```

Expected: p18.fb:17:20: error: 'Items' is Async, so it can't implement Lister's, which isn't
p18.fb:17:20: error: 'Count' is Async, so it can't implement Lister's, which isn't

Actual: p18.fb:17:20: error: C's 'Items' doesn't match Lister's, which takes () and returns Iterator<Int32>
p18.fb:17:20: error: 'Count' is Async, so it can't implement Lister's, which isn't

Suggested fix: Turn boxing off only when both are Async: const bool boxes = boxes_into(given_result, wanted_result) && !(candidate.modifiers.is_async && required.modifiers.is_async). A mismatch in Async-ness then matches, and check_implementation reports it. Add the sema test.

Skeptic: Ran f3.fb in debug. It printed "C's 'Items' doesn't match Lister's, which takes () and returns Iterator<Int32>" for 'Async Method Items(): Once', where Once is a Class implementing Iterator<Int>, and "'Count' is Async, so it can't implement Lister's, which isn't" for the neighbouring member. matches_required() in sema.cpp (around line 4972) sets boxes = boxes_into(...) && !candidate.is_async && !required.is_async. A mismatch in Async-ness alone therefore turns off the Iterator<T> substitution that a plain requirement allows, and the user gets a signature error for a result that is legal for a plain Method, instead of check_implementation's Async message. The member is still refused, so this is only a misleading diagnostic.

## S13-async-5 (low): Spec 06 still lets a Class implementing Iterator<T> stand in for an Async member's result, and spec 10 names only the Struct

Status: open

Where: docs/spec/10-concurrency.md:126

Spec 06 (06-user-types.md:205-208) says that where an Interface's Method gives an Iterator<T>, what implements it can give a Struct or Class that implements that Iterator<T>, with no exception for Async. Spec 10's new text says what implements an Async one gives 'not a Struct implementing the Iterator<T> it gives', which reads as if a Class is still fine. Sema refuses a Class too, which fits the step's 'exactly its result type' decision. Spec 10's 'A Static one is Async too, for generic code to call on its type' also reads as if every Static member is Async; it means 'can be Async'.

```
Import std.core

Class Once Implements Iterator<Int>
    Field done: Bool
    Public Method Next(): Int?
        If done Then Return Null
        done = True
        Return 1
    End Method
End Class

Interface Lister
    Async Method ItemsAsync(): Iterator<Int>
    Method Items(): Iterator<Int>
End Interface

Class C Implements Lister
    Async Method ItemsAsync(): Once => Once()
    Method Items(): Once => Once()
End Class

Print("ok")
```

Expected: As spec 06 reads (a Class implementing Iterator<T> can be given), this compiles and prints ok, unless the spec says Async members are the exception

Actual: p10.fb:17:20: error: C's 'ItemsAsync' doesn't match Lister's, which takes () and returns Iterator<Int32>

Suggested fix: Change spec 10 to say that what implements an Async one gives exactly the type the Interface's gives, not a Struct or Class implementing the Iterator<T> it gives, and that a Static one can be Async too. Add the Async exception to spec 06's Iterator<T> rule.

Skeptic: Ran f5.fb in debug and got "C's 'ItemsAsync' doesn't match Lister's, which takes () and returns Iterator<Int32>" for 'Async Method ItemsAsync(): Once', where Once is a Class. A Class is refused as well as a Struct, as the 'exact result' decision intends. Spec 06:205-208, unchanged by this step, still says that where an Interface's Method gives an Iterator<T>, what implements it can give a Struct or Class implementing it, with no Async exception, so it contradicts what sema now does. Spec 10's new text ('not a Struct implementing the Iterator<T> it gives') names only the Struct. Its 'A Static one is Async too' reads, word for word, as if every Static Interface member is Async, when it means one can be. Both are spec wording for what the step changed, which the lane owns. The fix is a few words in each.

## S13-buffered-binary-2 (low): BufferedWriter keeps a whole-buffer write, though its comments say it goes straight through

Status: open

Where: std/io/buffered.fb:138

Write's comment says a write of a whole buffer or more goes straight to the other Writer once what waits is sent. The class comment says bytes wait until the buffer fills. In the code, the `from.Count >= Held.Count` check is nested inside `If Length + from.Count > Held.Count`. So a write of exactly Held.Count bytes into an empty BufferedWriter is copied into Held and kept, which is an extra copy, and it waits for the next write or a Flush. A full buffer is also not sent when it fills: it waits for the next write. Rust's BufWriter, which the lane follows, checks `buf.len() >= capacity` outside the overflow check and writes such a buffer straight through. WriteAsync (line 150) has the same structure.

```
Import std.core
Import std.ffi
Import std.io

// Counts the writes it's given.
Class Counted Implements Writer
    Field Writes: Int

    Method Write(from: Buffer<UInt8>) Throws IOError
        Writes += 1
    End Method
End Class

// A write of a whole buffer, with nothing waiting, should go straight through.
Local inner = Counted()
Local buffered = BufferedWriter(inner, 4)
Try buffered.WriteText("abcd")
Print("{inner.Writes}")
Try buffered.WriteText("efghi")
Print("{inner.Writes}")
```

Expected: 1
2

Actual: 0
2

Suggested fix: Either move the whole-buffer check out of the overflow check (`If Length + from.Count > Held.Count Then Try Send()`, then `If from.Count >= Held.Count` write straight through, else Keep) in both Write and WriteAsync, or change the two comments to say that a full buffer waits until a write overflows it and that only a write larger than the room left goes straight through.

Skeptic: I ran f2.fb with a counting Writer and a 4-byte BufferedWriter. WriteText("abcd") into an empty buffer left Writes at 0. The next WriteText("efghi") brought it to 2, with sizes `4 5`. In std/io/buffered.fb lines 135-145 and 147-157, the `from.Count >= Held.Count` check is nested inside `If Length + from.Count > Held.Count`. So a write of exactly a whole buffer is copied in and kept. Write's comment (lines 133-134) says such a write goes straight to the other Writer. The data still arrives on the next write or Flush, so the harm is an extra copy and a comment that contradicts the code. The other half of the claim, that a full buffer waits for the next write, matches Rust's BufWriter and io_streams.out's `cd: []`. Only the whole-buffer write contradicts the comment.

## S13-buffered-binary-3 (low): ReadText's error for a count past what an array holds names ReadBytes, and isn't UNEXPECTED_END as the class comment says

Status: open

Where: std/io/binary.fb:55

ReadText(count) checks only for a negative count, then calls ReadBytes. ReadBytes throws IOError(OTHER, "ReadBytes reads up to the 2147483647 bytes an array holds, ...") for a count over 2147483647. So a ReadText call reports an error that names a Method the caller never called. A broken file's UInt32 length prefix is over that limit half the time, so this is a likely path. Also, BinaryReader's class comment says each read throws UNEXPECTED_END when the stream ends before what it reads. Here the stream has 1 byte and the error's Kind is OTHER, so either the comment or the Kind is wrong for this path.

```
Import std.core
Import std.io

// A broken file's count, past what an array holds, read as text.
Try
    Local text = Try BinaryReader(StringReader("x")).ReadText(3000000000)
Catch e As IOError
    Print("{e.Kind}: {e.Message}")
End Try
```

Expected: An error that names ReadText, for example: OTHER: ReadText reads up to the 2147483647 bytes an array holds, not 3000000000. Either that, or the class comment says that this case is OTHER and not UNEXPECTED_END.

Actual: OTHER: ReadBytes reads up to the 2147483647 bytes an array holds, not 3000000000

Suggested fix: Have ReadText run its own limit check with its own name, or pass the Method's name into a shared check. Then add to BinaryReader's class comment, or ReadBytes' comment, that a count over 2147483647 throws OTHER, and test that path. No test reaches it now.

Skeptic: I ran f3.fb in debug. ReadText(3000000000) printed `OTHER: ReadBytes reads up to the 2147483647 bytes an array holds, not 3000000000`. In std/io/binary.fb, ReadText (lines 75-78) checks only for a negative count, then calls ReadBytes. ReadBytes throws with its own name at lines 54-56. For the negative-count panic, the lane wrote ReadText a separate message that names ReadText (line 76), which shows each Method is meant to name itself. The over-limit path was missed. On a stream that ends early, the Kind is also OTHER, while the class comment (lines 17-18) and stdlib.md ('an early end UNEXPECTED_END') say UNEXPECTED_END. The message is wrong and the docs leave this case out.

## S13-buffered-binary-4 (low): BufferedReader.ReadByte has no Async twin, though ReadLine has one

Status: open

Where: std/io/buffered.fb:50

docs/stdlib.md's conventions say blocking I/O has an Async twin. ReadByte blocks when it has to fill from the other Reader, the same way ReadLine does. ReadLine has ReadLineAsync, but ReadByte has no ReadByteAsync. So code reading a socket a byte at a time through a BufferedReader (step 28) can't wait on the main thread without calling ReadAsync with a 1-byte Buffer. The lane listed BinaryReader, BinaryWriter, and Seeker as blocking only on purpose, but not ReadByte.

```
Import std.core
Import std.io

Local r = BufferedReader(StringReader("xy"), 4)
Print("{Try Await r.ReadByteAsync() ?? 0}")
```

Expected: 120

Actual: t12.fb:5:21: error: BufferedReader has no member named 'ReadByteAsync'

Suggested fix: Add `Public Async Method ReadByteAsync(): UInt8? Throws IOError` through FillAsync, list it in docs/stdlib.md's table row for buffered.fb, and test it in io_async.fb. Or, if leaving it out is intended, list ReadByte with the streams that are blocking only.

Skeptic: I ran f4.fb. It failed to compile with `error: BufferedReader has no member named 'ReadByteAsync'`. ReadByte (std/io/buffered.fb line 50) blocks on Inner.Read through Fill whenever Held is empty. The convention in docs/stdlib.md (line 188) says 'Blocking I/O has an Async twin'. ReadLine, in the same class, has ReadLineAsync, and stdlib.md's table row for buffered.fb lists ReadByte, ReadLine, and ReadLineAsync. The lane's blocking-only exceptions are BinaryReader, BinaryWriter, and Seeker, and ReadByte isn't among them. ReadAsync with a 1-byte Buffer works around it, so this is low.

## S13-buffered-binary-5 (low): Panics and paths in buffered.fb and binary.fb that no test covers

Status: open

Where: tests/programs/io_streams.fb:1

No test runs these paths: (1) BufferedWriter's size-below-1 panic. Only BufferedReader's has a panic program (panic_buffered_size), while Buffer has one panic program for each of its panics. (2) BinaryReader.ReadBytes' and ReadText's negative-count panics. (3) ReadBytes' OTHER IOError for a count over 2147483647. (4) BufferedReader.ReadAsync, the Async read of the buffered stream, which io_async.fb never calls: it uses only ReadLineAsync, and its other ReadAsync calls are on Trickle and MemoryStream. (5) BufferedWriter's Destroy losing a write error, which the class comment promises. Each of these works when run (see the program), but nothing would catch a regression in them.

```
Import std.core
Import std.ffi
Import std.io

// BufferedReader.ReadAsync, which no test calls.
Local r: Reader = BufferedReader(StringReader("xyz"), 2)
Local into = Buffer<UInt8>(1)
Print("{Try Await r.ReadAsync(into)} {into[0]}")
// A count past what an array holds, which no test reaches.
Try
    Local bytes = Try BinaryReader(StringReader("")).ReadBytes(2147483648)
Catch e As IOError
    Print("{e.Kind}")
End Try
// BufferedWriter's size panic, which has no panic program, as ReadBytes' and ReadText's
// negative-count panics have none.
Local w = BufferedWriter(StringWriter(), 0)
```

Expected: Each path has a test: panic programs for BufferedWriter's size and for ReadBytes'/ReadText's negative counts, and lines in io_streams.fb/io_async.fb for the over-limit IOError, BufferedReader.ReadAsync, and a Destroy whose write throws.

Actual: The program prints 1 120, then OTHER, then panic: a BufferedWriter writes 1 byte at a time at least, not 0 at t16.fb:17:11. None of these paths appears in tests/.

Suggested fix: Add panic_buffered_writer_size.fb, panic_binary_read_bytes.fb, and panic_binary_read_text.fb. In io_streams.fb, add a ReadBytes(2147483648) case and a BufferedWriter whose inner Write throws, going out of scope without an error. In io_async.fb, add a ReadAsync through a BufferedReader held as a Reader value.

Skeptic: I ran f5.fb. It printed `1 120`, then `OTHER`, then `panic: a BufferedWriter writes 1 byte at a time at least, not 0`, so each path works. I ran f6.fb: ReadText(-1) panics with its own message. I ran f1b.fb: a BufferedWriter whose inner Write throws goes out of scope and prints `went`. Searching tests/ turned up no test for any of these paths. The only panic program is panic_buffered_size.fb, which is BufferedReader's. Nothing runs ReadBytes or ReadText with a negative count or a count over 2147483647: io_streams.fb's largest is 1000000000. io_async.fb never calls BufferedReader.ReadAsync, including its `into.Count >= Held.Count` path straight to Inner.ReadAsync; its ReadAsync calls are on Trickle and MemoryStream. No test covers Destroy losing a write error. docs/history.md records the project's rule of 'a panic program for each panic', and Buffer has one for each of its panics. Finding 1's bug sat in exactly this kind of untested path. No wrong behavior is involved, so this is low.

## S13-streams-2 (low): MemoryStream.Write after a Seek near Int64's max overflows: it panics, differently in debug and release, instead of throwing its size IOError

Status: open

Where: std/io/memory.fb:38

Seek accepts any place up to Int64's max (SeekIn only checks for overflow and negative places). Write then computes `finish = At + from.Count` without a check. Debug builds trap on the overflow inside std. Release builds wrap finish to a negative number, which passes the `finish > 2147483647` check, and then index Data[At As Int] with the truncated value -1 and panic on that. Writes at any place from 2147483648 up to Int64.Max minus the write's size correctly throw "a MemoryStream holds up to 2147483647 bytes"; only the last few positions break. The fix is to compare against the limit without overflowing.

```
Import std.core
Import std.io

// A Seek far past the end is allowed; a write there should throw the MemoryStream's size IOError.
Local m = MemoryStream([1, 2, 3])
Print("{Try m.Seek(9223372036854775807, SeekOrigin.START)}")
Try
    Try m.WriteText("a")
    Print("wrote")
Catch e As IOError
    Print("{e.Kind}: {e.Message}")
End Try
```

Expected: 9223372036854775807
OTHER: a MemoryStream holds up to 2147483647 bytes, not ... (an IOError in both debug and release, as for any write that ends past 2147483647)

Actual: debug:
9223372036854775807
panic: integer overflow in '+' at std/io/memory.fb:38:24
release:
9223372036854775807
panic: index -1 is out of range for 3 elements at std/io/memory.fb:47:36

Suggested fix: Check before adding, for example `If At > 2147483647 - from.Count Then Throw ...`, or use At.CheckedAdd(from.Count) and throw when it is Null or above 2147483647. The message then needs wording that doesn't print the overflowed finish.

Skeptic: I ran seekmax.fb: MemoryStream([1,2,3]), Seek(9223372036854775807, START), then WriteText("a"). Debug printed 9223372036854775807, then 'panic: integer overflow in '+' at std/io/memory.fb:38:24'. Release printed 'panic: index -1 is out of range for 3 elements at std/io/memory.fb:47:36'. A write at 9223372036854775800 (seekmax2.fb) correctly throws 'OTHER: a MemoryStream holds up to 2147483647 bytes, not 9223372036854775801' in both builds. The cause is that memory.fb:38 computes `finish = At + from.Count` with no overflow check before the `finish > 2147483647` test. In release the sum wraps negative, passes the check, and Data[At As Int] panics. Seek allows such a place, so a write that should throw a catchable IOError panics instead. Only places within a write's size of Int64's max are affected, so I rate it low.

## S13-streams-3 (low): A Seek whose place overflows Int64 forward is reported as before the stream's start

Status: open

Where: std/io/memory.fb:138

SeekIn throws the same message when start.CheckedAdd(offset) overflows (gives Null) and when the place is negative. A large positive offset from END or CURRENT that overflows Int64 is past the end, not before the start, but the error says "is before the stream's start". The Seeker doc in streams.fb also says only that a place before the start throws, which doesn't cover this case.

```
Import std.core
Import std.io

// A Seek whose place is past what an Int64 holds is after the end, not before the start.
Local m = MemoryStream([1, 2, 3])
Try
    Try m.Seek(9223372036854775807, SeekOrigin.END)
Catch e As IOError
    Print("{e.Kind}: {e.Message}")
End Try
Try m.Seek(5, SeekOrigin.START)
Try
    Try m.Seek(9223372036854775807, SeekOrigin.CURRENT)
Catch e As IOError
    Print("{e.Kind}: {e.Message}")
End Try
```

Expected: An IOError whose message says the place is past what a stream can reach (more than an Int64 counts), for example "a Seek of 9223372036854775807 from 3 is past what an Int64 counts".

Actual: OTHER: a Seek of 9223372036854775807 from 3 is before the stream's start
OTHER: a Seek of 9223372036854775807 from 5 is before the stream's start

Suggested fix: Give the Null (overflow) case its own message in SeekIn, and say in Seeker.Seek's comment that a place past Int64's range throws too.

Skeptic: I ran seekover.fb in debug and release. MemoryStream.Seek(9223372036854775807, END) from 3 bytes printed 'OTHER: a Seek of 9223372036854775807 from 3 is before the stream's start'. After Seek(5, START), the same offset from CURRENT printed '... from 5 is before the stream's start'. StringReader gives the same message. memory.fb:137-140 uses one message both when CheckedAdd overflows (Null) and when the place is below 0, so a forward overflow is reported as being before the start, which is wrong. Seeker.Seek's comment in streams.fb:63-65 also mentions only places before the start.

## S13-streams-5 (low): streams.fb's header says Seeker has Async twins, but by design it has none

Status: open

Where: std/io/streams.fb:1

The file's header says Reader, Writer, and Seeker each have Async twins that wait on the main thread. stdlib.md (step 13's note) says Seeker has no Async twin, and the code gives it none, so the header contradicts both the design and the code.

```
Import std.core
Import std.io

// streams.fb's header says Reader, Writer, and Seeker each have Async twins.
Local s: Seeker = MemoryStream([1, 2, 3])
Print("{Try Await s.SeekAsync(1, SeekOrigin.START)}")
```

Expected: The header says only Reader and Writer have Async twins, matching stdlib.md, so this program is expected not to compile.

Actual: t11.fb:6:21: error: Seeker has no member named 'SeekAsync' (the code is right; the header comment is wrong)

Suggested fix: Reword the header, for example: "Reader, what bytes come from, and Writer, where they go, each with Async twins ...; and Seeker, what moves around in them".

Skeptic: Lines 1-2 of std/io/streams.fb read: 'Reader, what bytes come from, Writer, where they go, and Seeker, what moves around in them, each with Async twins'. stdlib.md (std.io, step 13's note) says 'Seeker has no Async twin', and the code gives Seeker none. Running t11.fb (Seeker value, Await s.SeekAsync) in debug and release fails with 't11.fb:6:21: error: Seeker has no member named 'SeekAsync''. The header comment contradicts both stdlib.md and the code.

## Refuted by the skeptics (no action)

- S13-async-4: milestones.md and decisions.md still say Interfaces' Methods can't be Async. f4.fb prints 10, which is correct. milestones.md:265 still has the Deferred row and decisions.md:349 still says an Interface's Methods can't be Async yet. But docs/m6-plan.md's Roles say the coordinator 'alone edits CLAUDE.md, docs/milestones.md, docs/decisions.md, docs/history.md', and Landing step 3 has the coordinator update milestones.md and decisions.md when the branch lands. Lane decisions are reported to the coordinator, who records them in decisions.md (m6-plan 'Language calls'). A lane branch is not supposed to touch these files, so their state before landing is not a defect in step 13's change. The spec and stdlib.md, which the lane does own, were updated.
- S13-streams-4: No test reaches Copy's, ReadAll's, or their Async twins' 65536-byte chunk edge, or the MemoryStream size limit. I ran the reviewer's program in debug and release, adding CopyAsync. Every case is correct: '65535 65535 65535 True' through '131073 131073 131073 True', and 'OTHER: a MemoryStream holds up to 2147483647 bytes, not 2147483648'. So the code behaves correctly, and the done-when ('each stream reads back what was written, at buffer edges too') is about the streams. io_streams.fb does test the buffered streams' buffer edges (BufferedReader with sizes 1, 2, 3, 4 and 64, BufferedWriter with 4 and 16), and MemoryStream and StringReader have no buffer of their own. Copy's and ReadAll's 64 KiB working buffer, and the size limit, are not stream buffer edges, so nothing contradicts the row. Even so, the regression tests for S13-streams-1 (a ReadAll or ReadAllAsync over more than one chunk) and S13-streams-2 (a write near the size limit) should add exactly these cases.
