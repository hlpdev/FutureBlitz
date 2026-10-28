# The review of M7 step 2 (prebuilt libraries embedded)

27 confirmed findings, each with its Status.

## codegen-1 (high): An object the library made has a different Class record from the program's, so the program's Extend ... Implements crashes on it

array_class() and channel_class() give every T[] and Channel record internal linkage, so the library and the program each have their own fb.Int32[].class. A library Class's record is linkonce_odr, but what it holds depends on the build: the program's copy has the tables for the program's own Extend ... Implements, and the library's copy doesn't. The program has a record only for a Class it makes itself. When the library made the object, it carries the library's record, which has no table for the program's Interface. A call through that Interface then reads past the end of the list and crashes.

Fix: A shared Class record has to be the same in every build. Either the program always emits the record, the one that wins, for every library Class and T[] it can see, with its full implementation list, or Interface tables for Extends are looked up by Interface at run time and not baked into the record. Give T[] and Channel records SHARED linkage when shared_type() allows it, so there is one record per type, as decisions.md says for Classes' fb_class_t's.

Status: fixed: a program that links a library makes its own record of each Class the .fbi shows, with its Extends' Interfaces, and LLVM keeps it

## codegen-2 (high): A library that depends on a prebuilt library defines that library's releaser, and programs using both fail to link

In a library build, lower_top_level still runs over the files that aren't the library's own, and call_prebuilt_initializer adds each prebuilt dependency's module to m_mir.modules. generate_releasers() then defines an empty external fb.<dep module>.(release) for each module in that list, including the dependency's. The program that links both binaries then has two external definitions of that symbol. The empty releaser would also skip releasing the dependency's Globals.

Fix: In generate_releasers(), only make releasers for the library's own modules, the ones lower_initializers() adds, whose initializer is EXPORTED. Or keep call_prebuilt_initializer's modules out of a library build's `modules`.

Status: fixed: as lower-2

## codegen-3 (high): fb build crashes on a library module whose first item is a type; a program crashes when a .fbi module starts with one

Module order, which generate_main's releases and the library's releasers sort by, comes from m_item_order.at(first item). declare_globals() records only a type statement's members in m_item_order, never the type statement itself. So lower_initializers() throws std::out_of_range when a library module's first item is a Class, Struct, or Interface, which is any module without an Import line that starts with a type. call_prebuilt_initializer() (line 2406) throws the same way in the program when the .fbi's module part starts with a type, for example after a leading Private Function that the .fbi leaves out.

Fix: Record each type statement in m_item_order too, at its first member's place, or take the order from the first item that has one. Add tests for a library module that starts with a Class, and for a .fbi module that starts with one.

Status: fixed: as lower-5

## codegen-4 (high): A prebuilt library's module with an empty .fbi part never has its initializer called, so its Globals are never set

The program's main calls '<module>.(init)' only from lower_top_level, where it first reaches one of the module's items. A library module with no Imports and only Private or Package declarations is written as `// module multi/b.fb 0` with no items, so main never calls its initializer and never calls its releaser. The library's code then reads its Globals unset.

Fix: Call each prebuilt module's initializer from the module list, in dependency order, or have the .fbi always write an item, such as a marker, that main can attach the call to. Don't depend on the module having an item in the .fbi.

Status: fixed: as lower-1

## driver-1 (high): A Public ThreadLocal Global the library's own code never uses loses its initializer in the .fblib

use_thread_local() only makes a ThreadLocal's initializer function the first time lowered code uses the Global ("one the program never uses costs nothing"). In a library build the program's code isn't there, so a Public ThreadLocal that only programs read gets a maker with no initializer call (checked in the disassembled bitcode: fb.make.tl.tl.names stores the state words and nothing else). A program that uses it reads its zero: 0 for an Int, Null for an array or object, so it crashes.

Fix: In a library build, make the initializer (use_thread_local) for every ThreadLocal declared in the library's own files, or at least every Public or Package one that the .fbi shows, before lowering, so each EXPORTED maker calls it.

Status: fixed: a library's build makes the initializer of every ThreadLocal of its own

## driver-2 (high): A library built against a prebuilt dependency defines that dependency's releaser, so no program can link it

In a library build, lower_top_level still calls call_prebuilt_initializer for a prebuilt dependency's modules. That pushes the dependency's module into m_module.modules, and codegen's generate_releasers() then defines an external fb.<dep>.(release) in the library's binary. The dependency's own .fblib also defines it, so LLVMLinkModules2 fails. (The library's (init) also calls the dependency's (init), which the program's main calls too.) With the same dependency built from source it works, so a prebuilt library only works if its own dependencies are source.

Fix: Skip call_prebuilt_initializer, and leave prebuilt modules out of m_module.modules, in a library build. The program's main already initializes and releases each prebuilt module it reaches. Add a CLI test with a prebuilt library that depends on another prebuilt library.

Status: fixed: as lower-2

## lower-1 (high): A prebuilt module with no items in its .fbi never gets its (init) called, so its Private Globals are never set

lower_top_level only calls call_prebuilt_initializer when it reaches a module's first item in m_ast.items(). A library module that has no Import and only Private or Package declarations is written to the .fbi as '// module lib1/b.fb 0', with no items. The program never reaches it, so it never calls 'lib1.b.(init)' (and never calls its (release)). When a Public function in another module calls into that module, it reads unset Globals.

Fix: Drive the initializer calls from the prebuilt modules themselves, in load order, not from their first item. For example, walk m_ast.modules() and call each prebuilt module's (init) at its place, even when it has no items. Add a test with a multi-module library whose module has no Import.

Status: fixed: main calls every prebuilt module's initializer in the modules' order, one with no items in its .fbi too

## lower-2 (high): A library built against another prebuilt library defines an empty (release) for it, so programs fail to link

In a library's build, lower_top_level still calls call_prebuilt_initializer for a prebuilt dependency's modules. That pushes them into m_module.modules (and puts an (init) call in the dead entry). codegen's generate_releasers then defines 'fb.<dep module>.(release)' in the library's binary, as an empty function with external linkage. The dependency's own binary also defines that symbol, so LLVMLinkModules2 rejects the program.

Fix: In a library's build (m_module.library), skip call_prebuilt_initializer, or skip lower_top_level's prebuilt handling entirely. m_module.modules should then hold only the library's own modules, so generate_releasers defines only its own. Add a test of a prebuilt library that depends on another prebuilt library.

Status: fixed: a library's build calls no prebuilt module's initializer, and makes releasers only for its own

## lower-4 (high): A Public Const with a runtime initializer runs it twice, once in the library's (init) and again in the program's main

The .fbi keeps a Const whose value isn't known at compile time as its source, for example 'Public Const First: Thing = Make()'. It isn't marked prebuilt, so declare_global gives it SHARED linkage, and the program's main runs its initializer. The library binary's EXPORTED copy is also set in '<module>.(init)'. The two copies merge into one symbol after linking. The initializer therefore runs twice: its side effects happen twice, the first value is destroyed early, and both the library and the program see the second value.

Fix: Treat every Global and Const of a prebuilt module, other than generic Static Field instances, as IMPORTED, or have the .fbi write '= ...' for a Const that isn't known at compile time. The library's binary then sets it once. Make the linkage depend on the module being prebuilt, not on node.prebuilt alone.

Status: fixed: every Global and Const of a prebuilt module is its binary's, set once by its initializer

## lower-5 (high): fb crashes with 'unordered_map::at' when a library module's first item is a Class, Struct, or Interface

declare_globals gives m_item_order an entry for a type_stmt's members but not for the type statement itself. lower_initializers takes 'part.order = m_item_order.at(id)' of a module's first item, and call_prebuilt_initializer takes 'm_item_order.at(first)'. Both throw when that first item is a Class, Struct, or Interface. This hits the library's build when the module has no Import. It also hits a program when the .fbi drops a Private Global that came first, leaving a type first.

Fix: Give the type statement an order too, for example m_item_order.emplace(id, ...) before its members, or take the order of the first member or Global found. Add tests with a module that starts with a type.

Status: fixed: a type has a place in the Globals' order, so a module can start with one

## codegen-5 (medium): Debug forms of a prebuilt library's objects show only the type name

A Class record's debug slot is filled only when its build used a debug form (lower's undebugged()/m_debug_used). A library that never shows `{x:?}` itself writes records with a null debug slot, and its T[] records are internal and have none either. When the program shows an object the library made, and the program has no record of its own for that type, the library's record is used. So `{x:?}` prints just the type's name. This is the same problem as codegen-1: linkonce_odr records whose contents depend on the build.

Fix: Have a library build always generate debug functions for its Classes and T[]s, since a program may show them, and make the T[] records shared, as in codegen-1.

Status: fixed: as lower-7

## codegen-6 (medium): T[] and Channel records are per build, so the leak report splits one type's count

The T[] records (array_class, about line 2215) and Channel records (channel_class) are LLVMInternalLinkage in every build that links. After linking, the class list has one fb.Node[].class for each binary, and the runtime counts and reports them separately. decisions.md's M7 step 2 row says Classes' fb_class_t's are linkonce_odr and become one once linked, so that a Class two binaries share is counted once.

Fix: Make T[] and Channel records SHARED (linkonce_odr) when shared_type(type) is true, as declare_classes does, so finish_class_list dedupes them.

Status: fixed: as lower-8

## codegen-7 (medium): A main-module Global named `classes` breaks every build that links a prebuilt library

Main-module Globals are named fb.<name>, so `Global classes` becomes @fb.classes. add_class_list() then gets fb.classes.N with appending linkage, which LLVM rejects. finish_class_list() also finds the user's Global by the name "fb.classes", RAUWs it and deletes it. The source build is fine, because there the list is private and its name doesn't matter.

Fix: Give the appending list and the count names that no fb name can produce, for example with characters an identifier can't contain, like 'fb.(classes)', the way '(init)' and '(release)' do. Have finish_class_list look it up by that name.

Status: fixed: the Class list and its count are fb.(classes) and fb.(class count), which no FutureBlitz name makes

## codegen-9 (medium): `--show llvm` fails with invalid IR for any program that uses a prebuilt library

The CLI's --show llvm calls emit_ir() without the prebuilt libraries' code in options.linked. So m_linking is false, and set_linkage() makes the IMPORTED declarations (the library's (init), functions, and Globals) internal. LLVM's verifier rejects that. Even if it didn't, the shown IR would not include the linked library.

Fix: Pass the prebuilt binaries to emit_ir for --show llvm, the way build_program does, or set m_linking whenever the MIR has IMPORTED functions or Globals or prebuilt modules. Add a CLI test for --show llvm with a prebuilt dependency.

Status: fixed: --show llvm links the prebuilt binaries too

## driver-3 (medium): A corrupt or unlinkable .fblib exits fb from inside LLVM, with a message that doesn't name the file

link_libraries() calls LLVMParseBitcodeInContext2 and LLVMLinkModules2 on a context that has no diagnostic handler. LLVM's default handler prints 'error: <LLVM text>' and exits the process, so fb's own messages ('a prebuilt library's .fblib isn't LLVM bitcode fb wrote' and '... doesn't link with the program') can't be reached. Neither says which library it is either. prebuilt_binaries() only checks the two header lines, and the ABI hash isn't checked against the code, so a truncated download reaches this path.

Fix: Set a diagnostic handler on the context (LLVMContextSetDiagnosticHandler) that records the message instead of exiting, and return an error naming the .fblib's path. Pass the paths with the code in codegen_options_t::linked. Optionally, check that the code matches the header's hash in prebuilt_binaries.

Status: fixed: as codegen-8

## driver-4 (medium): --show llvm fails on any program with a prebuilt dependency, and stops fb build

The LLVM view calls emit_ir with only { .mode }, so options.linked is empty and m_linking is false. set_linkage() then makes every IMPORTED declaration (each prebuilt module's (init), Globals, ThreadLocal makers, functions) internal, and the verifier rejects the module. fb check --show llvm and fb build/run --show llvm both return compile_errors_exit_code before building.

Fix: Pass the prebuilt binaries' code (prebuilt_binaries(compilation)) in the options the --show llvm path gives emit_ir, or set m_linking whenever the module has IMPORTED code, so that view shows external declarations.

Status: fixed: as codegen-9

## lower-3 (medium): A program reading a prebuilt library's Public ThreadLocal Global directly gets 0 instead of its initializer's value

For a prebuilt Global written 'ThreadLocal Global seed: Int = ...', node.value is empty, so 'runs' is false and m_thread_local_declarations never gets an entry. ready() emits a ready_t, which makes the thread run the maker, only when the Global is in m_thread_local_declarations or its type has references. The program's own reads of an Int ThreadLocal therefore never call fb.make.<name>, the library's maker, and read the raw zero. Reads through library code are correct.

Fix: Treat a prebuilt ThreadLocal Global as one with an initializer, so ready() emits a ready_t for it. The library binary defines fb.make.<name> for each of its ThreadLocals. Alternatively, record in the .fbi whether it has one.

Status: fixed: a program's use of a prebuilt ThreadLocal makes it through its binary's maker

## lower-6 (medium): number_alike_names numbers colliding names in each build's own order, so a program can call the wrong function in a prebuilt library

Two functions can lower to one name: module nm.x's Class y's Static Method Z, and module nm.x.y's Function Z, are both 'nm.x.y.Z()'. The later one in m_module.functions gets '#2'. The library's build orders functions by declaration, while a program orders them by first use. The IMPORTED declaration named 'nm.x.y.Z()' or 'nm.x.y.Z()#2' then binds to the other function's code in the binary. Both have the same signature, so nothing catches it.

Fix: Make names that cross a binary unique by construction, independent of lowering order. For example, put a separator a module name can't contain between the module and the rest, or number by declaration order across all builds. At least make it an error for a library to export two alike names.

Status: fixed: a name two things share is an error in a build that links, since each build numbers them in its order

## lower-7 (medium): A library's debug forms ({x:?}) show only the type's name for objects and arrays the program makes

A Class record gets its debug function only when its own build shows a debug form (undebugged()/m_debug_used). Records are SHARED (linkonce_odr), and LLVMLinkModules2 keeps the program's copy. When only the library's code uses '{x:?}', the program's record has no debug function, and the library's DEBUG_OBJECT falls back to the type's name. Arrays get a second problem: T[] records are always internal (codegen array_class), so an array the program makes never has the library's debug function.

Fix: When a program links prebuilt binaries, give every Class and array record its debug function (or record in the .fblib header that the binary uses debug forms). Make T[] records shared as other records are.

Status: fixed: a library's build, and a program that links one, give every Class its debug form

## lower-8 (medium): T[] Class records stay internal in a build that links, so leak reports count an array type once per binary

array_class (and channel_class and scheduler_class) set LLVMInternalLinkage unconditionally. The library's and the program's Node[] records stay two records, finish_class_list keeps both, and the runtime reports the same type on two lines. Decisions' M7 table says a Class two binaries share is counted once. This part only consumes MIR linkage, but it is the same sharing the lowering's SHARED linkage is meant to give.

Fix: Use set_linkage(global, shared_type(type) ? SHARED : INTERNAL) for array records, and likewise for channel records, as for other Class records. Their release and visit functions then need to be shared too, or internal copies kept per record.

Status: fixed: T[] and Channel records are shared, as Classes' are

## codegen-10 (low): libraries.md and the milestones row still describe the .fblib as position-independent objects

Step 2 made the .fblib a header plus LLVM bitcode that is linked into the program's module (spec 11 and decisions.md say so). libraries.md still says it is 'its compiled code as an archive of position-independent objects', and that embedding links its objects. The same section says the ABI hash covers only the interface file, the compiler's version, the target, and the mode, while spec 11 and decisions.md include the compiled code. docs/milestones.md line 244 still says '.fblib (position-independent objects and the .fbi)'.

Fix: Update libraries.md's ABI section and the milestones row to match spec 11 and decisions.md.

Status: fixed: libraries.md and the milestones row say what the .fblib is

## codegen-11 (low): New MIR types moved existing doc comments onto the wrong declarations

mir_linkage_t was inserted between global_t's doc comment ('/// A top-level Global or Const, or a Static Field.') and global_t. So that line now runs into the enum's comment, and global_t has none. In the same way, mir_library_module_t was inserted after the long mir_module_t comment (about naming, ending in 'numbered: "x.y.Z()#2"'), so that comment now sits above mir_library_module_t's own comment, and mir_module_t has none.

Fix: Move global_t's line below the enum, directly above `struct global_t`, and move the mir_module_t comment below mir_library_module_t, directly above `struct mir_module_t`.

Status: fixed: as lower-9

## codegen-8 (low): Bitcode that won't parse or link exits through LLVM's default diagnostic handler, so fb's own error never appears

link_libraries() uses LLVMParseBitcodeInContext2 and LLVMLinkModules2 on a context that has no diagnostic handler. When either fails, LLVM's default handler prints its own message and calls exit(1). So fb's messages ('a prebuilt library's .fblib isn't LLVM bitcode fb wrote' and '... doesn't link with the program') are never shown. The output doesn't name the .fblib, and there is no 'couldn't build app' line. The header check doesn't catch this, because it doesn't check the hash against the code.

Fix: Install a handler with LLVMContextSetDiagnosticHandler that records error diagnostics, so the two failure branches run, and include the library's .fblib path in the message. Add a test with a valid header and corrupt bitcode.

Status: fixed: a diagnostic handler keeps LLVM's error, and fb's names the .fblib

## driver-6 (low): The library's ABI hash and panic paths depend on the folder fb build ran from

build_library emits bitcode whose module ID and panic locations use paths shown relative to the working directory. The hash covers that code, so the same source, compiler, target, and mode give different builds depending on where fb build ran. A prebuilt library's panics point at the author's relative path, which doesn't exist beside the prebuilt package.

Fix: In a library build, name files by their package-relative module path (as '// module leaky/leaky.fb' does) for the module ID and panic locations.

Status: fixed: a library's build names its files by their paths in the program, and its module by its package

## driver-8 (low): docs/libraries.md and the milestones row still describe the .fblib as position-independent objects, and the hash as not covering the code

libraries.md says a .fblib is 'an archive of position-independent objects, with its .fbi', and that 'The ABI hash covers the interface file, the compiler's version, the target, and the mode'. docs/milestones.md's M7 step 2 row says '(position-independent objects and the .fbi)'. This step made the .fblib '// fblib ...', '// bundled ...', and LLVM bitcode, beside the .fbi rather than containing it, with a hash that also covers the code, as spec 11 and decisions.md now say.

Fix: Update libraries.md's ABI section (the .fblib's form, and the hash covering the code) and the milestones row's wording.

Status: fixed: as codegen-10

## lower-10 (low): The milestones row still says the .fblib is position-independent objects

M7 step 2's row describes 'the .fblib (position-independent objects and the .fbi)'. Spec 11, decisions.md's M7 table, and the code all make the .fblib two header lines followed by LLVM bitcode, which is linked into the program's module. Decisions also gives the reason not to use position-independent objects.

Fix: Reword the row, for example: 'the .fblib (its build's header and LLVM bitcode, beside the .fbi)'.

Status: fixed: the milestones row says what the .fblib is

## lower-9 (low): mir.hpp's new declarations took over global_t's and mir_module_t's doc comments

mir_linkage_t was inserted between global_t's comment ('A top-level Global or Const, or a Static Field.') and global_t. That comment now opens the enum's doc, and global_t has none. mir_library_module_t was inserted after mir_module_t's long comment ('A whole program, lowered from its modules' files... numbered: "x.y.Z()#2"'). That comment now documents mir_library_module_t, and mir_module_t has none.

Fix: Move '/// A top-level Global or Const, or a Static Field.' down to global_t, and move mir_library_module_t (with its comment) above mir_module_t's comment block.

Status: fixed: mir.hpp's comments are back on their declarations
