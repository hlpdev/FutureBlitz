# The review of M7 step 1 (project types and the interface file)

32 confirmed findings, each with its Status.

## loader-1 (high): A non-generic type's Operators are left out of the .fbi, so programs can't use +, <, [] on it

An Operator has its type's visibility and can't be written Public, so add_candidate sees it as Private and drops it. It is only kept when the whole body is kept, as in a generic type. A Public Struct's Operator +, <, == and [] are all missing from the interface file. A program that checks cleanly against the source then fails against the .fbi. An == silently falls back to the default comparison instead of the library's Operator ==.

Fix: In the writer, mark an Operator member as shown whenever its owner type is kept, since it has the type's visibility. Add Operators (binary, unary, [] and []=) to the driver test that compares a program against the library's source and against its .fbi.

Status: fixed: a kept type keeps its Operators

## loader-2 (high): A body-less New in a .fbi is checked for its Super.New call, so a subclass whose base takes parameters breaks every program

The writer turns 'Public New() / Super.New(1) / End New' into 'Public New() => ...'. Sema still requires a New of an Extends class to call Super.New(...) when the base has no New without parameters. It reports that error inside the prebuilt library's interface file for any program that imports it, even though the program doesn't use the class. The library itself checks and builds cleanly.

Fix: In sema, skip the missing-Super.New check (and the related 'assigns before Super.New' check) for a New whose body is the interface file's '=> ...'. Add a test with an Extends class whose base New takes parameters.

Status: fixed: a prebuilt New isn't checked for a Super.New call its body makes in the binary

## syntax-1 (high): Operators are left out of the interface file, so a + b on a library's type fails against the .fbi, and == quietly falls back to the built-in comparison

An Operator can't take a visibility of its own (sema says 'an Operator has its type's visibility, so it takes no Public'), so its AST modifiers.visibility stays PRIVATE. add_candidate sets shown from that visibility alone. An Operator's candidate name is '+' or '==', and m_names only collects IDENTIFIER tokens, so an Operator is never 'shown', 'always', or kept by name. Every Operator of a Public Struct or Class, or of an Extend, is dropped from the .fbi. A program that uses the operator then checks differently against the .fbi than against the source: '+' becomes an error, and '==' on a Struct silently means the built-in comparison instead of the library's. A Struct that Implements Equatable or Hashable through its Operator == loses it too.

Fix: In add_candidate, treat a function_kind_t::OPERATOR member as having its owner's visibility, so it's shown when its type or Extend is (or make it 'always' in a kept type). Add a driver test with a Struct Operator + and ==, and an Operator in an Extend, checked against the .fbi.

Status: fixed: a kept type keeps its Operators, an Extend's too

## syntax-2 (high): Non-Public Methods that implement an Interface, and an Extend that only adds Implements, are dropped, so the .fbi itself fails to check or a type stops implementing its Interface

A Method written without Public, such as 'Method ToString(): String' in 'Public Class Box Implements Printable', is valid source and implements the Interface. The writer leaves out every non-Public Method unless its name turns up in kept text. The Class header still says 'Implements Printable', so the .fbi has a Class that doesn't give the Interface's Method, and every program that imports the library gets an error inside the .fbi. An Extend whose members aren't Public ('Extend Pt Implements Printable' with 'Method ToString()'), or one with no members that takes all of an Interface's defaults ('Extend Box Implements Greeter' / 'End Extend'), is never included, because wanted() returns false for a block and only an included member brings the block in. The type then doesn't implement the Interface against the .fbi. The Greeter case with a member only survives when the Interface's own kept text happens to use the member's name.

Fix: Keep every member that implements one of its type's or its Extend's Interfaces (or keep a kept type's Interface Methods like Overrides, as 'always'). Keep an Extend with an Implements whenever its type is kept, even with no members. Test both cases against a .fbi.

Status: fixed: a kept type keeps every member, and every Extend, Implements and all, is kept

## syntax-3 (high): Main-thread checks stop at a prebuilt function: Spawn of a library function that uses a Global or calls @MainThread code checks clean against the .fbi

Spec 10 checks work on another thread 'with everything it can run: what it calls, ...', and it reports a call into a function that uses a mutable Global or calls a @MainThread function. check_function returns early for a prebuilt function, and '=> ...' carries nothing about what its body uses. A program that Spawns code calling a library's Public function therefore gets no error against the .fbi, though it gets one against the source. The same goes for what overrides call, for Destroy(), and for a ThreadLocal Global's '= ...' initializer. This is a check users will rely on, and it silently passes a data race.

Fix: The interface file has to record, for each prebuilt function, Method, Destroy, and ThreadLocal initializer, what it can run that only the main thread can, and say why (for example a written @MainThread, or a marker naming the Global or function the message points to). sema then reports it at prebuilt calls as it does at source calls. The format of that record is a design call for the user, since spec 11 doesn't settle it. Add a test that Spawns a prebuilt function of each kind.

Status: fixed: @MainThreadOnly("why") before a prebuilt function, Method, New, or Property's Get and Set says what analysis found in its body, which the main-thread rules use as the source's code (decisions.md)

## writer-1 (high): A type's Operators, [] and []= included, are left out of the interface file

Spec 06 says Operators take their type's visibility and can't have a modifier of their own. So the parser leaves them Private, `shown` is false, and `always` is false. An Operator's name is never an identifier in kept text, so it is never brought in by name either. Every Operator of every type gets dropped, Public types included, even when kept @Inline or generic code uses it.

Fix: Treat an Operator (function_kind_t::OPERATOR, [] and []= included) as having its type's visibility: shown when its owner is kept, as an Enum or Interface member is. Add a test with a Public Struct's +, ==, [] and []= used from a program.

Status: fixed: a kept type keeps every member, Operators, [], and []= among them

## writer-2 (high): A Private Method that implements an Interface is dropped, so the .fbi's own Class no longer implements it

Spec 06 allows Private implementers: "What implements an Interface's member can be Private: code anywhere calls it through the Interface". wanted() keeps a Private member only when its name shows up in kept text. When the Interface comes from another package, such as std's Printable, the name usually doesn't show up, so the implementing Method is dropped. The `Implements` clause stays in the header. The physics/kinds tests only pass because their Interfaces are in the same file and their members are Public.

Fix: When a type that lists Interfaces (its own or its bases') is kept, also keep each member that implements one of their members, Methods and Properties alike. Add a test where a Private ToString implements std's Printable.

Status: fixed: a kept type keeps every member, so a Private Method that implements an Interface is there

## writer-3 (high): An Extend that lists an Interface is dropped when its members are Private, so the type stops implementing it

A non-generic Extend is only kept when one of its members is kept. Spec 06's own example, `Extend Point Implements Printable` with `Method ToString(): String => ...`, has no visibility on the member, so the whole Extend is dropped, Implements clause included. That conformance should hold everywhere: "the type implements the Interface everywhere".

Fix: Always keep an Extend that has an Implements list when its type is kept or is another package's, along with the members that implement those Interfaces. Add a test.

Status: fixed: every Extend is kept, with all its members

## writer-4 (high): Every generic Extend (Extend Box<T>, Extend T[], or one with Where lines) is left out entirely

verbatim() returns true for a generic Extend, so add_candidate() never adds its members as candidates. An Extend candidate has no name and no modifiers, so `shown` is false and `name` is empty. That means wanted() never returns true for it, and a generic Extend is never written, whatever its members' visibility. Public generic code that spec 11 says the interface file keeps with its body is lost.

Fix: For a verbatim Extend, decide from its members: keep it when any member is Public, or is named by kept code. Add a test with Extend Box<T>, Extend T[], and an Extend with a Where line.

Status: fixed: every Extend is kept, a generic one whole

## loader-5 (medium): fb build on a prebuilt package writes an empty .fbi and reports success

A prebuilt package is a library whose library_files() is empty, so build_library writes an interface of none of its modules: only the header, with a new hash. It prints 'Finished debug build'. Anyone who runs fb build in a prebuilt package (to refresh it, or by mistake) gets build/<target>/<mode>/<name>.fbi with nothing in it. If that file is copied into lib/, every declaration is gone.

Fix: In build_library (or compile_command), when the main package is itself prebuilt (compilation.interfaces holds its own .fbi), stop with a usage error naming the .fbi. Add a cli test.

Status: fixed: fb build on a prebuilt package is an error naming its .fbi

## syntax-4 (medium): Because Private Methods are left out, a program's subclass can declare a Method with a base Private Method's name, accepted against the .fbi but an error against the source

The intended consequence of leaving Private Methods out was only that calling one says 'has no member' instead of 'is private'. It also changes which programs check. Against the source, a Class extending a library Class can't declare a Method named like the base's Private Method ('can't override Shape's: 'Hidden' is private'). Against the .fbi, the base has no such member, so the program checks clean. The done-when ('checks against a library's .fbi as it does against its source') doesn't hold for it.

Fix: Keep a kept Class's Private Methods' first lines in the .fbi (as '=> ...', so they're 'private to <the .fbi>'), or have sema not report a Private base Method from another package, so both paths agree. Add a test.

Status: fixed: a kept Class keeps its Private Methods, so a program's Class can't take their names

## writer-5 (medium): Kept code's calls that name no member (For-In's Iterate) don't bring that member in

Reach is worked out only from the identifier tokens in kept text. A generic or @Inline body that loops `For v In bag` over a Private Class of its own file calls Iterate without naming it. So that Private Iterate is left out, and the .fbi's own kept body no longer checks. Operators used in kept code fail the same way, see writer-1.

Fix: Work out reach from the analysis rather than from names: the symbols a kept body's expressions resolve to, implicit Iterate, Next, ToString and Operator calls included. Or keep, for each kept type, the members that protocols call. Add a test.

Status: fixed: a kept type keeps every member, so the Iterate and Operators that kept code calls without naming them are there

## writer-6 (medium): Hidden Imports use a module's dotted name, which doesn't parse for files like my-shapes.fb or End.fb

import_name() writes `Import <module.name> As __fbiN` for every module that isn't a .proto. The source can import any file by a quoted path, for example `Import "./my-shapes"` or `Import "./End"`. The dotted names `odd.my-shapes` and `odd.End` are not valid Import syntax, so the .fbi can't be read.

Fix: For a module of the same package, write the hidden Import as a quoted relative path. Use a dotted name only when every part is an identifier and not a keyword. Add a test.

Status: fixed: a hidden Import names one of the library's own modules by its path from the module's folder, and another package's by its dotted name, or it's an error to write the type

## writer-7 (medium): A library that imports a .proto writes an interface file that the loader rejects

The constructor takes every module whose path starts with '<name>/', which includes the module fb makes from a .proto. The .fbi therefore gets a `// module pro/sub/game.proto 22` part holding the generated FutureBlitz. read_prebuilt() rejects any module path that doesn't end in .fb, so every user of the prebuilt library gets "isn't an interface file that fb writes". The hidden Import that import_name() writes for the proto module is also `Import "../../pro/sub/game.proto" As __fbi1`. That path goes up past the package's own folder and back in through its name, which only resolves when the folder's name matches the package's.

Fix: Write a proto module as a .fb-named module part, or make the loader accept it. Compute hidden quoted Imports from paths inside the package. Add a test with a .proto in a nested folder.

Status: fixed: a .proto file's module is written under its path, the loader reads it as the module fb made, its Imports of other .proto files by their paths in the program, and hidden Imports of it are paths in the package

## writer-8 (medium): Leaving out Private Methods, Properties, and Static Fields changes what programs check, not only the message

decisions.md says a left-out Private Method or Property is "one programs can't see at all", but some programs can see them. Spec 06 makes a Class's Method that redeclares a hidden Method's parameters, or a Property named like a hidden Property, an error. Spec 13 has For Method, For Property and For Static Field go over Private members too. Against the .fbi, programs that the source rejects pass, and compile-time loops give different results. Spec 11 only promises that Fields agree.

Fix: Keep every Method, Property and Static Field of a kept type as its first line, the way Fields are kept, or settle the difference in spec 11 and decisions.md. Add tests for both cases.

Status: fixed: a kept type keeps every member, Private Methods, Properties, and Static Fields as their first lines, still Private (decisions.md)

## writer-9 (medium): An Embed in a Class Field's default stops the library build, though programs never compile it

check_embeds() reports every Embed inside a kept Field's text, and every Field of a kept type is kept whole. A Class's Field defaults are set by its New, and the .fbi writes that New as `=> ...`, so the code is in the library's binary. The message "the library's interface file keeps this code, which programs compile themselves" is untrue here. A common pattern, an asset embedded into a Class Field, makes `fb build` fail.

Fix: For a Class's Fields, keep the default out of the Embed check, or don't write it, since programs never compile it. Keep the check for code a program actually compiles. Add a test.

Status: fixed: a Class's Field defaults are its New's, in the binary, written as "= ...", so an Embed in one is the library's own

## loader-3 (low): A prebuilt package without a .fbi for this build's mode or target silently becomes an empty source library, with misleading messages

read_prebuilt returns quietly when lib/<target>/<mode>/<name>.fbi isn't there. A prebuilt package that ships only a release build, or only another target's, is then loaded as a source library with no files. Every Import of it fails with 'has no modules in its folder' or 'no .../sub/inner.fb'. One Deep() call even resolved to std's Private Deep, 'private to std/core/load.fb'. Spec 11 says a prebuilt package has both modes, and the native-library rule makes a missing target an error naming the package. Nothing says which build is missing.

Fix: In read_prebuilt (or after loading), a library package with no .fb source but a lib/ folder holding any .fbi (another mode or target) is an error naming the expected lib/<target>/<mode>/<name>.fbi. Add a test with only the other mode's .fbi.

Status: fixed: a missing build for this target and mode is an error naming the file

## loader-4 (low): An .fbi with CRLF line ends is rejected as 'isn't an interface file that fb writes'

read_interface_header and interface_modules split on '\n' only. With CRLF, the hash word is 33 characters and each '// module <path> <lines>' count ends in '\r', so from_chars fails. Git for Windows' default autocrlf=true converts a committed .fbi to CRLF on checkout, as other tools converting text files do. A prebuilt package shared through a repository then stops loading, with a message that suggests the file isn't fb's.

Fix: Strip a trailing '\r' from each line in lines_of and from the header line, or normalize CRLF to LF before parsing. Add a test that loads a CRLF copy of a written .fbi.

Status: fixed: an interface file's lines can end in "\r\n"

## loader-6 (low): A program's subclass can redeclare a library's Private or Package Method against the .fbi, but not against the source

Leaving out Private and Package Methods makes a program check differently in a second way, beyond the 'has no member' message the user accepted. Against the source, a subclass declaring a Method with a Private or Package base Method's name is an error. Against the .fbi it is accepted, so a program that the source rejects checks cleanly against the prebuilt build. In step 2 that name then shares a Method table with the library's hidden Method.

Fix: Keep a non-final Public Class's Private and Package Methods in the .fbi as first lines with '=> ...', still Private and Package, so sema knows they're there. Or record the hidden names for the override check. Add the case to the source-vs-.fbi driver test.

Status: fixed: a kept Class keeps its Private and Package Methods

## loader-7 (low): fb check on a prebuilt package checks nothing, and the driver test that claims to check it passes without reading the .fbi

compile_package checks a library through library_files(own), which is empty for a prebuilt package, so none of the .fbi's modules are parsed or checked. An .fbi with an undefined type and a wrong return still finishes the check with no errors. The test 'a library's interface file holds what programs compile against' says 'The same library, prebuilt: its interface file in place of its source' and checks messages are empty, which would pass for any .fbi with a valid header.

Fix: For a prebuilt main package, use its .fbi modules' paths as the 'extra' modules, so fb check parses and checks them. Make the driver test assert on a broken .fbi.

Status: fixed: fb check on a prebuilt package checks its interface file's modules, and the test checks a broken one

## loader-8 (low): Missing-module messages against a .fbi garble the folder: 'no folder phys/nothing in <fbi>/ of modules'

shown_path gives a prebuilt package's paths as '<path> in <fbi>'. report_missing puts that into 'no folder {}/ of modules', so the '/' and 'of modules' land after the .fbi's file name. The empty-package message reads 'has no modules in its folder, emp in <fbi>'.

Fix: Give report_missing (and the no-modules message) their own wording for a prebuilt package: the folder path with its '/', then 'in <fbi>'. Add a test of the message.

Status: fixed: a missing module of a prebuilt library says it isn't in its interface file

## manifest-1 (low): Whether a package is an executable depends on the disk's letter case: a dependency with Main.fb fails on Windows and macOS but is a library on Linux

When `main` isn't written, the loader decides between executable and library with std::filesystem::is_regular_file(root / "main.fb"). On a disk that ignores letter case (Windows, and macOS's default APFS), that call also finds Main.fb or MAIN.FB. So a package that has only Main.fb is an executable there, and since this step it can't be a dependency. On Linux the same package is a library whose Main.fb is the module casedep.Main. compile_package's is_regular_file(written) at line 2164 makes the same case-blind choice for the package itself. Before this step the only effect was whether Main.fb was left out of `Import casedep`. Now the same project builds on Linux and fails on the other two, with no warning. Imports already warn when letter case differs from the disk; this check doesn't.

Fix: Find the default main file by its exact name in a listing of the folder (as same_file and respelled_file already compare names), both in package_loader_t::load and in compile_package. Optionally warn when a different spelling, like Main.fb, is there. Add a driver test for a dependency with only Main.fb.

Status: fixed: without 'main', a package's main file is main.fb by that exact name, found in a listing of its folder, so Main.fb is a module everywhere

## manifest-2 (low): A prebuilt library without a build for this mode or target is reported as a package with no modules

read_prebuilt looks only for lib/<target>/<mode>/<name>.fbi and returns quietly when it isn't there. A prebuilt package that has only its debug build (or only another target's) is then loaded as a source library with no .fb files. The program's Import reports that the package has no modules in its folder. Spec 11 says a prebuilt package has both a debug and a release build, and a build links the one of its own mode. The message hides the actual problem: a missing build for this mode.

Fix: In read_prebuilt, when the package has no .fb source but its lib/ folder has a <name>.fbi for another mode or target, fail at the package's name with the path that's missing. Test it with a debug-only prebuilt package built in release.

Status: fixed: a package with no .fb source and an interface file for another target or mode is an error naming the lib/<target>/<mode>/<name>.fbi that's missing

## manifest-3 (low): fb check and fb build on a prebuilt package check nothing, and build writes an interface with none of its modules

When the package fb is given is itself prebuilt (project.toml plus lib/<target>/<mode>/<name>.fbi, no .fb), compile_package takes the library branch with library_files(own), which lists only .fb files on disk. That list is empty. `fb check` reports success without checking the .fbi's modules. `fb build` writes build/<target>/<mode>/<name>.fbi with only a header, under a different hash, so every declaration of the prebuilt build is lost. A user who runs `fb build` in a vendor's prebuilt package gets an empty interface with no warning.

Fix: In compile_package, when own.interface isn't empty, stop with a usage error ("liba is prebuilt, lib/windows-x64/debug/liba.fbi; there's no source to build"), or check its modules for fb check. Add a CLI test.

Status: fixed: fb check on a prebuilt package checks its interface file's modules, and fb build on one is an error naming its .fbi

## manifest-4 (low): Messages about an executable dependency give advice that leads to another error, or don't say it's a dependency

The message for a dependency with a main file advises type = "library". When the dependency's project.toml writes `main` (as in the step's own test, main = "./run.fb"), following that advice gives "a library has no main file", so the user has to fix it a second time. Also, a dependency with type = "executable" and no main file gets only "an executable needs its main file" in its own project.toml. Nothing says it can't be a dependency, so a user who adds the main file then hits the dependency error.

Fix: Mention removing `main` when manifest->main_span is set. For named != nullptr with type == EXECUTABLE, report the can't-be-a-dependency error, either as well as the main-file error or in place of it. Test both cases.

Status: fixed: the advice says type = "library" without 'main' when 'main' is written, and a dependency whose type is "executable" is the can't-be-a-dependency error

## manifest-5 (low): fb build on a library's main.fb names the executable after the package, as if main.fb were its main file

compile_package compares a given main file with own.root / manifest.main, and manifest.main is "main.fb" by default even for type = "library", where main.fb is an ordinary module. So `fb build lib/main.fb` names the program after the package, while any other file of the library names it after the file. Spec 11 says a build is named after the package, or after another main file it was given. A library has no main file of its own, so main.fb is another file and should name the executable.

Fix: In compile_package, compare against the package's main file only when it isn't a library (manifest.type != LIBRARY), so a library's main.fb names its program by its stem.

Status: fixed: in a library, main.fb is a module, so a program built from it is named by its stem

## manifest-7 (low): No test covers a package with type = "library" and a main.fb being checked or built itself, or an explicit type = "executable" dependency

The only test where a type = "library" package has main.fb compiles the program that depends on it (lib/run.fb as a module). No test runs fb check or fb build on such a package directly. That path goes through compile_package's `manifest.type == package_type_t::LIBRARY ||` branch, where main.fb has to be checked as a module (top-level code is an error) and written into the .fbi as lib/main.fb. Without that condition, the package would be compiled as a program from main.fb, and no test would fail. The tests' type = "library" packages (driver_tests.cpp:2533, 2661, 2821, and tests/packages/physics) have no main.fb, and no test file writes lib/main.fb or physics/main.fb. Also, the executable-dependency error is tested only for a dependency whose `main` is written, not for one with type = "executable" and main.fb.

Fix: Add those cases to the driver test (compile_file on the lib folder with main.fb and type = "library") and to the CLI build test (the .fbi contains `// module lib/main.fb`).

Status: fixed: tests of a library with main.fb checked and built, and of an executable-typed dependency

## syntax-5 (low): The hidden Import qualifier __fbiN can clash with a name the library declares, so the .fbi fails to check

qualifier() names hidden Imports '__fbi1', '__fbi2', ... without checking the module's own names. An identifier can start with underscores, and nothing in the spec reserves '__fbi'. A library module that declares '__fbi1' and has an inferred Global of another module's type gets a .fbi that redeclares the name, and every program that imports it fails.

Fix: Skip qualifiers that the module already uses as a name (its declarations and Import aliases), or reserve the prefix in spec 01 and report it in source. Add a test.

Status: fixed: a hidden Import's qualifier is one no identifier in the module's text is

## syntax-6 (low): In an interface file, '= ...' and '=> ...' are accepted where spec 11 doesn't allow them, giving silent INVALID types or misleading errors

Spec 11 allows '= ...' only on a Global or Static Field, after its type, and '=> ...' on a declaration whose code is in the binary. The parser takes '= ...' on any variable in an interface file: a Const, a Local in a kept body, a Field, or a Global with no type. check_initializer then returns early for any prebuilt variable without a value. So 'Public Global g = ...' gets no type and no error, and uses of it are quiet. 'Public Const C: Int = ...' gets no 'a Const needs a value' error, and its use as an array size gives the misleading 'a fixed array's size must be an integer known at compile time'. 'Local y: Int = ...' gives "'y' is read before it's assigned". 'Public Const Function K(): Int => ...' gives "'K' doesn't return a value on every path". fb never writes these, but a .fbi in a package can be hand-edited or damaged, and its messages should say what's wrong.

Fix: Parse '= ...' only for a GLOBAL or a Static FIELD, and report it elsewhere. In sema, report a prebuilt variable that has no type, and '=> ...' on a Const, Abstract, or Extern Function. Extend the parser test with these cases.

Status: fixed: "= ..." only follows a Global's or a Field's type, and "=> ..." after a Const Function is an error

## syntax-7 (low): accept_prebuilt took span_from's doc comment, parse_into's doc names only Globals, and the parser test leaves most header_ends unchecked

accept_prebuilt was inserted between span_from's doc comment and span_from. The comment "/// From `begin` to the end of the last consumed token." now sits on top of accept_prebuilt's own comment, and span_from has none. parser.hpp's parse_into doc says '= ...' is "for a Global", but the parser and spec 11 also allow it on a Static Field. The parser test checks header_end only for a Function and a Class. The Property, Extend, and Extern block header_ends (and Set's) that the writer slices by are untested in parser_tests.cpp.

Fix: Move the 'From `begin`...' line back above span_from. Say "for a Global or Static Field" in parser.hpp. Add header_end checks for a Property, an Extend with Implements, an Extern block with Lib, and a Static Field '= ...' to the parser test.

Status: fixed: span_from has its comment back, parser.hpp names Globals, Static Fields, and Class Fields, and the parser test checks a Property's, an Extend's, and an Extern block's first lines

## writer-10 (low): fb build on a prebuilt package, or on a library with no source, writes an empty interface file and reports success

write_interface() writes a header with no module parts whenever no module's path matches. That happens for a package with no .fb files, and for a prebuilt package, whose modules come from its .fbi. The CLI then prints Finished and exits 0. The prebuilt case discards the package's real interface in favor of an empty one. Both cases give the same hash, b6e678af..., which is the hash of an empty body.

Fix: In build_library or write_interface, refuse a package whose modules came from a .fbi, and a library with no .fb modules, each with an error naming the package. Add CLI tests.

Status: fixed: fb build on a prebuilt package, or on a library with no .fb files, is an error naming it

## writer-11 (low): Spec 11 and decisions.md leave out Const code among the reached Private declarations that keep their bodies

Spec 11 says the Private and Package declarations reached by kept bodies are kept "with bodies only when they're generic or @Inline themselves". decisions.md's step-1 row likewise says "the Private and Package ones its generic and @Inline bodies reach". The writer also keeps a reached Private Const Function's body, which it has to, since programs run it at compile time. The writer is right and the docs are wrong.

Fix: In spec 11 change it to "with bodies only when they're generic, @Inline, or Const themselves", and in decisions.md (line 968) change it to "its generic, @Inline, and Const bodies reach".

Status: fixed: spec 11 and decisions.md say generic, @Inline, or Const
