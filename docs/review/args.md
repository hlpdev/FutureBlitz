# The review of M6 step 17's Args.Parse<T> (std.os) and For Field over a T? (compiler)

11 confirmed findings: 1 high, 2 medium, 8 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## args-1 (high): Every Parse runs T's OnLoaded on an empty command line, for the help's defaults, so a valid command line can panic

Status: fixed in b631c471 and cb7f662b: the built-in Defaults<T>() (the user's call) gives Save's form of a T's Fields at their defaults, or zeros, leaving out a Field with neither, made without New, OnLoaded, or a Class's Destroy; a T? is its T, and a type without Fields of its own gives its zero's Value, or a Null, so ParserOf's copies for every Field can ask it (spec 13). Args reads the help's defaults from Defaults<T>(), so OnLoaded runs only on the T a command line gives. sema_tests' "Defaults<T> makes a Value of what a T is when nothing sets it", mir_tests' "lowering reports an instance's type that Defaults<T> can't write, and runs no OnLoaded", tests/programs/load_defaults.fb, and tests/programs/os_args.fb's Sized, whose OnLoaded divides by its Path's length and prints once

Where: std/os/args.fb:643

ParserOf<T> always calls DefaultsOf<T>(parser.Nothing()), which is Load<T> of an empty Object, and Load runs OnLoaded. This happens inside every Args.Parse<T>(...), not only for --help. So OnLoaded runs once on a T the user never gave (zeros and defaults), then again on the real one. Any side effect runs twice. An OnLoaded that assumes a required argument is present (divides by its length, Expects a value, opens the file) panics before the real command line is even read. DefaultsOf only catches ParseError. Separately, t2-style output shows 'OnLoaded ran, level 3' before 'level 5'.

```
Import std.core
Import std.os
Import std.text

Struct Opts
    @Positional(Help: "The file")
    Field Path: String
    Field Size: Int

    ByRef Method OnLoaded()
        Size = 100 / Path.Length
    End Method
End Struct

Local o = Try Args.Parse<Opts>(["abcd"])
Print(o.Path, o.Size)
```

Expected: abcd 25

Actual: panic: division by zero at t6.fb:11:16 (exit 101). With an OnLoaded that prints, it prints twice per Parse, first with the defaults (debug and release alike).

Suggested fix: Work out the defaults only when the help is made (in Help(), or when an ArgsHelp is about to be thrown), not in ParserOf. Better still, get them without running OnLoaded, for example through a Load that skips OnLoaded, or by reading the Fields' declared defaults.

Skeptic: Ran the finding's program (a1.fb). Debug and release both print 'panic: division by zero at a1.fb:11:16' and exit 101, though the command line ["abcd"] is valid. A second program (Field Level: Int = 3, an OnLoaded that prints, Parse(["--level","5"])) prints 'OnLoaded ran, level 3' and then 'OnLoaded ran, level 5' in debug and release. The cause is in std/os/args.fb: ParserOf<T> always calls DefaultsOf<T>(parser.Nothing()) (line 643), which is Save(Load<T>(empty)), and Load runs OnLoaded. DefaultsOf catches only ParseError, so a panic from OnLoaded escapes. Every Parse<T>, Parser<T> and Help<T> goes through ParserOf, so each one runs the user's OnLoaded on a T built from defaults. Spec 13 says OnLoaded checks what was loaded.

## args-2 (medium): A non-ASCII letter in a short-option group panics in Slice

Status: fixed in be880b85: Parse steps through a short group a character at a time, by CharAt(at).Utf8Length, so '-é' and '-vé' are an unknown option '-é'. tests/programs/os_args.fb's -é, -vé, and '-v é' cases

Where: std/os/args.fb:221

Parse walks a '-abc' group with word.Slice(at, at + 1) and at += 1, which treats byte offsets as characters. String.Slice panics unless both offsets fall between characters. So any short group with a multi-byte character ('-é', '-vé', or a typo on a non-US keyboard) crashes the program. It should report an unknown option.

```
Import std.core
Import std.os

Struct Opts
    @Flag(Short: "v")
    Field Verbose: Bool
    @Positional(Optional: True)
    Field Name: String
End Struct

For arguments In [["-é"], ["-vé"]]
    Try
        Local o = Try Args.Parse<Opts>(arguments)
        Print("ok", o.Verbose, o.Name)
    Catch e As ArgsError
        Print("{e.Kind} {e.Message}")
    End Try
End For
```

Expected: UNKNOWN unknown option '-é'
UNKNOWN unknown option '-é'

Actual: panic: Slice takes a position between characters, not 2 at std/os/args.fb:221:36 (exit 101)

Suggested fix: Step through the group by characters, not bytes: iterate word.Chars, or move `at` to the next character boundary. Keep the option-value slice at a character boundary too.

Skeptic: Ran a2.fb with ["-é"] and ["-vé"]. Debug and release both print 'panic: Slice takes a position between characters, not 2 at std/os/args.fb:221:36' (exit 101), where an ArgsError was expected. Parse steps through the short group with word.Slice(at, at + 1) and at += 1, which are byte offsets, so a multi-byte character splits. One mistyped argument from the user crashes the program.

## compiler-1 (medium): Where lines on a For Field loop over a type parameter's T? aren't checked at the call, and the compiler crashes or emits invalid IR

Status: fixed in ba78a218: collect_field_bounds() peels the T?s off a loop's type before matching it to the function's own type parameters, so `In T?` and `In T??` put their Where lines on T's fields at each call. sema_tests' "For Field over a T? goes over its T's fields" (Twos<P> and Twos<Q> over T? and T??), and tests/programs/reflection_nullable.fb's Twos<Point>

Where: compiler/src/sema.cpp:10320

collect_field_bounds only treats a loop as constraining a function's own type parameter when the loop's type is a bare named type (`In T`). `In T?` (or `T??`) is a nullable type node, so the loop's Where lines never become call-site bounds on T. Before this change that was harmless, since a T? had no fields and the body was never copied; now lowering copies the body for each of T's fields, so a call whose T has a field that doesn't meet the Where line slips past sema and reaches lowering/codegen with F bound to that field's type. With `In T` the same program gets the proper error ("'Twos' needs each field of Q to implement Numeric, but its field 'Name' is String, which doesn't"). The sema test for this feature only covers loops over `T` called with `Q?` and loops over a concrete `Q?`, not a loop written over `T?`, so it misses this. Variant: with only `Where F: Numeric` and `Local z = F.Zero() + F.Zero()` in the body, fb prints `error: invalid LLVM IR: Intrinsic has incorrect return type! ptr @llvm.uadd.with.overflow.p0`.

```
Import std.core

Struct P
    Field X: Int
End Struct

Struct Q
    Field Name: String
End Struct

Function Twos<T>(): String
    Local text = ""
    For Field f: F In T?
        Where F: Numeric
        Where F: Printable
        text += "{f.Name}={F.One() + F.One()} "
    End For
    Return text
End Function

Print(Twos<P>())
Print(Twos<Q>())
```

Expected: A compile error at Twos<Q>: 'Twos' needs each field of Q to implement Numeric, but its field 'Name' is String, which doesn't (the same error `For Field f: F In T` gives).

Actual: fb.exe crashes while compiling: "Segmentation fault", exit 139. Without the Twos<Q> line it builds and prints "X=2".

Suggested fix: In collect_field_bounds, peel any nullable wrappers off the loop's type node (or resolve the loop type and strip all ?s with all_non_null) before matching it against the function's own type parameters, so `In T?` and `In T??` add the same field bounds as `In T`. Add a sema test with a loop over `T?` whose call's T has a field that fails the Where line.

Skeptic: I put the programs under scratchpad/rv-args/forfield-nn. c1.fb is the finding's program, with the loop over `In T?`. In debug, fb.exe segfaults while compiling ("Segmentation fault", exit 139). In release it builds and prints "X=2 " then "Name= ", so Twos<Q> is accepted when it should be refused. c1b.fb is the same program with `In T`; it gives the right error at 22:7: "'Twos' needs each field of Q to implement Numeric, but its field 'Name' is String, which doesn't". c1c.fb is the variant (Where F: Numeric, `Local z = F.Zero() + F.Zero()`, called with Zs<Q>()). In debug it fails with "error: invalid LLVM IR: Intrinsic has incorrect return type! ptr @llvm.uadd.with.overflow.p0". In release the compiler segfaults (exit 139) after printing "c1c.exe exited with code 0xC0000005". The cause is in rev-math compiler/src/sema.cpp, collect_field_bounds, lines 10317-10322: it takes `std::get_if<named_type_t>` of loop->type, and for `T?` the node is a nullable type, so `parameter` is own.end() and the loop's Where lines are never added as call-site bounds. Meanwhile lowering copies the body for each of T's fields.

## args-10 (low): No tests for ArgsParser's documented panics

Status: fixed in f06b747a: tests/programs/panic_os_args_*.fb, one for each of ArgsParser's documented panics (key, long, short, short_name, nameless, after_rest, required_after, positional_commands, command_positionals, command_name), each at its caller's line

Where: tests/programs/os_args.fb:122

The doc comments for Flag, Option, Positional, and Command list panics: a repeated key, long name, or short name; a short name that isn't one ASCII letter or digit; a positional after a rest one; a required positional after an optional one; positionals beside commands; a repeated command name. No test anywhere covers any of them; os_args.fb is the only file that uses ArgsParser. They do fire, as this program shows, but nothing pins them or their @PanicsAtCaller locations.

```
Import std.core
Import std.os

Local p = ArgsParser("tool").Flag("A", "all", "a").Flag("B", "both", "a")
Print(p.Usage())
```

Expected: A test (panic program or expected-panic output) for each documented ArgsParser panic

Actual: panic: an ArgsParser has one option named '-a' at t11.fb:4:11, which no test checks

Suggested fix: Add panic tests, in whatever form the repo uses for expected panics, one for each documented ArgsParser panic, checking the message and that it's reported at the caller's line.

Skeptic: Ran a10.fb. It prints "panic: an ArgsParser has one option named '-a' at a10.fb:4:11", so the panic fires at the caller. grep finds no test anywhere for the documented ArgsParser panics: 'has one option named', 'takes positional arguments or commands', 'a short name is one ASCII letter', 'comes after', and 'has one command named'. Only tests/programs/os_args.fb uses ArgsParser, and it covers none of them. The repo pins documented panics with tests/programs/panic_*.fb (for example panic_algo_binomial_negative.fb), so these documented panics are missing that coverage.

## args-4 (low): The help lists '-h, --help' and '-V, --version' even when an option has taken -h or -V

Status: fixed in be880b85: Help() builds the --help and --version rows with Builtin(), from the names no option has, and leaves a row out when options have both. tests/programs/os_args.fb's Sizes (an -h flag) and taken (options with --help and --version)

Where: std/os/args.fb:297

Asked() correctly lets an option own -h, --help, -V, or --version (it checks Has). But Help() always adds the rows '-h, --help  Prints this help' and '-V, --version'. So the help shows -h twice, and the second row is wrong.

```
Import std.core
Import std.os

@Program(Name: "app")
Struct Opts
    @Flag(Help: "Prints sizes for people", Short: "h")
    Field Human: Bool
End Struct

Print(Args.Help<Opts>())
Print((Try Args.Parse<Opts>(["-h"])).Human)
```

Expected: Usage: app [options]

Options:
  -h, --human  Prints sizes for people
      --help   Prints this help
True

Actual: Usage: app [options]

Options:
  -h, --human  Prints sizes for people
  -h, --help   Prints this help
True

Suggested fix: In Help(), build the help and version rows from what Asked accepts: leave out the short or long form that Has(...) says an option owns, and drop the row entirely when both are taken.

Skeptic: Ran a4.fb. The help prints '-h, --human  Prints sizes for people' and then '-h, --help   Prints this help', and Parse(["-h"]) gives Human True. So the help lists -h twice, and the second row is false. Help() (line 297) always adds ('-h, --help', ...) and the -V row without checking Has(), which Asked() does check.

## args-5 (low): '--=x' reaches a flag or option with no long name; ArgsParser accepts one with neither name

Status: fixed in be880b85: Named() matches no spec for an empty long name, so '--=x' is an unknown option '--', and Add() panics for a flag or option with neither name. tests/programs/os_args.fb's short parser and tests/programs/panic_os_args_nameless.fb

Where: std/os/args.fb:357

Named(long) matches spec.Long == long. For '--=x', long is "", which matches any flag or option built with only a short name, so the error names '--'. Separately, Add accepts a Flag or Option with neither long nor short (e.g. Flag("Verbose")). The command line can't give it, its Shown() is '-', and its help row is blank.

```
Import std.core
Import std.os

Local p = ArgsParser("tool").Flag("Verbose", short: "v").Option("Out", short: "o")
Try
    Print(Try p.Parse(["--=x"]))
Catch e As ArgsError
    Print("{e.Kind} {e.Message}")
End Try
```

Expected: UNKNOWN unknown option '--'

Actual: INVALID '--' takes no value (the short-only -v flag was matched)

Suggested fix: In Named, skip specs whose Long is "" (or reject an empty name up front). In Add, panic for a flag or option with neither a long nor a short name.

Skeptic: Ran a5.fb. ArgsParser("tool").Flag("Verbose", short: "v").Option("Out", short: "o") parsing ["--=x"] gives "INVALID '--' takes no value". Named("") matched the short-only flag because its Long is "" (line 357). ArgsParser("tool").Flag("Verbose"), with no long or short name, is also accepted: Help() prints a blank row ('    ') under Options, and nothing on the command line can set it. Add() does not check for that.

## args-6 (low): ArgsParser.Command's optional: the last call decides for every command

Status: fixed in be880b85: CommandRequired = CommandRequired || !optional, so a command is required when any Command says so, which its comment now says. tests/programs/os_args.fb's two orders of a required and an optional command

Where: std/os/args.fb:189

Command sets CommandRequired = !optional on each call, so whichever Command was added last decides whether a command is required. Here a required command is added first and an optional one second, and an empty command line is accepted with usage '[command]'. Reversed, the optional one is ignored. The doc says 'One of the commands has to be given, unless optional', a per-call flag, which reads as if it should combine (required if any call says so), or be a parser-level setting.

```
Import std.core
Import std.os

Local a = ArgsParser("a")
Local b = ArgsParser("b")
Local p = ArgsParser("tool").Command("a", "A", a, optional: False).Command("b", "B", b, optional: True)
Print(p.Usage())
Try
    Print(Try p.Parse([]))
Catch e As ArgsError
    Print("error:", e.Message)
End Try
```

Expected: Usage: tool <command>
error: missing a command: a, b

Actual: Usage: tool [command]
{}

Suggested fix: Make it CommandRequired = CommandRequired || !optional (or panic when calls disagree), or move the setting to the parser, and say which in the doc comment.

Skeptic: Ran a6.fb. Command("a", optional: False) followed by Command("b", optional: True) prints 'Usage: tool [command]', and Parse([]) returns '{}' with no error. Line 189, CommandRequired = !optional, lets the last call override the earlier ones. That contradicts the per-call doc, 'One of the commands has to be given, unless optional', since the first call asked for a required command.

## args-7 (low): Type checks on TypeName miss T? wrappers: @Flag on an Int? doesn't count, and a String[]? option isn't repeatable

Status: fixed in be880b85: ParserOf reads a Field's type under its T?s, so an Int? @Flag counts and a String[]? option takes each; ArgSpec's Fills, which ParserOf's Unfilled() clears for a T[]?, keeps one that isn't given out, so it's Null. tests/programs/os_args.fb's Maybe

Where: std/os/args.fb:630

ParserOf checks types by name: many = type.EndsWith("[]"), counts = IsInteger(type), and the Bool test is type == "Bool". So an Int? @Flag becomes a plain Bool flag, and Load then fails with 'expected Int32, found true'. A String[]? @Option takes only one value and refuses a second '--tag'. The attribute docs say 'an integer one counts' and 'One that's a T[] takes each time it's given'.

```
Import std.core
Import std.os

@Program(Name: "t")
Struct Opts
    @Flag(Help: "More output", Short: "v")
    Field Verbose: Int?
    @Option(Help: "Tags to add")
    Field Tag: String[]?
End Struct

For arguments In [["-vv"], ["--tag", "a", "--tag", "b"]]
    Try
        Local o = Try Args.Parse<Opts>(arguments)
        Print("ok", o.Verbose ?? -1, o.Tag ?? ["none"])
    Catch e As ArgsError
        Print("{e.Kind} {e.Message}")
    End Try
End For
```

Expected: ok 2 [none]
ok -1 [a, b]

Actual: INVALID --verbose: expected Int32, found true
INVALID '--tag' is given more than once

Suggested fix: Strip any trailing '?' from the type name before the [] and integer and Bool checks. Or panic at ParserOf for a @Flag on a type that's neither Bool nor an integer, so it fails at once, not when a user gives it.

Skeptic: Ran a7.fb. An @Flag on an Int? Field with -vv gives 'INVALID --verbose: expected Int32, found true': ParserOf made a Bool flag because IsInteger("Int32?") is false, and Load fails only when the user gives it. A String[]? @Option with two --tag gives "INVALID '--tag' is given more than once" because many = type.EndsWith("[]") misses "String[]?". The same function already strips ? for `optional` (type.EndsWith("?")). So the T? case is half-handled, and an author error turns into a confusing message the user sees at run time.

## args-8 (low): A subcommand's help leaves out what it does: @Program on its Struct is ignored and @Subcommand's Help isn't used

Status: fixed in 689a5ba0 and be880b85: T.Attribute and T.Has on a T? read its T's declaration (spec 13 says so, as For Field over a T? does), so a subcommand's @Program is read, and ParserOf gives a command's parser its @Subcommand's Help where its @Program has no About. tests/programs/reflection_nullable.fb's Noted<Build?> and Noted<Top?>, and tests/programs/os_args.fb's Remotes

Where: std/os/args.fb:605

ParserOf<F> for a subcommand gets F as the T? type, and T.Attribute(Program) on a T? comes back Null. So @Program(About: ...) on the subcommand's Struct is dropped. The ArgsParser built for it also isn't given the @Subcommand's Help as its About. 'git remote --help' therefore prints no line saying what the command does, though the top-level help does.

```
Import std.core
Import std.os

Struct AddArgs
    @Positional(Help: "Its name")
    Field Name: String
End Struct

@Program(About: "Manages remotes.")
Struct RemoteArgs
    @Subcommand(Help: "Adds one")
    Field Add: AddArgs?
End Struct

@Program(Name: "git")
Struct Git
    @Subcommand(Help: "Remotes")
    Field Remote: RemoteArgs?
End Struct

Try
    Try Args.Parse<Git>(["remote", "--help"])
Catch e As ArgumentError
    Print(e.Message)
End Try
```

Expected: Manages remotes.

Usage: git remote [command]
...

Actual: Usage: git remote [command]

Options:
  -h, --help  Prints this help

Commands:
  add         Adds one

Suggested fix: In ParserOf, when there's no About, use the subcommand's Help (pass it in next to `command`). Read @Program from the unwrapped T of a T? as well.

Skeptic: Ran a8.fb. 'git remote --help' prints 'Usage: git remote [command]' and the options and commands, but not 'Manages remotes.' from @Program(About:) on RemoteArgs. A separate check (a8b.fb) shows the cause: a generic About<T>() returning T.Attribute(Program)?.About prints 'Manages remotes.' for RemoteArgs and '(null)' for RemoteArgs?. ParserOf<F> gets F as the T? type, so @Program on a subcommand's Struct is silently ignored. The @Subcommand Help is not passed in as the About either.

## args-9 (low): DefaultText shows '(default: 0.0)' for a Double, though its comment says a 0 isn't shown

Status: fixed in be880b85: DefaultText hides a number whose AsDouble() is 0, of any kind. tests/programs/os_args.fb's Maybe help, with Ratio: Double = 0.0 and Count: Int = 0

Where: std/os/args.fb:666

DefaultText hides Null, False, 0, "", and an empty Array, per its comment, by comparing against Value(0). A Double Field defaulting to 0.0 saves as Value(0.0), which isn't equal to Value(0), so the help shows '(default: 0.0)' while an Int 0 is hidden.

```
Import std.core
Import std.os

@Program(Name: "t")
Struct Opts
    Field Ratio: Double = 0.0
    Field Count: Int = 0
End Struct

Print(Args.Help<Opts>())
```

Expected: Usage: t [options]

Options:
      --ratio <ratio>
      --count <count>
  -h, --help           Prints this help

Actual:       --ratio <ratio>    (default: 0.0)
      --count <count>
(the 0.0 is shown, the Int 0 isn't)

Suggested fix: Treat a numeric zero of any kind as nothing worth showing (compare value.AsDouble() == 0 for numbers), or change the comment to say only an integer 0 is hidden.

Skeptic: Ran a9.fb. The help shows '--ratio <ratio>  (default: 0.0)', while '--count <count>' shows no default. DefaultText (line 666) compares against Value(0), and a Double 0.0 does not equal it. The comment says a 0 is not shown.

## compiler-2 (low): non_null's doc comment was left above the new all_non_null

Status: fixed in ba78a218: non_null's comment sits above it again, and all_non_null has only its own (a comment; no test)

Where: compiler/src/sema.cpp:12402

all_non_null was inserted between non_null's doc comment and non_null itself. all_non_null now has two stacked doc comments, the first of which ("The T of a T?, and any other type as it is.") describes non_null, and non_null has none. This is a comment-only defect, so the program below only exercises the code around it and builds fine.

```
Import std.core

Struct Box
    Field Size: Int
End Struct

For Field f: F In Box??
    Print(f.Name)
End For
```

Expected: In sema.cpp, `/// The T of a T?, and any other type as it is.` sits directly above non_null, and only `/// A type without any of the T?s around it.` sits above all_non_null.

Actual: Lines 12402-12403 put both comments above all_non_null, and non_null at line 12412 has no doc comment. The program itself prints "Size".

Suggested fix: Move `/// The T of a T?, and any other type as it is.` down so it sits directly above non_null.

Skeptic: In rev-math compiler/src/sema.cpp, line 12402 reads `/// The T of a T?, and any other type as it is.` and line 12403 reads `/// A type without any of the T?s around it.`. Both sit above all_non_null (line 12404), and non_null (line 12412) has no doc comment. The comment on line 12402 describes non_null, not all_non_null. The finding's program (c2.fb, For Field over Box??) builds and prints "Size", which matches the finding's claim that it builds fine. The defect is only in the comments.

## Refuted by the skeptics (no action)

- args-3: A Bool Field that defaults to True becomes a flag that can never turn it off. Reproduced the output: 'color: True', then "INVALID '--color' takes no value", then "UNKNOWN unknown option '--no-color'". This matches the documented design, though. @Flag's doc says it is 'A Bool Field that "--name" or "-s" sets', and the lane notes settle that flags take no value. Neither stdlib.md, the milestones row nor the spec promises --no-<name> or --name=false. A True default on a flag is the author's own choice, and clap's SetTrue behaves the same way. This is a possible design gap, not a contradiction of the docs, so I refute it.
