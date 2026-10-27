# The review of M6 step 20's Protocol Buffers: fb reads .proto files at compile time (Import "x.proto") and generates FB types, with std.data's protobuf wire format

17 confirmed findings: 4 high, 2 medium, 11 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## generate-1 (high): A .proto that has a service doesn't build: the generated Interface's Async Methods are rejected by sema

Status: fixed in 9397c23, on master after step 13's Async Methods in Interfaces (fdef0587): proto_services' greeter.proto has a unary rpc, a streaming one, and a deprecated one, which a Class implements and a client calls through the Interface, sending its messages through the wire format; the finding's program, which only uses the messages, builds too

Where: compiler/src/proto.cpp:4346

write_service() emits `Async Method Rpc(request_: Req): Resp Throws Core_.Error` inside a `Public Interface`. But sema's check_async_function (sema.cpp:7443, which predates this step) still reports "an Interface's Method can't be Async yet". So importing any .proto with a `service` block fails, even when the program only uses its messages. Services are common in real .proto files. docs/stdlib.md ("A service is an Interface of an Async Method for each rpc") and the lane's settled design both promise these Interfaces. No program test imports a .proto with a service: proto_tests.cpp only parses one, and no tests/programs/proto_* .proto declares one. That is why this was missed.

```
// svc.proto, beside this file:
//   syntax = "proto3";
//   message Req { string name = 1; }
//   message Resp { string text = 1; }
//   service Greeter {
//     rpc Hello(Req) returns (Resp);
//   }
Import std.core
Import "svc.proto"
Print("{Req(name: "a")}")
```

Expected: name: "a"

Actual: svc.proto.fb:190:18: error: an Interface's Method can't be Async yet
    Async Method Hello(request_: Req): Resp Throws Core_.Error
error: couldn't build svc.fb because of 1 error

Suggested fix: Either let sema accept Async Methods in Interfaces (and the lowering they need), or have write_service emit something the compiler supports today. Then add a program test whose .proto has a service, with unary and streaming rpcs and a deprecated one, implemented by a Class and called.

Skeptic: I wrote g1/svc.proto (proto3, Req, Resp, service Greeter { rpc Hello(Req) returns (Resp); }) and svc.fb, which imports it and prints only Req(name: "a"). Debug run: `svc.proto.fb:190:18: error: an Interface's Method can't be Async yet` at `Async Method Hello(request_: Req): Resp Throws Core_.Error`, then `couldn't build svc.fb because of 1 error`. The error comes from sema.cpp:7441-7443 (check_async_function), which still rejects every Async Method on an Interface. So any .proto with a service fails to import, even when the program never uses the service. stdlib.md line 676 says "A service is an Interface of an Async Method for each rpc". In tests/, only tests/compiler/proto_tests.cpp mentions a service; no program test .proto has one.

## generate-2 (high): A .proto import whose types the file doesn't name leaves an orphan module: hundreds of bogus errors

Status: fixed in f95ca6e (collect_imports adds each file the driver found for one of the .proto's imports, so its module Imports every file it imports, in the order of their paths, whose types it names or not; a .proto with errors gets failed_proto_module, a comment and those Imports, so its imports' modules are in the program too; proto_import_kinds builds a custom option over descriptor.proto, an unused import, and a file with only import public, 604, 16, and 3 errors before, and the unit test "a .proto with errors gives an empty module, past them" has bad.proto's one error with ok.proto imported, 87 before)

Where: compiler/src/proto.cpp:2996

collect_imports() writes an `Import "<path>" As alias_` only for files whose messages or enums a field or rpc names. The driver still loads a module for every .proto import (driver.cpp load_proto). initialization_order() only visits modules reached through Import lines, so that module is in no module. sema's module_of() then puts its statements in the main module, which reports its Import lines as misplaced, and its code can't see Core_/Proto_/std.collections. Common cases hit it: custom options (import google/protobuf/descriptor.proto and `extend google.protobuf.FieldOptions`), `import public` re-export files (the decisions name these), and plain unused imports. The same orphaning happens when a .proto with any error imports another .proto: the failed file's module is a comment, so its imports are orphaned. Any std module only they import is orphaned too, giving about 85 errors inside std/collections/*.fb for one typo.

```
// custom/opts.proto, beside this file:
//   syntax = "proto3";
//   import "google/protobuf/descriptor.proto";
//   extend google.protobuf.FieldOptions {
//     int32 weight = 50000;
//   }
//   message Item {
//     string name = 1 [(weight) = 5];
//   }
Import std.core
Import "custom/opts.proto"
Print("{Item(name: "a")}")
```

Expected: name: "a"

Actual: std/data/proto/google/protobuf/descriptor.proto.fb:2:1: error: Import lines go at the top of a file, before its other statements
  Import std.core As Core_
... ('Core_' isn't an imported module, 'AddAll' is in std.collections, which this file doesn't import, ...)
error: couldn't build custom.fb because of 604 errors
(The same happens with a/c.proto where c imports a, and a only has `import public "b.proto"`; and with a bad.proto that has a syntax error and imports ok.proto: 87 errors, most of them in std/collections/array.fb.)

Suggested fix: Make every loaded .proto module reachable. Have collect_imports also add each resolved file in m_file.imports, so the generated module imports every module the driver loaded for it. Or have the driver's initialization_order visit every m_loaded entry, not only those reached from main. Also skip loading, or keep reachable, the imports of a .proto that failed. Add program tests for an unused import, `import public`, custom options, and a failing .proto that imports another.

Skeptic: Reproduced exactly: custom/opts.proto imports google/protobuf/descriptor.proto and uses `extend FieldOptions` plus `[(weight) = 5]`. The build fails with `descriptor.proto.fb:2:1: Import lines go at the top of a file` and `'Core_' isn't an imported module`, 604 errors in all. Other cases I tried: (a) a plain unused `import "other.proto"` gives 16 errors in un/other.proto.fb; (b) c.proto importing a.proto, which only has `import public "b.proto"`, gives 3 errors in pub/a.proto.fb; (c) bad.proto with a syntax error importing ok.proto (which has a map) gives 103 errors after the real one, most of them in the orphaned ok.proto.fb and std modules. Cause: collect_imports (proto.cpp:2996-3030) only adds files whose types a field or rpc names (field.type, method.input/output), so the loaded module of any other import is reached from no Import line.

## parse-1 (high): A '}' the top level reaches hangs fb, and its memory grows without end

Status: fixed in f95ca6e (parse_top_level reports a '}' that closes nothing, "this '}' closes no block", and steps past it, and parse_rpc skips what isn't an option in its braces there, so the service keeps its own '}'; the syntax-errors test's stray '}', missing '{', and error in an rpc's braces, which each hung before)

Where: compiler/src/proto.cpp:886

At the top level, parse_top_level() reports "expected a declaration" and calls skip_statement(). But skip_statement() stops before a '}' at depth 0 and doesn't consume it. run()'s loop then sees the same '}' again, forever, adding a diagnostic each time. On the program below, `fb check` used 456 MB after 3 s and 1376 MB after 8 s, and never finished. Three ordinary mistakes put a '}' at the top level: (1) a stray '}'; (2) a missing '{', as in `message M int32 x = 1; }` or `enum E A = 0; }`, because skip_statement eats the body up to ';'; (3) an error inside an rpc's option block, as in `rpc F(A) returns (A) { foo; }`, because the service loop takes the rpc's '}' as its own. All three hang (each was checked with `timeout`). The LSP would hang the same way.

```
// f1/point.proto, beside this file:
//   syntax = "proto3";
//   message Point { int32 x = 1; }
//   }
Import std.core
Import "f1/point.proto"
Print("{Point(x: 1)}")
```

Expected: An error at f1/point.proto:3:1 for the '}' that closes nothing, then the build stops.

Actual: fb run / fb check never return (killed by timeout, exit 124), and memory grows by about 180 MB/s.

Suggested fix: Make every top-level recovery path consume at least one token. In run() or parse_top_level(), report a stray '}' and advance past it. Also have parse_rpc() skip to its own closing '}' before returning false, so the service loop doesn't take the rpc's '}' as the service's end. Add tests for a stray '}', a missing '{', and an error inside an rpc body.

Skeptic: I built three cases in rv-proto/parse-verify: f1 (a stray '}' after the message), f1b (`message M int32 x = 1; }`), and f1c (`rpc F(A) returns (A) { foo; }` inside a service). `timeout 8 fb check` exited 124 on all three and printed nothing. I ran f1 through Start-Process: after 4 s it was still running, with 603 MB working set and 808 MB private. The code: skip_statement() (proto.cpp ~811) returns at a depth-0 '}' without advancing. parse_top_level()'s else branch calls it, and run()'s `while (!at_end())` loop meets the same '}' forever, adding an error each time.

## parse-2 (high): A .proto import the generated module doesn't name breaks the build

Status: fixed in f95ca6e, with generate-2, the same bug (proto_import_kinds and the unit test "the module of a .proto imports each file it does, whose types it names or not")

Where: compiler/src/driver.cpp:1013

load_proto() loads and parses the module of every file a .proto imports. But the generated module only Imports the files whose types its fields and rpcs name (proto.cpp collect_imports). initialization_order() visits only files reached through Imports ("every other is reached from these"). So a .proto module that nothing Imports never gets into the module table, while its statements are still in the AST. Sema then treats its Imports as code after other code ("Import lines go at the top of a file"), and every Core_/Proto_ use fails. Three common cases trigger it: custom options (`import "google/protobuf/descriptor.proto"` used only by an `extend`), any unused import (protoc only warns), and a forwarding file that has only `import public "new.proto"`. proto_imports.fb doesn't catch it because the program Imports every .proto itself. Adding `Import "google/protobuf/descriptor.proto"` to the program makes the build below succeed.

```
// f2/item.proto, beside this file:
//   syntax = "proto3";
//   package demo;
//   import "google/protobuf/descriptor.proto";
//   extend google.protobuf.FieldOptions {
//     string label = 50000;
//   }
//   message Item {
//     string name = 1 [(label) = "Name"];
//   }
Import std.core
Import "f2/item.proto"
Local item = Item(name: "x")
Print("{item}")
```

Expected: name: "x"

Actual: std/data/proto/google/protobuf/descriptor.proto.fb:2:1: error: Import lines go at the top of a file, before its other statements ... error: couldn't build f2.fb because of 604 errors

Suggested fix: Make the modules of all of a .proto's imports reachable. Either record the .proto-level import edges in m_loaded[index].imports, so initialization_order() visits them, or have generate_proto() Import every import whose file was found, used or not. Alternatively, don't add the AST of a module nothing reaches. Add tests for an unused import, a custom option over descriptor.proto, and a forwarding file with only `import public`.

Skeptic: f2 (item.proto imports google/protobuf/descriptor.proto only for `extend FieldOptions` and a custom option) fails with `std/data/proto/google/protobuf/descriptor.proto.fb:2:1: error: Import lines go at the top of a file`. f2b (a plain unused `import "other.proto"`) fails the same way at f2b/other.proto.fb:2:1. f2d is a forwarding old.proto (only `import public "new.proto"`) that user.proto imports, and the program imports only user.proto; it fails at f2d/old.proto.fb:2:1. f2e, f2 with `Import "google/protobuf/descriptor.proto"` added to the program, builds and prints name: "x". In driver.cpp, load_proto() calls load_proto/parse_file on every import. But initialization_order() (line 1013) visits only m_loaded[].imports edges, which come from the generated module's FB Imports. So a .proto module that nothing Imports is parsed but never ordered.

## generate-3 (medium): ToString (text format) shows an enum value's escaped FutureBlitz name, like True_, not its .proto name

Status: fixed in f95ca6e (each Enum's Extend has ProtoName(value), named past its values, whose Select gives a value's name in the .proto, or an open enum's number it doesn't name, and the text writes an enum field, a repeated one's values, and a map's through it; proto_names' Poll prints True, False, and Default, True_, False_, and Default_ before)

Where: compiler/src/proto.cpp:4260

text_value() writes an enum field as `text_.Write("name", "{value}")`, which uses the FutureBlitz Enum's own text: the member name after value_name(), so a reserved name gets a '_'. protobuf's text format, which ToString is meant to match as ShortDebugString, prints the enum value's .proto name. Field names are written with their .proto names (field.name), and only enum values are wrong. PascalCase enums, common in C#-style protos, hit this with values like Default, True, False, End, or Static.

```
// enumtext/answer.proto, beside this file:
//   syntax = "proto3";
//   enum Answer { Default = 0; True = 1; False = 2; }
//   message Poll { Answer answer = 1; repeated Answer history = 2; }
Import std.core
Import "enumtext/answer.proto"
Local poll = Poll(answer: Answer.True_, history: [Answer.False_, Answer.Default_])
Print("{poll}")
```

Expected: answer: True history: False history: Default

Actual: answer: True_ history: False_ history: Default_

Suggested fix: For enum fields, write the .proto name. For example, generate a per-enum Static Method (named so it can't clash) that Selects on the value and returns the .proto name, with the integer in its Default for an open enum's other values. Call it from text_value for ENUM in fields, repeated fields, and map values. Add a test with reserved value names.

Skeptic: enumtext/answer.proto has `enum Answer { Default = 0; True = 1; False = 2; }` with a singular, a repeated, and a map field. Debug and release both print `answer: True_ history: False_ history: Default_ m { key: 1 value: True_ }`. protobuf's text format would print True/False/Default. text_value (proto.cpp:4239-4261) writes ENUM through its default branch, `Write(name, "{value}")`, which gives the FutureBlitz Enum member's escaped name. stdlib.md promises ToString is "protobuf's text format on one line", and protobuf can't parse this output back (it names values that don't exist).

## parse-3 (medium): An enum inside a group inside an extend survives the extend, with a parent index that's gone

Status: fixed in f95ca6e (parse_extend erases the enums its groups declared, with their messages; the test "proto2's labels, defaults, groups, extensions, and extends" has an enum in an extend's group and a message after it, and the enum stayed before)

Where: compiler/src/proto.cpp:1721

parse_extend() parses its fields into a scratch message, then erases m_file.messages from that scratch onward. A group's nested enum is pushed into m_file.enums with parent = the group's message index, and the erase doesn't remove it. That enum keeps a parent index into the erased messages. If fewer messages follow, proto_enum_name() reads past the end of m_file.messages, which fails the libstdc++ assertion (and is undefined behavior in a build without assertions). If enough messages follow, the dropped enum is generated as a nested enum of an unrelated message: below, with messages Bar, Baz, and Qux after the extend, `Baz_E.B` exists and prints B.

```
// f3/ext.proto, beside this file:
//   syntax = "proto2";
//   message Foo {
//     optional int32 a = 1;
//     extensions 100 to 200;
//   }
//   extend Foo {
//     optional group G = 100 {
//       enum E { A = 0; B = 1; }
//       optional E e = 1;
//     }
//   }
//   message Bar { optional int32 x = 1; }
Import std.core
Import "f3/ext.proto"
Local b = Bar(x: 3)
Print("{b}")
```

Expected: x: 3

Actual: stl_vector.h:1282: ... operator[] ... [with _Tp = fb::proto_message_t ...]: Assertion '__n < this->size()' failed. (exit 3). With Baz and Qux added after Bar, it builds, and the module has an Enum Baz_E.

Suggested fix: In parse_extend(), record m_file.enums.size() before the loop and erase enums back to that size along with the messages. Add the case to the 'proto2's labels, defaults, groups, extensions, and extends' test.

Skeptic: f3: extend Foo { optional group G = 100 { enum E {A=0;B=1;} optional E e = 1; } } followed by message Bar. `fb run` aborted with `stl_vector.h:1282 ... [with _Tp = fb::proto_message_t ...]: Assertion '__n < this->size()' failed.` (exit 3). f3b, with Baz and Qux after Bar, built and printed `x: 3` then `B` for `Print("{Baz_E.B}")`, so the dropped enum became Baz's. In parse_extend() (proto.cpp 1699-1723), only m_file.messages is erased back to the scratch index. An enum that parse_enum pushed with a parent in the erased range stays in m_file.enums.

## generate-4 (low): ToString writes map entries in insertion order; protobuf's text format sorts them by key, so equal messages print differently

Status: fixed in f95ca6e (a map's text takes its keys, sorts them, by value, false before true, or by a string's bytes, and writes each entry in that order; proto_messages' maps filled in two orders print alike, in protoc's order)

Where: compiler/src/proto.cpp:4294

text_field() writes a map by iterating the Map directly (`For key_, value_ In Self.m`), which goes in insertion order. protobuf's text format sorts a map's entries by key: C++ TextFormat::Printer::PrintField calls MapFieldPrinterHelper::SortMap, and Python's text_format iterates sorted(value). ShortDebugString does the same. So two messages that are == (Map equality ignores order) give different ToString text, and neither may match protoc's. The program tests only have one entry per map, so nothing checks the order.

```
// mp.proto, beside this file:
//   syntax = "proto3";
//   message M { map<int32, string> m = 1; }
Import std.core
Import "mp.proto"
Local a = M()
a.m[2] = "b"
a.m[1] = "a"
Local b = M()
b.m[1] = "a"
b.m[2] = "b"
Print("{a == b}")
Print("{a}")
Print("{b}")
```

Expected: True
m { key: 1 value: "a" } m { key: 2 value: "b" }
m { key: 1 value: "a" } m { key: 2 value: "b" }

Actual: True
m { key: 2 value: "b" } m { key: 1 value: "a" }
m { key: 1 value: "a" } m { key: 2 value: "b" }

Suggested fix: In text_field for a map, collect the keys into an array, sort them (integers by value, bools false first, strings by bytes, as protobuf does), and write each entry in that order. Add a test with several entries inserted out of order.

Skeptic: mp.proto has `map<int32, string> m = 1`. a is filled 2 then 1, b is filled 1 then 2. Debug and release both print `True`, then `m { key: 2 value: "b" } m { key: 1 value: "a" }`, then `m { key: 1 value: "a" } m { key: 2 value: "b" }`. text_field (proto.cpp:4292-4302) iterates the Map in insertion order. The lane says ToString is ShortDebugString, and C++ TextFormat::Printer always sorts map entries by key (Python, Java, and Go do too), so this output differs from ShortDebugString. I rated it low because the unsorted text is still valid text format that parses back to the same message. FutureBlitz's own Map also prints in insertion order ({2: b, 1: a} vs {1: a, 2: b} while ==), so different text for equal values isn't new to the language. The real problem is only the mismatch with ShortDebugString and protoc.

## generate-5 (low): An enum alias named Hash or ToString makes the generated Extend fail to build

Status: fixed in f95ca6e (value_name gives an alias, a Static Property of the Enum's Extend, a '_' after Hash or ToString, which every Enum has, and each use of an alias's name goes through it; proto_names' Answer has aliases Hash and ToString, which didn't build before)

Where: compiler/src/proto.cpp:3132

write_enum() turns each alias (allow_alias) into `Public Static Property <value_name(alias)>: E` in an `Extend E`. value_name() only escapes keywords and built-in type names. Spec 06 says an Extend of an Enum can't take the name of a member the Enum already has, and an Enum has ToString and Hash. So an alias with either name is an error in the generated module, and a valid .proto doesn't import. A member (not an alias) with those names works, and from_number() already avoids the value names. The alias Static Properties are the one place the escaping misses.

```
// alias/level.proto, beside this file:
//   syntax = "proto3";
//   enum Level {
//     option allow_alias = true;
//     LOW = 0;
//     HIGH = 1;
//     Hash = 1;
//   }
//   message Reading { Level level = 1; }
Import std.core
Import "alias/level.proto"
Print("{Reading(level: Level.HIGH)}")
```

Expected: level: HIGH

Actual: alias/level.proto.fb:14:28: error: Level already has a member named 'Hash'
    Public Static Property Hash: Level
error: couldn't build alias.fb because of 1 error
(The same happens for an alias named ToString.)

Suggested fix: Escape an alias's Static Property name against the Enum's built-in members too: fb_name(name, { "ToString", "Hash", plus the enum's other value names and FromNumber }). Use that name in check_values' clash check and in default_value() when a default names the alias. Add a test.

Skeptic: alias/level.proto has allow_alias and `HIGH = 1; Hash = 1;`. Building fails: `alias/level.proto.fb:14:28: error: Level already has a member named 'Hash'` at `Public Static Property Hash: Level`. The same happens for an alias named ToString (`already has a member named 'ToString'`). A control where Hash and ToString are real members, not aliases, builds and prints `level: Hash`. So the alias Static Property is the only place the escaping misses the Enum's built-in members, and a valid .proto is refused. I rated it low because aliases with those exact names are rare.

## parse-4 (low): A package line after declarations leaves them out of the package

Status: fixed in f95ca6e (parse_package puts the messages, enums, and services declared before it in the package too; the test "a package line puts what the file declares before it in the package too")

Where: compiler/src/proto.cpp:1201

full_name() uses m_file.package as it stands when each message, enum, or service is parsed. protoc's grammar allows `package` anywhere at the top level, and protoc applies it to every declaration in the file, whenever it's written. Here, a message declared before the package line gets the full name "A", not "p.A". So "B" doesn't resolve from A's scope, and other files can't name p.A. protoc accepts the file below.

```
// f4/late.proto, beside this file (t4/late.proto in the run):
//   syntax = "proto3";
//   message A { B b = 1; }
//   package p;
//   message B { int32 v = 1; }
Import std.core
Import "f4/late.proto"
Local a = A(b: B(v: 7))
Print("{a}")
```

Expected: b { v: 7 }

Actual: t4/late.proto:2:13: error: 'B' isn't a message or an enum that this file declares or imports (and 'A' and 'B' aren't declared in the program)

Suggested fix: Set full names once the whole file is read: either find the package line in a pass over the tokens before parsing, or at the end of run() prefix the full names of the messages, enums, and services declared before it. Add a test with the package line after a message.

Skeptic: f4/late.proto: syntax; `message A { B b = 1; }`; `package p;`; `message B {...}`. `fb run f4.fb` gives `f4/late.proto:2:13: error: 'B' isn't a message or an enum that this file declares or imports`. full_name() (proto.cpp 1195) reads m_file.package as it stands when each declaration is parsed. protoc's ParseTopLevelStatement takes `package` anywhere and sets the file-level package that every descriptor's full name uses, so protoc accepts this valid proto3 file. I set the severity to low, not medium: a package line after declarations is rare, the failure is a loud compile error, and full_name is used only in resolution and comments, so generated output isn't silently wrong.

## parse-5 (low): A \u surrogate pair in a string is an error, though protoc reads it as one character

Status: fixed in f95ca6e (escape joins a \u head surrogate and a \u trail surrogate right after it into one character, and either alone is still an error; the test "a .proto's numbers and strings, at their edges")

Where: compiler/src/proto.cpp:450

escape() rejects any \u code point from D800 to DFFF. protoc's tokenizer (FetchUnicodePoint) joins a \u head surrogate followed by a \u trail surrogate into one code point, so "😀" is U+1F600. A proto2 string default written that way, which JSON-minded tools produce, fails here. (The file has to be written with real backslashes; the run built it with Python.)

```
// f5/s.proto, beside this file (t2/s.proto in the run), with real backslashes:
//   syntax = "proto2";
//   message M {
//     optional string face = 1 [default = "😀"];
//   }
Import std.core
Import "f5/s.proto"
Local m = M()
Print("{m.faceOrDefault}")
```

Expected: The grinning-face emoji, U+1F600

Actual: t2/s.proto:3:40: error: '\uD83D' isn't a character's code point
t2/s.proto:3:46: error: '\uDE00' isn't a character's code point

Suggested fix: In escape(), when a \u value is a head surrogate and the next six characters are \u and a trail surrogate, combine the two into one code point, as protoc does. Keep the error for a lone surrogate. Add the case to the escapes test.

Skeptic: f5/s.proto has `optional string face = 1 [default = "😀"];` with real backslashes (grep confirmed). fb reports `f5/s.proto:3:40: error: '\uD83D' isn't a character's code point` and the same error at 3:46 for \uDE00. escape() (proto.cpp ~450) rejects any \u value from D800 to DFFF without checking for a following trail surrogate. protoc's FetchUnicodePoint joins a \u head surrogate and a \u trail surrogate into one code point. So a string protoc reads as U+1F600 is an error here.

## parse-6 (low): A .proto import in other letter cases than the disk's gets no warning

Status: fixed in f95ca6e (find_proto_import checks the parts of the path it found against the disk, warn_respelled_proto, as warn_respelled does an Import's; the driver test "a .proto's import in other letter cases than the disk's gets a warning", on a disk that ignores them)

Where: compiler/src/driver.cpp:554

find_proto_import() takes the first candidate has_file() finds. On Windows and macOS, the disk ignores letter case, so `import "Common/IDs.proto"` finds common/ids.proto silently, and on Linux the same program fails with "there's no ...". FB Imports warn about this (warn_respelled: "the disk spells it ..., in other letter cases, which only a disk that ignores them finds"). A quoted .proto Import in a .fb file gets that warning too. Only .proto-to-.proto imports skip it, because that path never calls warn_respelled.

```
// f6/common/ids.proto:
//   syntax = "proto3";
//   package common;
//   message Id { string value = 1; }
// f6/player.proto:
//   syntax = "proto3";
//   import "Common/IDs.proto";
//   message Player { common.Id id = 1; }
Import std.core
Import "f6/player.proto"
Local p = Player()
Print("built: {p}.")
```

Expected: A warning at f6/player.proto:2:8 that the disk spells it "common/ids.proto", as `Import "f6/Common/IDs.proto"` from a .fb file gets.

Actual: No diagnostic; it builds and prints "built: ." on Windows (and wouldn't find the file on Linux).

Suggested fix: Run the same respelling check on the path find_proto_import() returns, and warn at imported.span. Add a test next to the FB Import's letter-case test.

Skeptic: f6/player.proto has `import "Common/IDs.proto";`, and the disk has f6/common/ids.proto. `fb run f6.fb` builds with no diagnostic and prints `built: .`. f6b.fb, with `Import "f6/Common/IDs.proto"` from FB code, gets `warning: the disk spells it "f6/common/ids.proto", in other letter cases, which only a disk that ignores them finds`. find_proto_import() (driver.cpp 526-575) returns the first candidate has_file() accepts and never calls warn_respelled. Spec 11 says an Import's path in other letter cases than the disk's gets a warning, because only a disk that ignores case finds it. The same reason applies to a .proto's imports, which would fail on Linux.

## parse-7 (low): A long run of characters that aren't tokens overflows the stack

Status: fixed in f95ca6e (the lexer's run loops past each character that isn't a token, which next() reports and gives nothing for, instead of recursing; the syntax-errors test's 300000 '$', 300000 errors, a stack overflow before)

Where: compiler/src/proto.cpp:188

next() handles a character that isn't part of a .proto's syntax by reporting it and calling next_or_end(), which calls next() again. So a run of such characters, with only whitespace between them, recurses once per character. With a syntax line followed by 300000 '$' characters (200000 works), `fb check` dies with STATUS_STACK_OVERFLOW (0xC00000FD) and prints nothing.

```
// f7/junk.proto: the line syntax = "proto3"; then a line of 300000 '$' characters, then
// message M {}. Written with Python:
//   open('f7/junk.proto','w',newline='\n').write('syntax = "proto3";\n' + '$'*300000 + '\nmessage M {}\n')
Import std.core
Import "f7/junk.proto"
Print("hi")
```

Expected: Errors for the '$' characters (capped as usual), then the build stops.

Actual: fb.exe exits with 0xC00000FD (stack overflow) and no output.

Suggested fix: Loop in next() (or run()) over characters that aren't tokens, instead of recursing through next_or_end().

Skeptic: I wrote f7/junk.proto with Python: a syntax line, a line of 300000 '$', then `message M {}`. f7b has 300000 '$ ' pairs. Run from PowerShell, `fb check` exits 0xC00000FD (STATUS_STACK_OVERFLOW) on both and prints nothing; Git Bash reports 127. In next() (proto.cpp ~180-189), a character that isn't a token reports an error and returns next_or_end(), which calls next() again. That is one stack frame per character, and whitespace between them doesn't break the chain.

## parse-8 (low): Number literals differ from protoc's at the edges

Status: fixed in f95ca6e (an integer past 64 bits is a token marked too_big, at the most 64 bits hold, so what takes an integer reports it by its text, a field's number too; a float's or double's default reads a decimal one as a double, as protoc does, and a hex or octal one is an error; a custom option reads past it; a '.' or an exponent after an octal number is an error; the test "a .proto's numbers and strings, at their edges")

Where: compiler/src/proto.cpp:294

(1) A decimal integer past 64 bits is a lexer error. protoc's ConsumeNumber for a float or double default reads it as a floating-point number (TryParseFloat), so `[default = 100000000000000000000]` is 1e20 there. Because the error is in the lexer, the same literal also fails inside custom options and aggregates, which fb otherwise reads past. (2) A number that starts with '0' and has a '.' after the digits, like 01.5, reads as the float 1.5. protoc rejects it ("Hex and octal numbers must be integers").

```
// f8/big.proto:
//   syntax = "proto2";
//   message H {
//     optional double big = 1 [default = 100000000000000000000];
//   }
// f8/h.proto (separately, as f8.fb):
//   syntax = "proto2";
//   message H { optional double frac = 1 [default = 01.5]; }
Import std.core
Import "f8/big.proto"
Local h = H()
Print("{h.bigOrDefault}")
```

Expected: 1e+20 for big.proto; an error at 01.5 for h.proto.

Actual: f8/big.proto:3:38: error: 100000000000000000000 doesn't fit in 64 bits. And h.proto builds, printing 1.5.

Suggested fix: Keep an overflowing decimal integer as a token marked too big, not as an error: let a float or double default read its text as a double, and report the overflow only where an integer is needed. In number(), report a '.' or exponent after an octal (leading 0) integer, as protoc does.

Skeptic: f8: `optional double big = 1 [default = 100000000000000000000];` gives `f8/big.proto:3:38: error: 100000000000000000000 doesn't fit in 64 bits`. f8c, the same literal as the value of a custom double option `(weight)`, gives the same error at 4:46, so the lexer error also reaches options that fb otherwise reads past. f8b: `[default = 01.5]` builds and prints 1.5. number() treats the leading-zero digits followed by '.' as a decimal float. protoc's parser ConsumeNumber(double*) falls back to TryParseFloat for a decimal integer past uint64. protoc's tokenizer reports `Hex and octal numbers must be integers` for 01.5. Both are edge cases.

## parse-9 (low): No test for `Import "x.proto" As P`, `import weak`, or a stray '}'

Status: fixed in f95ca6e (proto_import_kinds Imports a.proto As P, which imports b.proto weak, whose C comes through its import public, and From c.proto Import C; the syntax-errors test has a stray '}')

Where: tests/compiler/proto_tests.cpp:119

The settled calls include "As renames" for a quoted .proto Import, and "weak is ordinary". Neither appears in tests/compiler/proto_tests.cpp or tests/programs/proto_*: grep finds no `.proto" As` and no `import weak`. Both work (program below). Error recovery has no test of a '}' at the top level, which would have caught parse-1.

```
// t7/c.proto: syntax = "proto3"; package q; message C { int32 v = 1; }
// t7/b.proto: syntax = "proto3"; package q; import public "c.proto"; message B { C c = 1; }
// t7/a.proto: syntax = "proto3"; package q; import weak "b.proto";
//             message A { B b = 1; C c = 2; .q.C d = 3; q.C e = 4; }
Import std.core
Import "t7/a.proto" As P
From "t7/c.proto" Import C
Local a = P.A(c: C(v: 1), e: C(v: 2))
Print("{a}")
```

Expected: Tests that pin this behavior: c { v: 1 } e { v: 2 }

Actual: It prints c { v: 1 } e { v: 2 }, but no test checks As on a .proto Import, import weak, or a top-level '}'.

Suggested fix: Add this case to proto_tests.cpp, or to tests/programs/proto_imports, and add a parse_errors case for a stray '}' once parse-1 is fixed.

Skeptic: Grepping tests/compiler/proto_tests.cpp and tests/programs/proto_* finds no `.proto" As` Import and no `import weak`. The import test at line 119 has only `import` and `import public`. The syntax-error test at line 242 has no '}' at the top level, which would have caught parse-1. The behavior itself works: t9 (`Import "t9/a.proto" As P`, `import weak "b.proto"`, `From "t9/c.proto" Import C`) prints `c { v: 1 } e { v: 2 }` in debug and release. This is a coverage gap only, which the project's rules ask to close with tests.

## wire-1 (low): ProtoTag builds a wrong tag, with no panic, for a field number outside 1..PROTO_FIELD_LIMIT or a wire type past 5

Status: fixed in 95916f6 (ProtoTag is @PanicsAtCaller, and panics for a number that isn't from 1 to PROTO_FIELD_LIMIT or a wire type past WIRE_FIXED32; panic_proto_tag and panic_proto_tag_wire)

Where: std/data/protobuf.fb:37

ProtoTag is Public and is meant for "code that reads or writes the wire format itself". It is just ((number As UInt32) << 3) | wire and checks nothing. Field 0 and field 536870912 both come out as tag 0. Field -1 comes out as 4294967288. A wire type of 9 runs into the number's bits, so ProtoTag(1, 9) is 9, which is field 1 with wire type 1. ProtoReader.ReadTag rejects all of these tags, so the writer and reader in the same file disagree. std's conventions say a bug (a bad index, a negative size) panics at the caller's line, and this function silently gives a wrong tag.

```
Import std.core
Import std.collections
Import std.data.protobuf

Print("{ProtoTag(0, WIRE_VARINT)} {ProtoTag(536870912, WIRE_VARINT)} {ProtoTag(-1, WIRE_VARINT)} {ProtoTag(1, 9)}")
```

Expected: A panic at the caller's line (t8.fb:5) naming field 0 as outside 1 to 536870911, as std's conventions want for a bug.

Actual: 0 0 4294967288 9

Suggested fix: Mark ProtoTag @PanicsAtCaller. Panic when number < 1 or number > PROTO_FIELD_LIMIT, or when wire > WIRE_FIXED32.

Skeptic: I ran t8.fb (ProtoTag(0, WIRE_VARINT), ProtoTag(536870912, WIRE_VARINT), ProtoTag(-1, WIRE_VARINT), ProtoTag(1, 9)) in debug. It printed '0 0 4294967288 9' and exited 0. std/data/protobuf.fb:37 is just ((number As UInt32) << 3) | wire. It checks nothing and has no @PanicsAtCaller. ProtoReader.ReadTag (lines 243-252) throws on field 0, on a number past PROTO_FIELD_LIMIT, and on wire types 6 and 7, so the file's own writer helper makes tags its reader rejects. The header of protobuf.fb says code that writes the wire format itself can use these helpers. docs/stdlib.md's Conventions say a bug (a bad index, a negative size) panics at the caller's line, and std uses @PanicsAtCaller widely for bad arguments. A silent wrong tag contradicts that.

## wire-2 (low): The public writer, reader, and text helpers panic inside protobuf.fb on bad arguments, not at the caller

Status: fixed in 95916f6 (WriteUnknown, EndLength, and KeepFrom are @PanicsAtCaller: WriteUnknown panics with the ParseError's message, EndLength for a start that isn't the 0 BeginLength left, and KeepFrom for one outside 0 to where it reads; panic_proto_unknown, panic_proto_end_length, and panic_proto_keep_from)

Where: std/data/protobuf.fb:588

These methods are Public for hand-written wire code, but none of them checks its arguments or carries @PanicsAtCaller. ProtoText.WriteUnknown, given bytes that aren't fields Skip kept (for example, truncated ones), catches the ParseError and calls Unreachable(). The panic then says the code isn't meant to be reached and points at protobuf.fb:588. ProtoWriter.EndLength with a start that BeginLength didn't give panics on an index at protobuf.fb:191 (EndLength(5) after one byte). A start that is in range but wrong silently overwrites a byte. ProtoReader.KeepFrom(-1, kept) panics on an index at protobuf.fb:442. std's conventions put a bug's panic at the caller's line.

```
Import std.core
Import std.collections
Import std.data.protobuf

Local text = ProtoText()
text.WriteUnknown([8, 1, 10, 5, 65])
Print(text.Finish())
```

Expected: A panic at the caller's line (t11.fb:6) saying the bytes aren't kept fields, for example 'the bytes end inside a field of 5 bytes at byte 3'.

Actual: panic: this code isn't meant to be reached at std/data/protobuf.fb:588:13 (similarly, EndLength(5) gives 'panic: index 5 is out of range for 1 element at std/data/protobuf.fb:191:9', and KeepFrom(-1, kept) gives 'panic: index -1 is out of range for 2 elements at std/data/protobuf.fb:442:25')

Suggested fix: Mark WriteUnknown, EndLength, and KeepFrom @PanicsAtCaller. WriteUnknown should panic with the ParseError's message instead of calling Unreachable. EndLength should check 0 <= start < Count, and KeepFrom should check 0 <= start <= Position. The other option is to make WriteUnknown non-Public, since only generated code calls it.

Skeptic: Each case ran in debug. ProtoText().WriteUnknown([8, 1, 10, 5, 65]) gave 'panic: this code isn't meant to be reached at std/data/protobuf.fb:588:13'. The Unreachable() in WriteUnknown's Catch can be reached, because the method is Public and takes any bytes. ProtoWriter with WriteRaw([1]) then EndLength(5) gave 'panic: index 5 is out of range for 1 element at std/data/protobuf.fb:191:9'. ProtoReader([8, 1]).KeepFrom(-1, kept) gave 'panic: index -1 is out of range for 2 elements at std/data/protobuf.fb:442:25'. A start that is in range but wrong corrupts silently: WriteTag(8), WriteVarint(1), EndLength(0) gave [1, 1], so the tag byte was overwritten and nothing panicked. None of these methods is @PanicsAtCaller or checks its arguments. protobuf.fb's header offers them to hand-written wire code, and stdlib.md's Conventions put a bug's panic at the caller's line.

## wire-3 (low): docs/stdlib.md says the generated modules go through ProtoTag, but they never call it

Status: fixed in f95ca6e (stdlib.md's row has ProtoTag and the WIRE_ constants as helpers for code that writes the wire format itself)

Where: docs/stdlib.md:133

The table row for std/data/protobuf.fb lists "`ProtoTag`, `ProtoIsZero`, `ProtoKeepEnum`, and `ProtoPanic`, which the modules fb makes from .proto files go through". The generator works out every tag in C++ (tag_of in compiler/src/proto.cpp) and writes it as a literal: WriteTag(8), Case 16, and so on. ProtoTag appears nowhere in proto.cpp or in any generated module's MIR. Only the other three helpers are used.

```
// p/t4.proto:
// syntax = "proto3"; package t4;
// message S { uint64 u = 1; sint32 z = 2; bool flag = 3; int32 i = 4; uint32 u32 = 5; sint64 z64 = 6; repeated bool flags = 7; repeated sint32 zs = 8; }
// Run as: fb check --show mir t13.fb | grep -c ProtoTag
Import std.core
Import "p/t4.proto"

Print("{S(z: -1).Encode()}")
```

Expected: If the docs were right, the MIR of S.WriteTo would call std.data.protobuf.ProtoTag.

Actual: grep -c ProtoTag gives 0. The MIR has 'call virtual std.data.protobuf.ProtoWriter.WriteTag(UInt32)(_1, 8)', '(_1, 16)', and '(_1, 24)', all literal tags.

Suggested fix: Take ProtoTag out of the list of helpers the modules go through, and describe it, with the WIRE_ constants, as a helper for code that writes the wire format itself.

Skeptic: I grepped the whole checkout (*.cpp, *.hpp, *.h, *.fb, *.md) for ProtoTag. It appears only at its definition, std/data/protobuf.fb:37, and in docs/stdlib.md:133. compiler/src/proto.cpp builds tags itself with tag_of() (line 2697) and writes them as literals, for example m_out.line("{}.WriteTag({})", writer, tag_of(...)) at lines 3706-3762. The generator does emit ProtoIsZero (3687), ProtoKeepEnum (3889), and ProtoPanic (3439, 3661). So stdlib.md's row, which says the modules fb makes from .proto files go through ProtoTag, describes the code wrongly.

## new-1 (low): The hint for a type of a .proto's module that a file doesn't import names the module without .proto

Status: fixed in f95ca6e (import_advice writes a .proto module's Import by the .proto's path, as it's written, without ./; the unit test "the module of a .proto imports each file it does, whose types it names or not" has "'Fresh' is in new.proto, which this file doesn't import: Import "new.proto"", which said Import "./new" before)

Where: compiler/src/sema.cpp:8351

Found while writing proto_import_kinds. A program that names a type of a .proto's module it doesn't import is told the Import line, but the line drops the .proto, as a .fb file's path drops .fb, and starts with ./, so it names a .fb file that isn't there.

```
// new.proto, beside this file: syntax = "proto3"; message Fresh { int32 v = 1; }
// main.proto: syntax = "proto3"; import "new.proto"; message Main { Fresh f = 1; }
Import "main.proto"
Local f = Fresh()
```

Expected: 'Fresh' is in new.proto, which this file doesn't import: Import "new.proto"

Actual: 'Fresh' is in new.proto, which this file doesn't import: Import "./new"

## new-2 (low): A .proto's module wrote its Import lines in the order of its imports' addresses in memory

Status: fixed in f95ca6e (the generator keeps the files it imports in m_imported, in the order of their paths, which their aliases were already given in; the unit test "the module of a .proto imports each file it does, whose types it names or not" checks the four lines' order)

Where: compiler/src/proto.cpp:2990

Found while fixing generate-2. m_imports, a std::map keyed by each imported file's proto_file_t*, was what the Import lines were written from, so their order, and with it the order in which the imported modules' Globals initialize, could change from one build to the next.

## new-3 (low): ToString's floats lost their exponent's sign after step 5's text for a double

Status: fixed in a7ac427 (FloatText writes the exponent as C's printf does, as protobuf does: with its sign, and two digits at least; proto_scalars' max and repeated lines print 1.7976931348623157e+308 and 1e+100 again, as protoc does, and its exponents line has 1.5e-07, 1e+21, 2.5e-300, and 1e-10)

Where: std/data/protobuf.fb:652

Found while rebasing onto master after step 5, which writes a double's exponent without its sign, 1e100 for 1e+100. ProtoText writes a float or a double through that text, so a message's ToString no longer read as protobuf's text format, ShortDebugString, writes it.

## Refuted by the skeptics (no action)

- wire-4: No test reads a map entry without its key or value, out of order, or with a field it doesn't know. I ran the program, plus more cases, in debug and release, using p/entries.proto with map<int32,string> and map<string,Inner>. The six entries printed exactly the expected lines. A message value given twice in one entry merged: 'ms { key: "k" value { a: 1 b: 2 r: 3 } }'. Two entries with the same key: the last entry won whole. A fixed32 value of the wrong wire type, and a group inside an entry, were both skipped. A decoded entry that had no value encoded back as [10, 4, 8, 5, 18, 0], which is protoc's form. There is no wrong behavior. The milestone row's protobuf done-when is that generated types read what protoc's encoder wrote, and protoc never writes these entries, so nothing in the spec, stdlib.md, or the row requires these tests. The gap is real (no committed test has such entries; proto_errors.fb has no map field), but it is a missing test, not a bug.
- wire-5: proto_errors.fb says a 10-byte varint keeps the low 64 bits, but its varint has no bits past 64; overlong varints aren't tested. I ran the program in debug and release, using p/varints.proto. It printed 'u: 18446744073709551615', then an empty line, then 'i: 5'. A 10-byte varint whose 10th byte is 1 also gave u: 18446744073709551615. This matches protobuf's readers: ReadVarint (protobuf.fb:257-273) keeps only bit 0 of the 10th byte, as C++, upb, and Java do. The comment in proto_errors.fb, 'A varint of 10 bytes keeps the low 64 bits, and an int32 its varint's low 32', is true of the case it covers, which reads a 10-byte varint's 64 bits and then takes the low 32. It is not false, only less broad than it could be. The behavior is correct and nothing contradicts the spec, stdlib.md, or the milestones row. What's missing is a test of a 10th byte above 1 and of an overlong varint, which is a coverage gap, not a bug.
