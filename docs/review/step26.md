# The review of M6 step 26, std.term: the console (keys, mouse, pastes, resizes, colors, raw mode), line widgets (tables, boxes, progress bars, spinners, prompts), and full-screen apps (a Screen that redraws only changed cells, canvases, regions, text inputs, lists); it sits on copies of steps 5, 6, and 7, so review only lane 12's commits

34 confirmed findings: 2 high, 13 medium, 19 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## console-1 (high): A paste whose start mark ends a read loses its end mark: the text includes \e[201~ and swallows keys typed for the next second

Status: fixed in 8516dbd7 (term_input, "paste's mark alone"): Parse sets PasteScan back to 0 where it clears Bytes, so a paste's start mark at the end of one read, and its text and end mark in the next, give paste "hi"; without it, the parser waited and flushed paste "hi\x1B[201~".

Where: std/term/input.fb:247

When the bytes run out right after "\e[200~", Parse goes back to the top of its loop and its `At >= Bytes.Count` branch clears Bytes and sets At to 0. PasteScan keeps its old value: the index just past the start mark. On the next read, FindPasteEnd starts scanning at Max(At, PasteScan), which is past where a short paste's end mark now sits. So a paste whose text is shorter than the bytes that came before it in its first read (a word or a number when the start mark came alone, as xterm writes it) never finds its end. It waits PASTE_WAIT and then flushes everything as the paste: the end mark, plus any keys typed in that second. I reproduced it in a real Windows console (ConPTY) through Terminal.Console() in raw mode: typing "\e[200~", then 200 ms later "hi\e[201~", then 300 ms later "x" gave one event, paste "hi\x1B[201~x". FakeTerminal shows it too, because each Feed is a separate read. That contradicts its doc, which says "Bytes fed one after another come as one" (terminal.fb:427). No test feeds a paste's start mark on its own.

```
Import std.core
Import std.term

Const E = "\u{1b}"

// A paste whose start mark is the last thing in a read, and whose text and end come in the next.
Local parser = InputParser()
parser.Feed("{E}[200~")
Print("first:", parser.Next() == Null, parser.IsPasting, parser.IsWaiting)
parser.Feed("hello{E}[201~")
Local event = parser.Next()
Print("next:", event == Null ? "Null" : "{event}")
If event == Null
    Local flushed = parser.Flush()
    Print("flushed:", flushed == Null ? "Null" : "{flushed}")
End If

// The same through a FakeTerminal, as two reads.
Local fake = FakeTerminal(80, 24)
fake.Feed("x{E}[200~")
fake.Feed("hello{E}[201~")
Loop
    Local next = fake.ReadEvent()
    If next == Null Then Break
    Print("read:", "{next}")
End Loop
```

Expected: first: True True False
next: paste "hello"
read: key x
read: paste "hello"

Actual: first: True True False
next: Null
flushed: paste "hello\x1B[201~"
read: key x
read: paste "hello\x1B[201~"

Suggested fix: Reset PasteScan to 0 in Parse's `At >= Bytes.Count` branch, where Bytes is cleared. Alternatively, have FindPasteEnd scan from Max(At, Min(PasteScan, Bytes.Count - 5)). Add a test that feeds "\e[200~" in its own read, then a short text and the end mark together.

Skeptic: The given build-rev2 fb.exe is stale: it was built 22:01, before the 01:18 checkout of 2dc3d44f, and it can't compile std ("'Char' isn't a type"). So I used the earlier reviewer's build at rv-term/console/b, which is configured on rev-crypto and newer than every compiler, runtime and cli source. My programs are in rv-term/console-v/. c1.fb printed the same in debug and release: "first: True True False / next: Null / flushed: paste \"hello\x1B[201~\" / read: key x / read: paste \"hello\x1B[201~\"". c1c.fb ran in raw mode through my own ConPTY runner, with \e[200~ typed, then hi\e[201~ 200 ms later, then x 300 ms later. It gave one event, paste "hi\x1B[201~x". Cause: input.fb:246-248 clears Bytes and sets At to 0, but PasteScan keeps its old value (6). FindPasteEnd (480) then starts past the end mark at index 2-5. The paste text gets corrupted and keys typed for up to a second are swallowed.

## screen-1 (high): TextInput keeps its sideways scroll after its text gets shorter, so text that fits is hidden and the input looks empty

Status: fixed in 1dc0c798 (term_screen, the input whose text gets shorter): TextInput.Draw scrolls no further than its text's end needs, Min(Scroll, Max(width of the text - width + 1, 0)), so Backspace, Ctrl+W, and the Text setter bring what it scrolled past back into view ("7 [ghijk l]" down to "7 [abcdefg]", and "2 [hi]"). Ask's line in prompts.fb scrolled the same way and is fixed alike, in 6e01555a (term_prompts' "scrolled ask").

Where: std/term/views.fb:220

TextInput.Draw only moves Scroll to keep the cursor in view (cursor < Scroll, or cursor >= Scroll + width). It never brings Scroll back when the text gets shorter. Say you type past the input's width and then press Backspace, Ctrl+W, or Alt+Backspace, or set Text: the view stays scrolled right. The columns on the right go empty while the text on the left stays hidden. Once the cursor reaches Scroll, the input draws nothing at all and puts the cursor at column 0, even though the whole text would fit. This is normal use: type a long line, then back up. (I ran these with a fb built from 2dc3d44f, rv-term/widgets/b/cli/fb.exe. build-rev2's fb.exe predates the checkout and can't compile std.term.) Debug and release give the same output.

```
Import std.core
Import std.term

// Type past an 8-column input's width, then take it back with Backspace.
Local input = TextInput()
Local line = Canvas(8, 1)
For c In "abcdefghijk".Chars
    input.HandleKey(KeyEvent(c))
End For

line.Clear()
Print(input.Draw(line, line.Area), "[{line}]", input.Text)
For i = 0 Until 8
    input.HandleKey(KeyEvent(KeyCode.BACKSPACE))
    line.Clear()
    Print(input.Draw(line, line.Area), "[{line}]", input.Text)
End For

input.Text = "hi"
line.Clear()
Print(input.Draw(line, line.Area), "[{line}]", input.Text)
```

Expected: No column goes empty while text is hidden to its left, and text that fits shows in full:
7 [efghijk] abcdefghijk
7 [defghij] abcdefghij
7 [cdefghi] abcdefghi
7 [bcdefgh] abcdefgh
7 [abcdefg] abcdefg
6 [abcdef] abcdef
5 [abcde] abcde
4 [abcd] abcd
3 [abc] abc
2 [hi] hi

Actual: 7 [efghijk] abcdefghijk
6 [efghij] abcdefghij
5 [efghi] abcdefghi
4 [efgh] abcdefgh
3 [efg] abcdefg
2 [ef] abcdef
1 [e] abcde
0 [] abcd
0 [] abc
0 [] hi

Suggested fix: In Draw, before the two cursor checks, clamp the scroll to what the text needs: Scroll = Min(Scroll, Max(DisplayWidth(shown) - width + 1, 0)). Add a term_screen case that types past the width and then uses Backspace, Ctrl+W, and the Text setter.

Skeptic: I ran rv-term/fullscreen-v/s1.fb (the finding's program) with the fb built from 2dc3d44f (rv-term/widgets/b/cli/fb.exe). build-rev2's fb.exe really can't compile std at this commit: it fails in std/core/format.fb. Debug and release both print 7 [efghijk], 6 [efghij], 5 [efghi], 4 [efgh], 3 [efg], 2 [ef], 1 [e], 0 [] abcd, 0 [] abc, 0 [] hi. So an 8-column input shows nothing while it holds "abcd", and after Text = "hi" it shows nothing too. In views.fb:220-221 Draw only moves Scroll when the cursor leaves the view. Nothing brings Scroll back when the text gets shorter: with Scroll=4, "abcd" has its cursor at 4, which is not < Scroll, and "hi" sets Scroll=2, so every grapheme sits left of Scroll. This breaks stdlib.md's "scrolled sideways to keep its cursor in view" in normal typing.

## console-2 (medium): On Linux and macOS, Ctrl+\ in raw mode kills the program with SIGQUIT and leaves the terminal raw

Status: fixed in 877cf4e5 (term_tests: "raw mode reads each key as it's typed, Ctrl+\ too", and the pty's keys test types Ctrl+\ and reads 1c): raw mode, now fb::term::raw_mode(), sets VQUIT to _POSIX_VDISABLE, so Ctrl+\ is a key on Linux and macOS as on Windows; checked in WSL Ubuntu's pty.

Where: runtime/src/term.cpp:173

apply_mode leaves ISIG on (so Ctrl+C ends the program), which also leaves VQUIT active: Ctrl+\ sends SIGQUIT. fb_start installs its run_end handler only for SIGINT, SIGTERM and SIGHUP, so SIGQUIT kills the process (core dump) without restore_terminal running. Echo and canonical mode stay off, and a Screen's alternate screen, hidden cursor and mouse reporting stay on. stdlib.md says the console is put back "however the program ends, a signal's end too". On Windows the same key is just a key ("key Ctrl+\" in a ConPTY run), so the platforms also differ. Evidence: I built runtime/src/term.cpp and runtime.cpp at 2dc3d44f in WSL Ubuntu, with a probe that calls fb_start, fb_term_set_mode(1) and fb_term_read, and drove it in a pty, typing 'a' and then a control key. With Ctrl+C: "signal 2; after: ECHO=1 ICANON=1". With Ctrl+\: "signal 3; after: ECHO=0 ICANON=0".

```
Import std.core
Import std.term

// Run in a terminal on Linux or macOS and press Ctrl+\ (then run `stty -a`).
Local console = Terminal.Console()
console.StartRawMode()
Loop
    Local key = console.ReadKey()
    If key == Null Then Break
    Print("key:", "{key}")
    If key.IsChar('q') Then Break
End Loop

console.EndRawMode()
```

Expected: Either "key: Ctrl+\" (as on Windows), or the program ends with the terminal put back (echo and icanon on), as it does for Ctrl+C.

Actual: The process dies of SIGQUIT (Quit, core dumped) and the terminal is left with -echo -icanon (probe in a pty: "Ctrl+\ in raw mode: signal 3; after: ECHO=0 ICANON=0 ISIG=1").

Suggested fix: In apply_mode, set raw.c_cc[VQUIT] = _POSIX_VDISABLE, so Ctrl+\ is read as a key as on Windows, or add SIGQUIT to the signals fb_start routes through run_end.

Skeptic: I built term.cpp and runtime.cpp at 2dc3d44f in WSL Ubuntu (~/rvcv), with a probe that calls fb_start, fb_term_set_mode(1) and fb_term_read. I drove it in a pty with drive.py. Ctrl+C gave "signal 2; after: ECHO=1 ICANON=1 ISIG=1". Ctrl+\ gave "signal 3; after: ECHO=0 ICANON=0 ISIG=1", so the terminal was left raw. The code agrees: apply_mode (term.cpp:171-183) keeps ISIG and VQUIT, and fb_start (runtime.cpp:335) routes only SIGINT, SIGTERM and SIGHUP through run_end. That contradicts stdlib.md's "puts the console back however the program ends, a signal's end too".

## console-3 (medium): A ReadKey or PollEvent timeout near Duration.MaxValue() panics in debug and returns Null at once in release

Status: fixed in 8516dbd7 and 877cf4e5 (term_input's "forever", and term_tests' pipe read with Int64's most as its timeout, sent 50 ms later): NextEvent and KeyWithin work their deadline out through DeadlineOf, which gives none for a timeout past what the clock reaches, and fb_term_read through fb::term::deadline_of(), which does the same.

Where: std/term/terminal.fb:282

KeyWithin (line 282) and NextEvent (line 242) compute `Instant.Now().Nanoseconds + timeout`. For a very long timeout such as Duration.MaxValue() (a natural way to say "wait forever"), this overflows: debug builds panic. In release the .fb arithmetic wraps back correctly, but fb_term_read then computes `steady_clock::now() + nanoseconds(timeout)` (term.cpp:858-861), which overflows the same way (signed overflow, UB). The deadline lands in the past, so a pipe's read returns 0 at once and ReadKey gives Null with no key read. A 10-day timeout works correctly.

```
Import std.core
Import std.time
Import std.term

// A key that comes after a second, waited for as long as a Duration can be.
// Run as: (sleep 1; echo a) | program
Local started = Instant.Now()
Local key = ReadKey(Duration.MaxValue())
Print("key:", key == Null ? "Null" : "{key}", "after a second:", Instant.Now() - started >= 900.Milliseconds)
```

Expected: key: a after a second: True (in both debug and release)

Actual: debug: panic: integer overflow in '+' at std/term/terminal.fb:282:44 (PollEvent: terminal.fb:242:44)
release: key: Null after a second: False

Suggested fix: Saturate the deadline in NextEvent and KeyWithin: when timeout > Int64 max minus now, treat it as -1 (no deadline). In fb_term_read, compute the deadline with saturation, or treat a timeout longer than what time_point can hold as forever.

Skeptic: I ran c3.fb (ReadKey(Duration.MaxValue())) and c3b.fb (console PollEvent(Duration.MaxValue())) as `(sleep 1; echo a) | exe`. Debug: "panic: integer overflow in '+' at std/term/terminal.fb:282:44" and "...terminal.fb:242:44", both exit 101. Release: "key: Null after a second: False" and "event: Null after a second: False", so Null came at once with no key read. The cause is `Instant.Now().Nanoseconds + timeout` with no saturation in KeyWithin and NextEvent. In release, fb_term_read's steady_clock::now()+nanoseconds(timeout) also overflows.

## console-4 (medium): The console's ReadEvent and PollEvent without StartRawMode read keys as typed, without echo, on Windows, but a line at a time, echoed, on Linux and macOS

Status: fixed in 8516dbd7 (term_input's "read in raw mode", a Terminal of the program's own that keeps each read's input mode): ReadEvent and PollEvent start raw mode while they wait, as ReadKey does, so the console reads keys as they're typed, without echo, on every OS.

Where: std/term/terminal.fb:147

ReadKey and ReadPassword enter raw mode while they wait; ReadEvent and PollEvent do not. On Windows, read_console uses ReadConsoleInputW, which bypasses line input and echo, so Terminal.Console().ReadEvent() gets each key at once without echo. On POSIX, the terminal is still in canonical mode with echo, so the same call shows what is typed and gives nothing until Enter, then the whole line with "\n". The Terminal doc says "Reading keys is in raw mode, where they come as they're typed, without showing them". A game loop that calls PollEvent without StartRawMode works on Windows only. Evidence: on Windows (ConPTY), typing "ab" without Enter gave "event: key a" and "event: key b" at once, with no echo. In WSL, a probe calling fb_term_read on a cooked pty showed the echo "ab" and gave "bytes 61 62 0a" only after Enter.

```
Import std.core
Import std.term

// The console's events, read without StartRawMode, until q. Type "ab" and wait before Enter.
Local console = Terminal.Console()
Print("ready")
Flush()
Loop
    Local event = console.ReadEvent()
    If event == Null Then Break
    Print("event:", "{event}")
    Flush()
    If event.IsChar('q') Then Break
End Loop
```

Expected: The same on every OS: either "event: key a" and "event: key b" as typed, with no echo (raw while waiting, as ReadKey does), or line input everywhere.

Actual: Windows: event: key a, event: key b at once, with no echo. Linux: "ab" echoed, then key a, key b and Enter only after Enter is pressed.

Suggested fix: Make NextEvent use raw mode while it waits, as KeyWithin does (StartRawMode/EndRawMode around the wait when it isn't already raw), or document that ReadEvent and PollEvent need StartRawMode and make Windows match in cooked mode.

Skeptic: Windows: c4t.fb calls Terminal.Console().ReadEvent() without StartRawMode. Run in ConPTY with "ab" typed, then Enter a second later, it printed "event: key a at 0" and "event: key b at 0" immediately, and the raw output had no echo of ab. read_console uses ReadConsoleInputW whatever the mode. Linux: in WSL, the same fb_term_read on a cooked pty (probe with mode 0, cooked.py) echoed 'ab' at 1.01s and gave "bytes 61 62 0a" only at 2.01s, after Enter. So the same call is key-at-a-time without echo on Windows, and line input with echo on POSIX. Terminal's doc says reading keys is in raw mode.

## console-5 (medium): Modified F1 to F4 in SS3 form (Konsole's \eO2P) and the Linux console's F1 to F5 (\e[[A) are read as a typed letter

Status: fixed in 8516dbd7 (term_input's "konsole's f keys" and "linux console's f keys"): Sequence takes digits after "\eO" as xterm's modifier number, so "\eO2P" is Shift+F1, and Csi takes "\e[[" and A to E as F1 to F5, and skips "\e[[" and any other final byte whole.

Where: std/term/input.fb:508

Sequence reads "\eO" plus exactly one byte. Konsole sends Shift+F1 as "\eO2P" and Ctrl+F2 as "\eO5Q". Ss3Key('2') is Null, so "\eO2" is skipped and 'P' comes through as a CHAR key. The Linux virtual console sends F1 to F5 as "\e[[A" to "\e[[E". Csi takes '[' as the final byte, skips "\e[[" as an unknown sequence, and then 'A' to 'E' come through as typed letters. InputParser's doc says "A sequence it doesn't know is skipped"; here the end of one becomes a key, which a prompt or TextInput would insert.

```
Import std.core
Import std.term

Const E = "\u{1b}"

// Every event in text, taking what waits as it is.
Function Events(text: String): String
    Local parser = InputParser()
    parser.Feed(text)
    Local shown = ""
    Loop
        Local event = parser.Flush()
        If event == Null Then Break
        shown += "[{event}]"
    End Loop

    Return shown
End Function

// Konsole's Shift+F1 and Ctrl+F2, SS3 with a modifier, and the Linux console's F1 and F5.
Print("konsole shift+f1:", Events("{E}O2P"), "ctrl+f2:", Events("{E}O5Q"))
Print("linux f1:", Events("{E}[[A"), "f5:", Events("{E}[[E"))
```

Expected: konsole shift+f1: [key Shift+F1] ctrl+f2: [key Ctrl+F2]
linux f1: [key F1] f5: [key F5]
(or at least nothing, if skipped)

Actual: konsole shift+f1: [key P] ctrl+f2: [key Q]
linux f1: [key A] f5: [key E]

Suggested fix: In Sequence, accept digits after "\eO" as an xterm modifier number (ModifiersOf) before the final letter. In Csi, handle "\e[[" plus A to E as F1 to F5, taking the fourth byte, before the parameter scan.

Skeptic: c5.fb printed "konsole shift+f1: [key P] ctrl+f2: [key Q]" and "linux f1: [key A] f5: [key E]". For comparison, unknown sequences like \e[99X and \eOz are skipped whole (empty). Sequence (input.fb:508) takes exactly one byte after \eO, so '2' is skipped and P becomes a typed char. Csi takes '[' as the final byte of \e[[, so A-E become typed letters. That contradicts InputParser's doc, "A sequence it doesn't know is skipped". A TextInput or prompt would insert the letter.

## screen-2 (medium): Canvas.Fill takes any text: a wide character, several characters, or "" break the cell grid that Present relies on

Status: fixed in 1dc0c798 (term_screen's "wide fill", and panic_term_fill): Fill panics at the caller for text that isn't one grapheme cluster one or two columns wide, "" too, and fills two cells with a wide one, with a space in a last column without room for it.

Where: std/term/screen.fb:129

Fill's doc says its text is 'a grapheme cluster that takes one' cell, but nothing checks that. Fill calls Put(column, row, text, False, look) for every cell, whatever the text is. With "中" or an emoji like "🟩" (a plausible fill for a game board), each one-column cell holds a two-column character. With "ab", each cell holds two characters. Present then writes 8 columns into a 4-column row, so the terminal wraps, and its cursor tracking (CUF gaps, column + 1) falls out of step with the screen. Fill with "" makes every cell look like the right half of a wide character, so Present skips those cells and the terminal keeps whatever it showed before. std's convention is that a bad argument panics at the caller.

```
Import std.core
Import std.text
Import std.term

Function Shown(text: String): String => text.Replace("\u{1b}", "\\e")

// Fill with a wide grapheme cluster, two of them, and an empty one.
Local fake = FakeTerminal(4, 3, ColorLevel.NONE)
Local screen = Screen(fake, False)
fake.TakeOutput()
screen.Frame.Fill(Region(0, 0, 4, 1), "中", Style())
screen.Frame.Fill(Region(0, 1, 4, 1), "ab", Style())
screen.Frame.Fill(Region(0, 2, 4, 1), "", Style())
Print(screen.Present(), Shown(fake.TakeOutput()))
Print("[{screen.Frame}]")
```

Expected: A panic at the first Fill's line, saying Fill takes one grapheme cluster one cell wide, not "中" (or else wide ones drawn two cells each, so a row never holds more than 4 columns).

Actual: 8 \e[1;1H中中中中\e[2;1Habababab
[中中中中
abababab
]
(8 columns written into each 4-column row; the third row is never written, so the terminal keeps what it had.)

Suggested fix: Mark Fill @PanicsAtCaller and panic unless text is exactly one grapheme cluster with DisplayWidth 1, or draw a wide one as two cells with Put(..., True, ...) every other column. Add a panic_term_fill_*.fb test.

Skeptic: I ran rv-term/fullscreen-v/s2.fb, debug and release. Both print `8 \e[1;1H中中中中\e[2;1Habababab`, and the frame reads [中中中中 / abababab]. Present writes 8 columns into each 4-column row, and the row filled with "" is never written. screen.fb:129-135 Fill hands any text to Put(column, row, text, False, look) with no check. The doc's own requirement ("a grapheme cluster that takes one") is never enforced, which breaks stdlib.md's Conventions: a bug panics at the caller's line. Fill quietly corrupts the cell grid ("two cells for a wide one") that Present's cursor tracking relies on, and the screen output goes wrong with no diagnostic.

## screen-3 (medium): At a panic or uncaught error, a Screen on stderr is put back on stdout, so the terminal stays on the alternate screen

Status: fixed in 877cf4e5 (term_tests: "what puts the screen back goes to the stream it was changed on", the probe's panic on stderr with its stdout going nowhere): fb_term_set_screen takes the stream the screen was changed on, which ConsoleTerminal gives, and what puts it back, at the end, a panic, an uncaught error, or Ctrl+Z, goes to that stream.

Where: runtime/src/term.cpp:277

Screen.Start writes its enter text through its Terminal, which is stderr for Terminal.Console(StandardStream.ERROR). That is how a picker draws its UI while stdout carries the answer. KeepScreen hands the leave text to fb_term_set_screen, but restore_terminal (at a panic, an uncaught error, or the end) and on_stop always write it with write_out, which goes to STD_OUTPUT_HANDLE / STDOUT_FILENO. When stdout is redirected, the terminal (stderr) is left on the alternate screen with the cursor hidden and bracketed paste on. The panic message is written into the alternate screen, and the redirected stdout gets escape codes appended. Run: fb run t7.fb > out.txt 2> err.txt.

```
Import std.core
Import std.term

// A picker draws on stderr, so stdout keeps only its answer.
Local screen = Screen(Terminal.Console(StandardStream.ERROR), False)
Print("answer")
Panic("boom")
```

Expected: stdout: "answer\n" only.
stderr: \e[?1049h\e[?25l\e[?2004h then \e[0m\e[?2004l\e[?25h\e[?1049l, then "panic: boom at t7.fb:7:1".

Actual: stdout: "answer\n\e[0m\e[?2004l\e[?25h\e[?1049l"
stderr: "\e[?1049h\e[?25l\e[?2004hpanic: boom at t7.fb:7:1" (the terminal is never put back)

Suggested fix: Have fb_term_set_screen take the stream the screen was entered on (the ConsoleTerminal's Stream), keep it next to the texts, and have write_out write the leave and enter texts to that handle or fd. Add a panic test with a Screen on StandardStream.ERROR whose stdout is checked separately.

Skeptic: I built rv-term/fullscreen-v/s3.fb (Screen(Terminal.Console(StandardStream.ERROR), False); Print("answer"); Panic("boom")) and ran it as ./s3.exe > out3.txt 2> err3.txt, debug and release. stdout: `answer\n\e[0m\e[?2004l\e[?25h\e[?1049l`. stderr: `\e[?1049h\e[?25l\e[?2004hpanic: boom at s3.fb:7:1`. The leave text goes to stdout, so stderr, the screen the Screen entered, never leaves the alternate screen, and the panic message lands on it. term.cpp:83-110 write_out always writes to STD_OUTPUT_HANDLE or STDOUT_FILENO, and restore_terminal (line 277) and on_stop use it. fb_term_set_screen (line 950) never records which stream the screen was entered on, while ConsoleTerminal.Write uses its Stream. This contradicts stdlib.md's promise that the console is put back before a panic's message, so that the message shows on the screen the program started on.

## screen-4 (medium): After Ctrl+Z and fg, the re-sent enter text hides the cursor, but the Screen still counts it as shown and never shows it again

Status: fixed in 1dc0c798 (term_screen's "resumed"): Track's RESIZE sets ShownRow to -1, as the enter text written again after Ctrl+Z hides the cursor, so the next Present shows it again with \e[?25h.

Where: std/term/screen.fb:441

On SIGCONT, on_stop writes entered(), which is Screen.Start's enter text and includes \e[?25l, then reports a resize. Track handles that RESIZE by resizing the Frame and setting Whole, but leaves ShownRow alone. When a TextInput's cursor was showing (ShownRow >= 0), the next Present moves the cursor but skips \e[?25h (`If ShownRow < 0 Then ...`). The cursor stays hidden until the program hides and shows it again. The program below does on a FakeTerminal exactly what the runtime does on SIGCONT: it writes the enter text, then sends a resize.

```
Import std.core
Import std.text
Import std.term

Function Shown(text: String): String => text.Replace("\u{1b}", "\\e")

// A text input's cursor shows; then Ctrl+Z and fg: on SIGCONT the runtime writes what enters
// the screen again (which hides the cursor), and reports a resize.
Local fake = FakeTerminal(6, 2, ColorLevel.NONE)
Local screen = Screen(fake, False)
fake.TakeOutput()
screen.Frame.Print(0, 0, "ab")
screen.ShowCursor(2, 0)
Print("before:", Shown(fake.TakeOutput()), screen.Present(), Shown(fake.TakeOutput()))
fake.Write("\u{1b}[?1049h\u{1b}[?25l\u{1b}[?2004h")
fake.Resize(6, 2)
Print("event:", screen.ReadEvent()?.Kind == InputKind.RESIZE)
screen.Present()
Print("after:", Shown(fake.TakeOutput()))
```

Expected: before:  12 \e[1;1Hab    \e[2;1H      \e[1;3H\e[?25h
event: True
after: \e[?1049h\e[?25l\e[?2004h\e[1;1Hab    \e[2;1H      \e[1;3H\e[?25h

Actual: before:  12 \e[1;1Hab    \e[2;1H      \e[1;3H\e[?25h
event: True
after: \e[?1049h\e[?25l\e[?2004h\e[1;1Hab    \e[2;1H      \e[1;3H

Suggested fix: In Track's RESIZE branch, set ShownRow = -1 next to Whole = True, so the next Present shows the cursor again when CursorRow >= 0. That matches the enter text, which hides it. Add a term_screen case that shows the cursor, resizes, and checks for \e[?25h.

Skeptic: I ran rv-term/fullscreen-v/s4.fb, debug and release. after: `\e[?1049h\e[?25l\e[?2004h\e[1;1Hab    \e[2;1H      \e[1;3H`, with no \e[?25h, although ShowCursor is still in effect. I checked the code to confirm the simulation matches the runtime. On SIGCONT, term.cpp:231-236 on_stop runs apply_mode, then write_out(entered().get(true)), which is Start's enter text and includes \e[?25l, then on_resize. on_stop is installed by watch_signals from fb_term_set_mode, and ISIG stays on unless ctrlCIsKey. screen.fb:441-446 Track's RESIZE branch sets Whole but leaves ShownRow >= 0, so Present (line 366) skips \e[?25h. After Ctrl+Z and fg, a TextInput's cursor stays hidden. POSIX only; I couldn't run that path on Windows, but the code and the FakeTerminal replay agree.

## widgets-1 (medium): ProgressBar's percent is one too low for many values (29/100 shows 28%)

Status: fixed in 6e01555a (term_widgets: 28, 29, 57, 58, and 100 of 100): ProgressBar works its percent out in Int64s, done * 100 / Total, or done / (Total / 100) past what that holds, so 29 of 100 is 29%.

Where: std/term/widgets.fb:511

Render works out the percent as (fraction * 100.0) As Int, with fraction = Current / Total as a Double. Floating-point error leaves values that should be whole just under them: 0.29 * 100 is 28.999999999999996, so 29 of 100 shows 28%, 57 shows 56%, and 58 shows 57%. The same goes for totals of 200 or 1000 (58/200, 290/1000, and so on). Debug and release builds agree. A note on the setup: build-rev2's fb.exe was built at 22:01, before the Char type (a0bdc631), and its ninja run (01:18:18.1) came just before rev-crypto was checked out (01:18:18.7). So it can't compile std at 2dc3d44f. Every result here comes from a fresh build of 2dc3d44f at C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-term/widgets/b/cli/fb.exe.

```
Import std.core
Import std.time
Import std.term

Function At(milliseconds: Int64): Instant => Instant.FromNanoseconds(milliseconds * 1000000)

Local fake = FakeTerminal(80, 10, ColorLevel.NONE, False)
Local bar = ProgressBar(100, fake, At(0))
bar.Parts = ProgressParts.PERCENT
For value In [28, 29, 57, 58]
    bar.Set(value, At(0))
    Print(value, "[" + bar.Render(At(0)) + "]")
End For
```

Expected: 28 [████████░░░░░░░░░░░░░░░░░░░░░░  28%]
29 [████████░░░░░░░░░░░░░░░░░░░░░░  29%]
57 [█████████████████░░░░░░░░░░░░░  57%]
58 [█████████████████░░░░░░░░░░░░░  58%]

Actual: 28 [████████░░░░░░░░░░░░░░░░░░░░░░  28%]
29 [████████░░░░░░░░░░░░░░░░░░░░░░  28%]
57 [█████████████████░░░░░░░░░░░░░  56%]
58 [█████████████████░░░░░░░░░░░░░  57%]

Suggested fix: Work the percent out in integers: Clamp(Current, 0, Total) * 100 / Total when Total <= Int64 max / 100, and the Double only past that (or Floor(fraction * 100.0 + 1e-9)). Add 29/100 and 57/100 to term_widgets.fb.

Skeptic: build-rev2's fb.exe is stale (std/core/format.fb fails to compile), so I used the fresh build of 2dc3d44f at rv-term/widgets/b/cli/fb.exe (its CMake source dir is rev-crypto). rv-term/refute-w/w1.fb printed '29 [...  28%]', '57 [... 56%]', '58 [... 57%]' in both debug and release. widgets.fb:511 does {((fraction * 100.0) As Int):>3}% with fraction = Current/Total as a Double, so 0.29*100 = 28.999... truncates to 28. A bar going up one at a time reads 28, 28, 30 and never shows 29%. The output is wrong but nothing crashes, so medium.

## widgets-2 (medium): A styled cell or box line that wraps is split by its escape codes' bytes, and its color runs into the borders

Status: fixed in 6e01555a (term_widgets' styled table cell and box): Table and Box wrap through style.fb's WrapStyled, which wraps as WordWrap does but by the columns text shows in, codes taking none, and ends a style that's on at a break with \e[0m and starts it again on the next line, so no border shows in it. Plain text wraps as before.

Where: std/term/widgets.fb:263

Table.RowLines and Box.Render check whether to wrap with VisibleWidth, which skips escape codes, but then wrap with String.WordWrap, which counts a code's bytes (like "[31m") as columns. So a styled word that fits is split partway through. The SGR that opens the style stays on the first wrapped line and its reset lands on the last, so the borders and padding on the lines between show in the cell's color. A column of Paint'ed cells (such as a red "failed") in a table with a MaxWidth, or a styled line in a Box with a Width, hits this.

```
Import std.core
Import std.text
Import std.term

Function Shown(text: String): String => text.Replace("\u{1b}", "\\e")

// A styled cell that has to wrap, in a table and in a box.
Local table = Table("Status")
table.AddRow("\u{1b}[31mfailed badly\u{1b}[0m")
table.MaxWidth = 12
Print(Shown(table.Render()))
Local box = Box("\u{1b}[1;31mvery bold red words\u{1b}[0m")
box.Width = 10
Print(Shown(box.Render()))
```

Expected: Words are wrapped by their visible columns ("failed" / "badly"; "very bold" / "red" / "words"), and each wrapped line opens and closes its own style, so no border is colored, e.g. "│ \e[31mfailed\e[0m   │" then "│ \e[31mbadly\e[0m    │".

Actual: ╭──────────╮
│ Status   │
├──────────┤
│ \e[31mfail     │
│ ed       │
│ badly\e[0m    │
╰──────────╯
╭──────────╮
│ \e[1;31mve       │
│ ry bold  │
│ red      │
│ words\e[0m    │
╰──────────╯

Suggested fix: Wrap by visible columns: give std.term a wrap that treats escape codes as zero-width (CodeEnd already finds them), and carry the active SGR codes across each break, ending each line with \e[0m and reopening them on the next. Use it in Table.RowLines and Box.Render, and add a styled cell that wraps to term_widgets.fb.

Skeptic: w2.fb gave the reported table ('│ \e[31mfail     │' / '│ ed       │' / '│ badly\e[0m    │') and box ('\e[1;31mve' / 'ry bold'...). A cell styled by Style.Apply wraps the same way ('\e[0;31mfa' / 'iled'). RowLines (widgets.fb ~263) and Box.Render check the width with VisibleWidth but wrap with String.WordWrap, which counts the escape bytes as columns. The opening SGR stays on the first line and the reset lands on the last, so the borders between show in the cell's color. This goes against the file's own rule that widths are columns as VisibleWidth counts them.

## widgets-3 (medium): Ask on a line too narrow for its question never shows what's typed

Status: fixed in 6e01555a (term_prompts' "narrow ask" and "long default"): Ask keeps a third of the line, from 2 to 20 columns, for what's typed and a column for the cursor, cutting the question, and the default's hint first, down to half of what's left for the question, so what's typed shows.

Where: std/term/prompts.fb:178

In live mode, AskWith cuts the question to Columns - 5 - VisibleWidth(hint) columns. That leaves room = Columns - 1 - VisibleWidth(prompt) = 1, a single column. LineOf scrolls so the cursor stays in that column (scroll = cursor - room + 1), so no typed grapheme is ever in view. Whenever the question has to be cut, the user types blind until ENTER; on 80 columns that means any question longer than about 75 characters, or a long default hint. The comment says the line "keeps a column for what's typed", but the cursor takes that column. term_prompts.out's "narrow ask" frames show the same thing ("? What i… (x) \e[K" while ab is typed).

```
Import std.core
Import std.text
Import std.term

Function Shown(text: String): String => text.Replace("\u{1b}", "\\e").Replace("\r", "\\r")

// A question too long for the line: Ask cuts it so the line doesn't wrap, but what's typed
// should still show as it's typed.
Local fake = FakeTerminal(30, 5, ColorLevel.NONE)
fake.Feed("hello\r")
Local answer = Ask(fake, "What is the name of the folder to back up?")
Print(answer ?? "Null")
Local frames = fake.Output.Split("\r?")
For frame In frames
    Print(Shown("?" + frame))
End For
```

Expected: The frames show what's been typed, scrolled as needed, e.g. "? What is the name… h", "… he", …, "… hello", with the question cut further to leave several columns for the answer.

Actual: hello
?
? What is the name of the … \e[K\r\e[28C
? What is the name of the … \e[K\r\e[28C
? What is the name of the … \e[K\r\e[28C
? What is the name of the … \e[K\r\e[28C
? What is the name of the … \e[K\r\e[28C
? What is the name of the … \e[K\r\e[28C
? What is the name of the … hello\e[K

Suggested fix: Cut the question so the typed text keeps a real share of the line, e.g. room = Max(Min(Columns / 3, 20), 2) columns plus the cursor's, and cut the hint as well when it doesn't fit. Add a frame check to term_prompts.fb's narrow ask showing the typed text.

Skeptic: w3.fb on 30 columns: every frame while 'hello' is typed is '? What is the name of the … \e[K\r\e[28C', with no typed text, and only the final line shows hello. On 80 columns with a 77-character question it is the same: no typed text until ENTER. prompts.fb:178 cuts the question to Columns-5-hint, leaving room = Columns-1-VisibleWidth(prompt) = 1 in LineOf (216). With scroll = cursor - room + 1, no grapheme ever shows, so the user types blind.

## widgets-4 (medium): A Spinner's line isn't cut to the terminal's width, so it wraps and can't be redrawn in place

Status: fixed in 6e01555a (term_widgets' spinner on 20 columns): Tick cuts its frame and message a column short of the edge, as ProgressBar does, so its line doesn't wrap and is drawn again in place.

Where: std/term/widgets.fb:648

Spinner.Tick writes "\r" + frame + " " + Message + "\e[K" with no cut. ProgressBar.Render cuts its line a column short of the edge "so the terminal doesn't wrap it", but Spinner doesn't. When the message is wider than the terminal, the line wraps. Each later "\r" only goes back to the start of the last physical row, and \e[K clears only that row, so every tick leaves another copy of the message on screen.

```
Import std.core
Import std.text
Import std.time
Import std.term

Function At(milliseconds: Int64): Instant => Instant.FromNanoseconds(milliseconds * 1000000)

// A spinner draws its line again in place with "\r", which only works while the line doesn't
// wrap; a progress bar cuts its line a column short of the edge, and a spinner should too.
Local fake = FakeTerminal(20, 5, ColorLevel.NONE)
Local spinner = Spinner("Downloading the second file of three", fake, At(0))
spinner.Tick(At(0))
spinner.Tick(At(80))
Local widest = 0
For piece In StripStyles(fake.Output).Split("\r")
    widest = Max(widest, VisibleWidth(piece))
End For

Print("columns 20, widest line drawn:", widest)
Local bar = ProgressBar(10, fake, At(0))
bar.Label = "Downloading the second file of three"
Print("a bar's line:", VisibleWidth(bar.Render(At(0))))
```

Expected: columns 20, widest line drawn: 19
a bar's line: 18

Actual: columns 20, widest line drawn: 38
a bar's line: 18

Suggested fix: In Tick, cut the message to Term.Columns - 1 - DisplayWidth(frame) - 1 with Truncated, as ProgressBar.Render does, and add a narrow-terminal spinner to term_widgets.fb.

Skeptic: w4.fb: on a 20-column FakeTerminal the spinner wrote '\r⠋ Downloading the second file of three\e[K\r⠙ ...', and the widest line drawn was 38. A ProgressBar with the same label renders 18 columns. Spinner.Tick (widgets.fb ~648) writes the frame and Message uncut, while ProgressBar.Render cuts its line a column short of the edge. A wrapped line can't be drawn again in place with \r and \e[K, which goes against stdlib.md's 'draw themselves again in place'.

## widgets-5 (medium): Ask's sideways scroll puts the cursor a column past the text when it cuts a wide character

Status: fixed in 6e01555a (term_prompts' "straddled ask", "中文字符" on 10 columns): LineOf draws a space for the half of a wide character that's in view, so what's after it and the cursor are where they'd be.

Where: std/term/prompts.fb:224

LineOf leaves out any grapheme that starts before scroll. When scroll falls inside a wide character, the next grapheme is drawn right after the prompt, but the cursor's column is still worked out as VisibleWidth(prompt) + cursor - scroll. The text shown and the cursor then disagree by a column: here "b" is at column 5 but the cursor goes to column 7, leaving a gap. From then on, edits seem to happen a column away from where they show. CJK input at the scroll edge hits this.

```
Import std.core
Import std.text
Import std.term

Function Shown(text: String): String => text.Replace("\u{1b}", "\\e").Replace("\r", "\\r")

// Ask's line scrolls sideways; where it scrolls to the middle of a wide character, the text
// shown and the cursor's column should still agree.
Local fake = FakeTerminal(9, 5, ColorLevel.NONE)
fake.Feed("a中b")
Print(Ask(fake, "Q?") ?? "Null")
Local frames = fake.Output.Split("\r?")
Print(Shown("?" + frames[frames.Count - 2]))
Print(Shown("?" + frames[frames.Count - 1]))
```

Expected: Null
? Q? 中\e[K\r\e[7C
? Q?  b\e[K\r\e[7C   (a space for the cut half of 中, so b is at column 6 and the cursor after it; or "? Q? b" with \e[6C)

Actual: Null
? Q? 中\e[K\r\e[7C
? Q? b\e[K\r\e[7C

Suggested fix: When a wide grapheme straddles scroll, draw a space for the half that's in view (or move scroll to the grapheme's end), so shown text and cursor use the same origin. TextInput in views.fb scrolls the same way and is worth checking.

Skeptic: w5.fb (9 columns, 'a中b') gave a last frame of '? Q? b\e[K\r\e[7C': b is drawn at column 5 but the cursor goes to column 7. w5b.fb on 10 columns with all-wide input '中文字符' (room 4, even) is off by one on every frame past the edge: '? Q? 文\e[K\r\e[8C', '? Q? 字\e[K\r\e[8C', '? Q? 符\e[K\r\e[8C', where the glyph ends at column 7. LineOf (prompts.fb 222-237) drops a grapheme that straddles scroll but still works out the cursor as VisibleWidth(prompt)+cursor-scroll. With an even room, CJK-only input always straddles.

## widgets-9 (medium): StripStyles takes quadratic time on long styled text

Status: fixed in 6e01555a (term_style's "long log", 400,000 styled words stripped in under 5 seconds): StripStyles keeps what it takes in a StringBuilder. It takes 136 ms in a debug build, and the old one 1 minute 53 seconds.

Where: std/term/style.fb:341

StripStyles builds its result with kept += text.Slice(...) for each run between codes, so each append copies everything kept so far. Doubling the input roughly quadruples the time (release build: 200 KB plain in 75 ms, 400 KB in 337 ms, 800 KB in 1486 ms). Stripping a captured colored log of a few MB takes about a minute. VisibleWidth goes through it as well.

```
Import std.core
Import std.text
Import std.time
Import std.term

// A long log, colored word by word: stripping its codes should take time in step with its length.
For count In [40000, 80000, 160000]
    Local builder = StringBuilder()
    For i = 0 Until count
        builder.Append("\u{1b}[31mword\u{1b}[0m ")
    End For

    Local text = builder.ToString()
    Local started = Instant.Now()
    Local plain = StripStyles(text)
    Local took = (Instant.Now().Nanoseconds - started.Nanoseconds) / 1000000
    Print(count, plain.Length, "{took} ms")
End For
```

Expected: Time roughly doubling with the input (linear), e.g. about 75, 150, and 300 ms.

Actual: (fb run --release) 40000 200000 75 ms
80000 400000 337 ms
160000 800000 1486 ms

Suggested fix: Collect the kept slices in a StringBuilder (std.text.builder) and return its ToString().

Skeptic: w9.fb --release: 40000 words, 200000 chars, 73 ms; 80000, 335 ms; 160000, 1433 ms, so about 4x per doubling. w9b.fb isolates the cause: the same number of String += appends took 30, 107, then 672 ms, while Find from an offset took 2, 4, then 8 ms (linear). So StripStyles' 'kept += text.Slice(...)' (style.fb:341) is quadratic. A public std function for stripping captured logs takes seconds to minutes on MB inputs.

## console-10 (low): The leak report prints while a leaked Screen still has the alternate screen, so it vanishes on a terminal

Status: fixed in 877cf4e5 (leak_term_screen): fb_finish calls run_before_report() before a leak report's first line, as fb_panic and fb_error_uncaught do, so a leaked Screen's leave text comes before the report.

Where: runtime/src/term.cpp:307

restore_terminal is registered with before_report only for panics and uncaught errors. fb_finish (object.cpp) prints its "leak:" lines to stderr and calls run_end only afterwards. When a Screen leaks (held in a cycle, so its Destroy never runs), the report is drawn on the alternate screen, and the "\e[?1049l" written after it hides the report. That happens exactly in the TUI programs that have a leak to find. With 2>&1 the order is visible: the escape codes that enter the screen, then the leak lines, then the codes that leave it.

```
Import std.core
Import std.term

// An app that holds its Screen, and a node that holds the app, in a cycle, so neither is released.
Class App
    Field Display: Screen
    Field Self_: Node? = Null

    New()
        Display = Screen()
    End New
End Class

Class Node
    Field Owner: App? = Null
End Class

Local app = App()
Local node = Node()
node.Owner = app
app.Self_ = node
Print("drawn")
Flush()
```

Expected: The screen-leaving codes (\e[0m...\e[?1049l) before the "leak:" lines, so the report shows on the screen the program started on.

Actual: \e[?1049h\e[?25l\e[?2004h\e[?1000h\e[?1002h\e[?1006hdrawn
leak: 1 InputParser object was never released
... leak: a cycle of 2 objects: App.Self_ -> Node.Owner -> the first App
\e[0m\e[?1006l\e[?1002l\e[?1000l\e[?2004l\e[?25h\e[?1049l

Suggested fix: Call fb::runtime::run_before_report() in fb_finish before the first leak line (when there is a leak), as fb_panic and fb_error_uncaught do.

Skeptic: `c10.exe 2>&1 | cat -v` printed "^[[?1049h...^[[?1006hdrawn", then all the leak: lines, including "leak: a cycle of 2 objects: App.Self_ -> Node.Owner -> the first App", and only then "^[[0m^[[?1006l...^[[?25h^[[?1049l". fb_finish (object.cpp:539-571) prints the report before run_end, and restore_terminal is hooked to before_report only for panics and uncaught errors. So on a terminal the report is drawn on the alternate screen and wiped when the program leaves it.

## console-11 (low): Alt+[ ("\e[" alone) comes as ESCAPE and '[', unlike Alt+O and every other Alt+key

Status: fixed in 8516dbd7 (term_input's "alt+[", and "alt cut short" now Alt+[): Csi gives Alt and [ for "\e[" with nothing after it, or before a byte that can't go on with a sequence, as Sequence does for "\eO".

Where: std/term/input.fb:382

When "\e[" is all there is and the parser flushes, Csi returns ESCAPE and leaves '[' as a key, while "\eO" alone is turned into Alt+O and "\e" plus any other character into Alt+key. Pressing Alt+[ therefore delivers ESCAPE, which prompts and Choose treat as cancel (Null), and then '['.

```
Import std.core
Import std.term

Const E = "\u{1b}"

Function Events(text: String): String
    Local parser = InputParser()
    parser.Feed(text)
    Local shown = ""
    Loop
        Local event = parser.Flush()
        If event == Null Then Break
        shown += "[{event}]"
    End Loop

    Return shown
End Function

// Alt and a key, each an escape and the key's character, sent alone.
Print("alt+a:", Events(E + "a"), "alt+O:", Events(E + "O"), "alt+[:", Events(E + "["))
```

Expected: alt+a: [key Alt+a] alt+O: [key Alt+O] alt+[: [key Alt+[]

Actual: alt+a: [key Alt+a] alt+O: [key Alt+O] alt+[: [key Escape][key []

Suggested fix: In Csi, when done and nothing follows the "[" (At + 2 == Bytes.Count), give Alt+'[' and advance by 2, as Sequence does for "\eO".

Skeptic: c11.fb printed "alt+a: [key Alt+a] alt+O: [key Alt+O] alt+[: [key Escape][key []". Csi's cut-short branch (input.fb:382-386) gives ESCAPE then '[', while Sequence turns a lone \eO into Alt+O. A real Alt+[ therefore delivers ESCAPE, which prompts treat as cancel. That is wrong for the key the user pressed, and inconsistent with the parser's own Alt handling.

## console-12 (low): SGR mouse reports for buttons 8 to 11 (back and forward, codes 128 to 131) are read as LEFT, MIDDLE or RIGHT presses

Status: fixed in 8516dbd7 (term_input's "other buttons", SGR's 128, 129, and 131, and X10's): a mouse report for buttons 8 to 11, its code's 128 bit set, is skipped, and MouseButton's doc says so.

Where: std/term/input.fb:555

MouseOf looks only at bits 0-1, 32 and 64. xterm encodes extra buttons 8 to 11 as 128 plus 0 to 3, so pressing a mouse's back button reads as PRESS LEFT and the forward button as PRESS MIDDLE. Apps then act on clicks the user never made.

```
Import std.core
Import std.term

Const E = "\u{1b}"

Function Events(text: String): String
    Local parser = InputParser()
    parser.Feed(text)
    Local shown = ""
    Loop
        Local event = parser.Flush()
        If event == Null Then Break
        shown += "[{event}]"
    End Loop

    Return shown
End Function

// xterm's SGR reports for buttons 8 and 9, a mouse's back and forward buttons: 128 and 129.
Print("back:", Events("{E}[<128;5;3M"), "forward:", Events("{E}[<129;5;3M"))
```

Expected: back: (skipped, or a non-LEFT button) forward: (skipped, or a non-MIDDLE button)

Actual: back: [mouse PRESS LEFT at 4, 2] forward: [mouse PRESS MIDDLE at 4, 2]

Suggested fix: In MouseOf (or in Csi before calling it), skip reports whose code has bit 128 set, or give them as MouseButton.NONE.

Skeptic: c12.fb printed "back: [mouse PRESS LEFT at 4, 2] forward: [mouse PRESS MIDDLE at 4, 2]" and "back release: [mouse RELEASE LEFT at 4, 2]". MouseOf (input.fb:548-571) ignores bit 128, which xterm and kitty use for buttons 8-11. A back or forward button press is reported as a left or middle click.

## console-13 (low): A "\n" typed after Enter in a later read (Ctrl+J, after a pause) is dropped as if it were part of "\r\n"

Status: fixed in 8516dbd7 (term_input's "returns", Enter and Ctrl+J a second apart): after a "\r", NextEvent waits ESCAPE_WAIT at most for a "\n" that's part of it, and a read that gets nothing ends the pairing (InputParser.Paused). FakeTerminal's Pause now leaves what's left of it after a shorter read, as time going by would.

Where: std/term/input.fb:263

AfterReturn stays set after a "\r" key no matter how long passes. The next "\n", whenever it comes, is taken as the second half of a CRLF line end and dropped. A terminal sends Enter as "\r" and Ctrl+J (and Ctrl+Enter on many terminals) as "\n", so Enter followed later by Ctrl+J loses the second key. The pairing is meant for line ends from pipes and files, which arrive without a pause between the two bytes.

```
Import std.core
Import std.time
Import std.term

// Enter, then a second later Ctrl+J, which a terminal sends as "\n", then x.
Local fake = FakeTerminal(80, 24)
fake.Feed("\r")
fake.Pause(1.Seconds)
fake.Feed("\n")
fake.Feed("x")
Loop
    Local event = fake.ReadEvent()
    If event == Null Then Break
    Print("{event}")
End Loop
```

Expected: key Enter
key Enter
key x

Actual: key Enter
key x

Suggested fix: Clear AfterReturn when a wait ends with nothing read (NextEvent's count == 0 path, or a Flush), so only a "\n" that comes right after the "\r" pairs with it.

Skeptic: c13.fb (Feed "\r", Pause(1s), Feed "\n", Feed "x") printed "key Enter / key x": the second Enter was lost. AfterReturn (input.fb:263-269) is never cleared by time or by a wait that read nothing. In raw mode, ICRNL is off on POSIX and Windows gives Ctrl+Enter as \n, so Enter followed later by Ctrl+J or Ctrl+Enter drops a key.

## console-14 (low): InputParser.Feed(bytes, count) panics inside input.fb for a count past the array, and accepts a negative count silently

Status: fixed in 8516dbd7 (panic_term_feed_count): Feed(bytes, count) is @PanicsAtCaller and panics for a count below 0 or past bytes.Count.

Where: std/term/input.fb:203

std's convention is that a bug such as a bad index or a negative size panics at the caller's line (@PanicsAtCaller). Feed indexes bytes[i] up to count without checking: a count larger than bytes.Count panics at std/term/input.fb:212 with an index error, and a negative count does nothing.

```
Import std.core
Import std.term

// Feeding more bytes than the array has.
Local parser = InputParser()
Local bytes: UInt8[] = [0x61, 0x62]
parser.Feed(bytes, -1)
Print("negative count fed:", parser.IsWaiting)
parser.Feed(bytes, 3)
Print("fed")
```

Expected: A panic at the caller's line for the negative count (and for 3), naming the count and the array's size.

Actual: negative count fed: False
panic: index 2 is out of range for 2 elements at std/term/input.fb:212:23

Suggested fix: Mark Feed(bytes, count) @PanicsAtCaller and panic when count < 0 or count > bytes.Count.

Skeptic: c14.fb printed "negative count fed: False", then "panic: index 2 is out of range for 2 elements at std/term/input.fb:212:23", the same in debug and release. That contradicts stdlib.md:203-205, where a bug such as a bad index or a negative size panics at the caller's line (@PanicsAtCaller). Feed(bytes, count) has no check: a negative count is silently accepted, and an oversized one panics inside std.

## console-6 (low): A UTF-8 lead byte without its continuation bytes swallows the bytes after it, such as the Enter after Latin-1 text

Status: fixed in 8516dbd7 (term_input's "cut short characters"): KeyAt checks the bytes after a lead byte as they come, and a character cut short by one that doesn't go on with it is U+FFFD at once, taking only its own bytes, so [0xE4, 0x61] gives U+FFFD and a, and "caf\xe9\n" its Enter.

Where: std/term/input.fb:332

KeyAt decides from the lead byte alone that it needs Utf8Length bytes. When fewer than that have arrived, it waits even if a byte already there is not a continuation byte. On Flush (at a timeout or the end of input) it returns `(Bytes.Count - at, U+FFFD)`, which consumes every remaining byte. So [0xE4, 0x61] gives a single U+FFFD and loses 'a', while [0xC3, 0x28] correctly gives U+FFFD and then '('. Piping Latin-1 text "café\n" (0x63 0x61 0x66 0xE9 0x0A) to ReadKey loses the Enter, because 0xE9 claims three bytes and only "\n" follows. Even when more bytes do come, the parser waits ESCAPE_WAIT first for no reason.

```
Import std.core
Import std.term

// Keys from stdin, a pipe of Latin-1 text, "café" and a line break, until the input ends.
// Run as: printf 'caf\xe9\n' | program
Loop
    Local key = ReadKey()
    If key == Null Then Break
    Print("key:", "{key}")
End Loop
```

Expected: key: c
key: a
key: f
key: ï¿½ (U+FFFD)
key: Enter

Actual: key: c
key: a
key: f
key: ï¿½ (U+FFFD)
(the Enter is lost; InputParser on [0xE4, 0x61] likewise gives only "|, key U+FFFD")

Suggested fix: In KeyAt, check the continuation bytes that are already there, and return (1, U+FFFD) as soon as one is not 0x80-0xBF. When flushing a truncated character, consume only the lead byte and the valid continuation bytes after it.

Skeptic: `printf 'caf\xe9\n' | c6.exe` gave c, a, f, U+FFFD and no Enter, in both debug and release. c6b.fb (InputParser) gave [0xE4,0x61] -> " |[key �]" ('a' lost), while [0xC3,0x28] -> "[key �][key (] |". KeyAt (input.fb:331-334) trusts the lead byte's length and on flush consumes Bytes.Count - at. At most 3 trailing bytes are lost, and only after an invalid lead byte near the end of input or before a pause, so low.

## console-7 (low): A resize during a paste cuts the paste short, and the rest of it comes as typed keys

Status: fixed in 8516dbd7 (term_input's "resized pasting"): a resize while a paste is coming is given at once, without flushing the paste, which goes on to its end mark: resize 100x40, then paste "abcdef".

Where: std/term/terminal.fb:261

When ReadInput returns -2 while the parser is waiting, NextEvent stores the resize and calls Parser.Flush(), even when the parser is in the middle of a paste. Flush ends the paste with the text so far. The rest of the paste then arrives as keys (and its end mark is skipped as an unknown sequence), so a TextInput or prompt gets the tail typed in, character by character. The design says a paste waits a second for its end mark; a resize is not a sign that the paste has ended.

```
Import std.core
Import std.term

Const E = "\u{1b}"

// The window is resized while a paste's text is still coming.
Local fake = FakeTerminal(80, 24)
fake.Feed("{E}[200~abc")
fake.Resize(100, 40)
fake.Feed("def{E}[201~")
Loop
    Local event = fake.ReadEvent()
    If event == Null Then Break
    Print("{event}")
End Loop
```

Expected: paste "abcdef" and resize 100x40 (in either order)

Actual: paste "abc"
resize 100x40
key d
key e
key f

Suggested fix: On -2 while Input.Parser.IsPasting, keep Input.Resized and go on reading (with the remaining PASTE_WAIT) instead of flushing. Return the resize once the paste is complete or its wait runs out.

Skeptic: c7.fb printed "paste \"abc\" / resize 100x40 / key d / key e / key f". NextEvent's -2 branch (terminal.fb:261-266) calls Parser.Flush() even while IsPasting, which ends the paste early, and its tail is then read as typed keys. That contradicts the design's "a paste waits a second for its end mark". A resize landing inside a paste's bytes is a narrow race, so low.

## console-8 (low): On Windows, TerminalSize() ignores stdin, unlike its docs and POSIX

Status: fixed in 877cf4e5 (term_tests: "the terminal's size is the console's, wherever stdout and stderr go", the probe in a pseudoconsole with its stdout and stderr on NUL): fb_term_size and the mouse's rows read the console's window through console_window(), which opens CONOUT$ when only stdin is the console's.

Where: runtime/src/term.cpp:719

console.fb says TerminalSize is "stdout's, or else stderr's or stdin's", and term.hpp says the size of the terminal that "stdout, stderr, or stdin is, the first that's one". POSIX tries STDIN_FILENO too. The Windows branch tries only streams 1 and 2, because GetConsoleScreenBufferInfo does not take an input handle. With stdout and stderr redirected and stdin a console, Windows gives Null and the console Terminal falls back to 80x24.

```
Import std.core
Import std.term

// Run from a 100x30 console as: program > out.txt 2>&1, then read out.txt.
Local size = TerminalSize()
Print(IsTerminal(StandardStream.INPUT), IsTerminal(StandardStream.OUTPUT), IsTerminal(StandardStream.ERROR))
Print(size == Null ? "Null" : "{size.0}x{size.1}")
```

Expected: True False False
100x30

Actual: True False False
Null

Suggested fix: On Windows, when stdin is a console, open CONOUT$ (CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)) and read its screen buffer info, or change the docs to say stdout or stderr only on Windows.

Skeptic: In ConPTY at 100x30, I ran `cmd /c c8.exe > c8out.txt 2>&1`. c8out.txt had "True False False / Null / console: 80 24". With stdout a console it gave "100x30 / console: 100 30". fb_term_size's Windows branch (term.cpp:719) tries only streams 1 and 2, while console.fb and term.hpp say stdin's too, and POSIX tries STDIN_FILENO.

## console-9 (low): On Windows, ConsoleColors(StandardStream.INPUT) reads stdin's echo bit as VT processing: NONE in raw mode, and it can turn echo on

Status: fixed in 877cf4e5 (the same probe's "input 1 1", fb_term_start(0) before raw mode and in it): fb_term_start gives 1 for a stdin that's a console, without testing or setting an output's flags on it.

Where: runtime/src/term.cpp:748

fb_term_colors(0) calls fb_term_start(0), which tests ENABLE_VIRTUAL_TERMINAL_PROCESSING (0x4) on the input handle. For an input handle, 0x4 is ENABLE_ECHO_INPUT. Outside raw mode echo is on, so stdin counts as a VT terminal. In raw mode echo is off, so it calls SetConsoleMode(stdin, mode | 0x4). Without line input that fails and gives NONE. With line input on but echo off (a password read by something else), it turns echo on. On POSIX, isatty(0) gives the same level both times. ConsoleColors and Paint take any StandardStream.

```
Import std.core
Import std.term

// Run in a console, stdin, stdout, and stderr all its own: stdin's colors, before and in raw mode.
Local console = Terminal.Console()
Print("terminal:", IsTerminal(StandardStream.INPUT), "before:", ConsoleColors(StandardStream.INPUT))
console.StartRawMode()
Print("raw:", ConsoleColors(StandardStream.INPUT))
console.EndRawMode()
Print("after:", ConsoleColors(StandardStream.INPUT))
```

Expected: (with TERM=xterm-256color) terminal: True before: INDEXED
raw: INDEXED
after: INDEXED

Actual: terminal: True before: INDEXED
raw: NONE
after: INDEXED

Suggested fix: In fb_term_start, for stream 0 return whether GetConsoleMode succeeds, without testing or setting output flags (or have fb_term_colors(0) use fb_term_is_terminal).

Skeptic: c9.fb in ConPTY (TERM=xterm-256color) printed "terminal: True before: INDEXED stdout: INDEXED", then "raw: NONE stdout: INDEXED", then "after: INDEXED". fb_term_start(0) tests and sets ENABLE_VIRTUAL_TERMINAL_PROCESSING (0x4) on the input handle, where 0x4 is ENABLE_ECHO_INPUT. In raw mode the SetConsoleMode fails, so the level is NONE.

## screen-5 (low): Negative sizes don't panic: Canvas(-3, 2), Resize(4, -1), and Constraint.Fixed/Min/Fill(-n) are quietly clamped

Status: fixed in 1dc0c798 (panic_term_canvas_size and panic_term_constraint): Canvas's New and Resize, and Constraint's Fixed, Min, and Fill, panic at the caller for a negative size, and Percent for one below 0 or past 100. term_screen's cases of those now use sizes that are allowed.

Where: std/term/screen.fb:36

docs/stdlib.md's Conventions list 'a negative size' as a bug that panics at the caller's line, and std does this elsewhere: Array.Resize panics with 'Resize takes a count that isn't negative'. Canvas.New and Canvas.Resize turn a negative size into 0 (their comment says 'or none for a negative size'). Constraint.Fixed, Min, and Fill turn negatives into 0, and Percent(150) becomes 100 (region.fb lines 99-113). None of these exceptions to the convention are in stdlib.md.

```
Import std.core
Import std.term

// std's conventions: a negative size is a bug, which panics at the caller.
Local canvas = Canvas(-3, 2)
Print(canvas.Columns, canvas.Rows)
canvas.Resize(4, -1)
Print(canvas.Columns, canvas.Rows)
Print("{Constraint.Fixed(-5)} {Constraint.Min(-2)} {Constraint.Fill(-1)} {Constraint.Percent(150)}")
```

Expected: A panic at line 5, like "Canvas takes a size that isn't negative, not -3x2".

Actual: 0 2
4 0
Fixed(0) Min(0) Fill(0) Percent(100)

Suggested fix: Mark Canvas.New, Canvas.Resize, and the Constraint factories @PanicsAtCaller and panic on a negative size (and on a Percent outside 0 to 100). Or, if clamping is wanted, write that exception into stdlib.md's std.term section. Update term_screen's Canvas(-1, -1) and Constraint cases to match.

Skeptic: I ran rv-term/fullscreen-v/s5.fb, debug and release: `0 2`, `4 0`, `Fixed(0) Min(0) Fill(0) Percent(100)`, with no panic. screen.fb:35-37 and 76-78 clamp with Max(columns, 0), and region.fb:99-113 clamp the Constraint factories. stdlib.md's Conventions say a bug, with "a negative size" as an example, panics at the caller's line (@PanicsAtCaller). The std.term section notes no exception, and none of the lane's listed decisions covers it. The clamping is deliberate, since term_screen.fb lines 136 and 147 test Canvas(-1, -1) and Fixed(-1), but it contradicts the documented convention.

## screen-6 (low): Region splits add Fill weights in an Int: large weights panic inside std in debug and give negative sizes in release

Status: fixed in 1dc0c798 (term_screen: Fill(2000000000) twice, and Fill of Int's most around a Min(1), on 10 columns): Sizes adds the weights in an Int64, and scales them down to 2^31 in all past that, so a share's arithmetic can't overflow: [(0, 0) 5x1, (5, 0) 5x1].

Where: std/term/region.fb:137

Sizes adds the weights in `Local weights = 0`, an Int (Int32), while shared and counted are Int64. Two Fill weights near 2^31 overflow the sum. A debug build panics at std/term/region.fb:146 rather than at the caller. A release build wraps, and the split gives negative widths and columns. That's a wrong result, and debug and release differ.

```
Import std.core
Import std.term

Local area = Region(0, 0, 10, 1)
Print("{area.SplitColumns(Constraint.Fill(2000000000), Constraint.Fill(2000000000))}")
```

Expected: [(0, 0) 5x1, (5, 0) 5x1] in debug and release.

Actual: debug: panic: integer overflow in '+' at std/term/region.fb:146:17
release: [(0, 0) -67x1, (-67, 0) -68x1]

Suggested fix: Keep weights as an Int64 (Local weights: Int64 = 0), and the per-constraint weight as an Int64 too, so shared * counted and the sum can't overflow.

Skeptic: I ran rv-term/fullscreen-v/s6.fb (Region(0,0,10,1).SplitColumns(Fill(2000000000), Fill(2000000000))). Debug: `panic: integer overflow in '+' at std/term/region.fb:146:17`. Release: `[(0, 0) -67x1, (-67, 0) -68x1]`. In region.fb:137, `Local weights = 0` is an Int, and Fill weights are summed into it while shared and counted are Int64. A valid but large weight panics inside std in debug and gives negative widths in release, so the two builds disagree.

## screen-7 (low): Canvas.Print at a column near Int's maximum overflows inside std: a debug panic in std, a release index panic

Status: fixed in 1dc0c798 (term_screen's columns near Int's most and least): PutText compares at > stop - width, which can't overflow, so a column past the edge draws nothing.

Where: std/term/screen.fb:204

PutText checks `at + width > stop`, which overflows when the column is near Int's maximum. A column past the right edge should just draw nothing. Instead, a debug build panics at std/term/screen.fb:204, and a release build wraps, passes the check, and panics with an out-of-range index in Put (screen.fb:223). Neither panic is at the caller, and the two builds differ.

```
Import std.core
Import std.term

Local canvas = Canvas(4, 2)
Print(canvas.Print(2147483647, 0, "x"))
```

Expected: 2147483647 (nothing drawn, since it's past the edge), in debug and release.

Actual: debug: panic: integer overflow in '+' at std/term/screen.fb:204:16
release: panic: index 2147483647 is out of range for 8 elements at std/term/screen.fb:223:12

Suggested fix: Compare without adding: `If at >= stop || width > stop - at Then Break` (stop is between 0 and Width here), or do the comparison in Int64.

Skeptic: I ran rv-term/fullscreen-v/s7.fb (Canvas(4, 2).Print(2147483647, 0, "x")). Debug: `panic: integer overflow in '+' at std/term/screen.fb:204:16`. Release: `panic: index 2147483647 is out of range for 8 elements at std/term/screen.fb:223:12`. The check `If at + width > stop Then Break` in PutText (screen.fb:204) overflows for a column near Int's max. In release it wraps, passes the check, and Put indexes out of range. Print is documented to draw "as far as the canvas goes", so a column past the edge should draw nothing and give the column. Neither panic is at the caller.

## widgets-10 (low): StripStyles leaves a DCS's or an APC's payload in (sixel, kitty graphics)

Status: fixed in 6e01555a (term_style's sixel, kitty, PM, and SOS): CodeEnd takes a DCS, an APC, a PM, and an SOS up to their ST or BEL, as it does an OSC.

Where: std/term/style.fb:389

StripStyles' comment says it removes "any other escape and the characters it takes". But CodeEnd ends only CSI and OSC at their terminators. ESC P (DCS), ESC _ (APC), ESC ^ (PM), and ESC X (SOS) run to an ST just as an OSC does, yet here they are treated as a two-byte escape. Their payload stays in the text and VisibleWidth counts it.

```
Import std.core
Import std.term

// A sixel image is a DCS (ESC P ... ESC \), and kitty's graphics an APC (ESC _ ... ESC \): each
// takes everything up to its ST, as an OSC does, so none of it shows.
Local sixel = "a\u{1b}Pq#0;2;0;0;0\u{1b}\\b"
Local kitty = "a\u{1b}_Gf=24;AAAA\u{1b}\\b"
Print("[" + StripStyles(sixel) + "]", VisibleWidth(sixel))
Print("[" + StripStyles(kitty) + "]", VisibleWidth(kitty))
```

Expected: [ab] 2
[ab] 2

Actual: [aq#0;2;0;0;0b] 13
[aGf=24;AAAAb] 12

Suggested fix: In CodeEnd, treat kinds 0x50 (P), 0x5F (_), 0x5E (^), and 0x58 (X) as OSC is treated, up to ESC \ (or BEL), and add these to term_style.fb.

Skeptic: w10.fb printed '[aq#0;2;0;0;0b] 13' and '[aGf=24;AAAAb] 12'. CodeEnd (style.fb 362-402) runs only CSI (0x5B) and OSC (0x5D) to their terminators. ESC P/_/^/X fall through to the two-byte escape path, so the DCS/APC payload stays in the text. That contradicts StripStyles' comment ('any other escape and the characters it takes').

## widgets-12 (low): stdlib.md offers Ask with a check on the console, but there's no such overload

Status: fixed in 6e01555a (term_console's "checked ask", reading "" then 7): prompts.fb has Ask(question, validate, default) on the console.

Where: docs/stdlib.md:826

stdlib.md says "`Ask(question, default)`, with a check or not, ... on the console or on a Terminal given". prompts.fb only has Ask(question, default), Ask(terminal, question, default), and Ask(terminal, question, validate, default), so a console Ask with a check doesn't compile.

```
Import std.core
Import std.term

// stdlib.md: "Ask(question, default), with a check or not, ... on the console or on a Terminal
// given", so a check should work on the console too.
Local age = Ask("Age?", Function(text: String) => text == "" ? "say a number" : Null)
Print(age ?? "Null")
```

Expected: Compiles; with "7" piped in, prints "? Age? 7" and then 7.

Actual: askcheck.fb:6:13: error: no overload of 'Ask' takes (String, Func(String): String?)

Suggested fix: Add Public Function Ask(question: String, validate: Func(String): String?, default: String = ""): String? that calls AskWith(Terminal.Console(), ...), or reword stdlib.md.

Skeptic: w12.fb: 'error: no overload of 'Ask' takes (String, Func(String): String?)'. prompts.fb has Ask(question, default), Ask(terminal, question, default), and Ask(terminal, question, validate, default) only. stdlib.md's std.term section says 'Ask(question, default), with a check or not, ... on the console or on a Terminal given', so the console overload with a check is documented but missing.

## widgets-13 (low): ConsoleColors' comment says any NO_COLOR turns colors off, but an empty one doesn't

Status: fixed in 6e01555a: ConsoleColors' comment says "NO_COLOR is set and isn't empty", and stdlib.md says so too; the runtime's own test already checks that an empty NO_COLOR is as if it weren't set.

Where: std/term/style.fb:313

The comment on ConsoleColors says NONE "where ... NO_COLOR is set to anything". runtime/src/term.cpp:690 (color_level) turns colors off only for a NO_COLOR that isn't empty, as decision 4 and no-color.org say, and the runtime's own test checks exactly that (tests/runtime/term_tests.cpp:347-348: "An empty NO_COLOR is as if it weren't set"). The comment is wrong, and stdlib.md's "NO_COLOR is set" is vague on the point. I couldn't open a console here to run the program (starting one was denied), so the actual result below comes from that code and its test.

```
Import std.core
Import std.text
Import std.term

// Run with stdout a terminal and NO_COLOR set to "": style.fb says NONE "where NO_COLOR is set
// to anything". The panic's message goes to stderr, which can be a file.
Panic("colors {ConsoleColors()}, stdout a terminal {IsTerminal()}, painted {Paint("x", Style(TermColor.Red)).Length} bytes")
```

Expected: Going by the comment, with NO_COLOR= and stdout a terminal: panic: colors NONE, stdout a terminal True, painted 1 bytes

Actual: Going by color_level and its test: on Windows' console, panic: colors TRUE_COLOR, stdout a terminal True, painted 12 bytes (BASIC with TERM=xterm elsewhere)

Suggested fix: Make the comment read "or NO_COLOR is set and isn't empty", and say the same in stdlib.md's std.term section.

Skeptic: style.fb:313 says NONE where 'NO_COLOR is set to anything'. runtime/src/term.cpp:690 turns colors off only for a NO_COLOR that is non-null and non-empty, and tests/runtime/term_tests.cpp:347-348 checks 'An empty NO_COLOR is as if it weren't set' (gives BASIC). Lane decision 4 also says not empty. The code plainly contradicts the comment, so it's a wrong comment. I couldn't run it with stdout as a real terminal here.

## widgets-14 (low): Prompts reading a line from a pipe drop its tabs

Status: fixed in 6e01555a (term_prompts' "piped tab"): ReadLine keeps a TAB with no modifiers as "\t".

Where: std/term/prompts.fb:150

Line-at-a-time prompts read through ReadLine, which keeps only keys whose IsText is true. The parser turns a 0x09 byte into KeyCode.TAB, which isn't IsText, so a tab in a piped line is silently dropped and Ask gives "namevalue" for "name\tvalue".

```
Import std.core
Import std.term

// From a pipe, Ask reads a line; a tab in it is part of the line.
Local piped = FakeTerminal(40, 8, ColorLevel.NONE, False)
piped.Feed("name\tvalue\n")
Print("[" + (Ask(piped, "Pair?") ?? "Null") + "]")
```

Expected: ? Pair? name	value
[name	value]

Actual: ? Pair? namevalue
[namevalue]

Suggested fix: In ReadLine, add "\t" for a KeyCode.TAB key with no modifiers, and add a piped tab to term_prompts.fb.

Skeptic: w14.fb: a non-interactive FakeTerminal fed 'name\tvalue\n' gave Ask '[namevalue]', and it echoed '? Pair? namevalue'. ReadLine (prompts.fb 127-158) appends only keys where IsText is true, and a 0x09 byte parses to KeyCode.TAB, so piped tabs are silently dropped. That loses data from a line-at-a-time read.

## widgets-6 (low): Choose doesn't cut its filter, so the question line wraps and the redraw goes wrong

Status: fixed in 6e01555a (term_prompts' "long filter", the widest line 15 on 16 columns): Choose shows a filter too long for the line by its end, after a "…", and cuts the question to what's left.

Where: std/term/prompts.fb:281

ChooseOn says each line is "cut to the terminal's width, so none wraps". The question is cut to columns - 5 - DisplayWidth(filter), but the filter itself is appended in full. Once the filter is longer than about Columns - 6 columns, the line is wider than the terminal and wraps. Redrawn then moves up only drawn - 1 rows, so the old lines are left on screen.

```
Import std.core
Import std.text
Import std.term

Function Shown(text: String): String => text.Replace("\u{1b}", "\\e").Replace("\r", "\\r")

// Choose cuts each line it draws to the terminal's width, a column short of it, so none wraps
// and it can draw them again in place; the filter typed on the question's line should be cut too.
Local fake = FakeTerminal(16, 6, ColorLevel.NONE)
fake.Feed("abcdefghijklmnop\u{1b}")
Print(Choose(fake, "Pick?", ["one", "two"]) ?? -1)
// Each line drawn, between the line breaks and the moves back up to draw them again.
Local drawn = (Try? fake.Output.ReplaceAll(r"\x1b\[\d*F", "\n")) ?? ""
Local widest = 0
For line In StripStyles(drawn.Replace("\r", "")).Split("\n")
    widest = Max(widest, VisibleWidth(line))
End For

Print("columns 16, widest line drawn:", widest)
Local lines = fake.Output.Split("\u{1b}[1F")
Print(Shown(lines[lines.Count - 2]))
```

Expected: -1
columns 16, widest line drawn: 15
(the question line shows the filter's tail, e.g. "?  › …jklmnop")

Actual: -1
columns 16, widest line drawn: 21
\e[J?  › abcdefghijklmnop\r
  nothing matc…

Suggested fix: Fit the filter into what the line has left, showing its end with a leading "…" (as Ask scrolls), so "? " + head + " › " + filter is never wider than columns. Add a long filter to term_prompts.fb's narrow test.

Skeptic: w6.fb on 16 columns: the question line became '?  › abcdefghijklmnop', 21 columns, the widest line drawn. Redrawn then moves up only \e[1F, so a wrapped line's top row is left behind. prompts.fb:280-281 cuts the question to make room for the filter but appends the filter in full, which contradicts the comment 'each line cut to the terminal's width, so none wraps'. Low, because it needs a filter about as long as the terminal is wide (74+ characters on 80 columns).

## widgets-7 (low): A Box whose inside is one column wide breaks its borders for wide characters

Status: fixed in 6e01555a (term_widgets' narrow boxes of 中文 and 中): Box's inside is at least as wide as its content's widest grapheme cluster, up to 2, so each of its lines is as wide as the others.

Where: std/term/widgets.fb:327

Box.Render lets inner shrink to 1 column (Max(Width - 2 * Padding, 1)). A width-2 character can't fit, so WordWrap leaves it whole and Aligned returns it as it is. Those lines come out a column wider than the top and bottom. Table guards against this by keeping each column at least as wide as its widest grapheme cluster (least), but Box doesn't.

```
Import std.core
Import std.text
Import std.term

// A box narrower than a wide character: each line should still be as wide as the others.
Local box = Box("中文")
box.Width = 3
Local rendered = box.Render()
Print(rendered)
For line In rendered.Split("\n")
    Print(VisibleWidth(line))
End For

// Padding 0 and a width of 1.
Local tight = Box("中")
tight.Padding = 0
tight.Width = 1
Print(tight.Render())
```

Expected: Every line of each box is the same width (for example the inside grows to 2 columns: 6, 6, 6, 6), and the tight box's top matches "│中│".

Actual: ╭───╮
│ 中 │
│ 文 │
╰───╯
5
6
6
5
╭─╮
│中│
╰─╯

Suggested fix: Raise inner to at least the content's widest grapheme cluster (capped at 2 columns, as Table's least is) before wrapping, and add a test.

Skeptic: w7.fb: Box('中文') with Width 3 renders '╭───╮' / '│ 中 │' / '│ 文 │' / '╰───╯' with line widths 5, 6, 6, 5. With Padding 0 and Width 1 it renders '╭─╮' / '│中│'. Box.Render sets inner = Max(Width - 2*pad, 1), and a width-2 grapheme can't wrap into one column. Table guards this with 'least', Box doesn't. An ASCII word in a narrow box stays aligned (8, 8, 8, 8, 8). Low, because it only happens when the inside is 1 column wide.

## widgets-8 (low): ProgressBar.Render panics when the ETA is too large for an Int64

Status: fixed in 6e01555a (term_widgets' bar of Int64's most, 1 done in 10 seconds): TimeText gives "-:--" from 10,000 hours, and for what isn't a number, so the ETA can't overflow an Int64.

Where: std/term/widgets.fb:690

With ETA in Parts (the default), Render works out left = elapsed * (Total - Current) / Current as a Double, and TimeText converts it with seconds As Int64. Per the spec, a float As an integer that doesn't fit panics in every build. A very large Total with little done (here Int64's max, with 1 done after 10 seconds) makes the widget panic while drawing. Decision 2 says widgets can't fail. Debug and release both panic.

```
Import std.core
Import std.time
Import std.term

Function At(milliseconds: Int64): Instant => Instant.FromNanoseconds(milliseconds * 1000000)

// A bar of a huge total, one done after ten seconds: its ETA is far off, but it shouldn't panic.
Local fake = FakeTerminal(80, 10, ColorLevel.NONE, False)
Local bar = ProgressBar(9223372036854775807, fake, At(0))
bar.Set(1, At(10000))
Print(bar.Render(At(10000)))
```

Expected: A line such as "░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░   0% 1/9223372036854775807 ETA -:--" (or a capped ETA), and no panic.

Actual: panic: the value doesn't fit in Int64 at std/term/widgets.fb:690:19
error: eta.exe exited with code 101, which a panic gives

Suggested fix: Cap the time before converting: in TimeText (or before calling it), show anything that's not finite or is past some limit (say 9999 hours) as "-:--" or ">9999:59:59", and add the case to term_widgets.fb.

Skeptic: w8.fb: ProgressBar(Int64 max) with 1 done at 10 s panics in both debug and release: 'panic: the value doesn't fit in Int64 at std/term/widgets.fb:690:19' (TimeText's seconds As Int64). Render guards Current > 0 and elapsed > 0 but doesn't cap the ETA. Widgets aren't meant to fail, so this is a real crash, but it needs an ETA past about 9.2e18 seconds (a total near 1e18 or a sentinel total), so low.

## Refuted by the skeptics (no action)

- widgets-11: Style's ToString joins several attributes with " | ". w11.fb does print 'bold | underline red on blue'. But it's the lane's chosen and tested format: tests/programs/term_style.out line 17 expects 'bold | dim | italic | underline | blink | reverse | hidden | strikethrough' for an all-attributes Style. The doc comment's 'Like "bold red on blue"' example has a single attribute, and stdlib.md doesn't say what ToString gives. It's cosmetic, intended, and contradicts no doc, so refuted.
