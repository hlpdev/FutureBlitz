#include "binary_images.hpp"
#include "fb/driver.hpp"
#include "fb/interface.hpp"
#include "fb/lower.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#endif

namespace {

/// "<file>: <source text>: message" for each diagnostic, in order.
std::vector<std::string> errors_of(const fb::compilation_t& compilation) {
    std::vector<std::string> errors;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        const fb::source_file_t& file = compilation.sources.get(diagnostic.span.file);
        errors.push_back(std::format("{}: {}: {}", file.path(), file.slice(diagnostic.span),
                                     diagnostic.message));
    }

    return errors;
}

/// `files`, with the standard library, which Print is in.
std::vector<fb::library_file_t> with_std(std::vector<fb::library_file_t> files) {
    std::ranges::move(fb::standard_library().value_or(std::vector<fb::library_file_t>{}),
                      std::back_inserter(files));
    return files;
}

/// The expression whose source text is `text`, which must be in the compilation.
fb::expr_id_t expression(const fb::compilation_t& compilation, const std::string_view text) {
    for (std::size_t i = 0; i < compilation.ast.expr_count(); ++i) {
        const auto id = static_cast<fb::expr_id_t>(i);
        const fb::span_t span = compilation.ast.get(id).span;
        if (compilation.sources.get(span.file).slice(span) == text) {
            return id;
        }
    }

    FAIL("no expression " << text);
    return {};
}

/// The file that declares what the call or name `text` refers to.
std::string declared_in(const fb::compilation_t& compilation, const std::string_view text) {
    const std::optional<fb::symbol_id_t> symbol =
        compilation.analysis.expr(expression(compilation, text)).symbol;
    CHECK(symbol);
    return symbol ? compilation.sources.get(compilation.analysis.symbol(*symbol).span.file).path()
                  : std::string();
}

/// The type of the expression `text`, as MIR names it.
std::string qualified_type(const fb::compilation_t& compilation, const std::string_view text) {
    return compilation.analysis.types.qualified_name(
        compilation.analysis.expr(expression(compilation, text)).type);
}

} // namespace

TEST_CASE("compile runs every stage, even after errors", "[driver]") {
    // The first line has a type error, and the second still gets analyzed.
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Import std.core\n"
                               "Local x: Int = \"a\"\nPrint(\"{x}\")");
    CHECK(compilation.source().path() == "test.fb");
    CHECK(compilation.diagnostics.error_count() == 1);
    CHECK(compilation.tokens.back().kind == fb::token_kind_t::END_OF_FILE);
    // The standard library's modules come first, and the main file's Import and two statements
    // after them.
    CHECK(std::ranges::count_if(compilation.ast.items(), [&](const fb::stmt_id_t id) {
              return !compilation.ast.outside_main(id);
          }) == 3);
    CHECK(compilation.analysis.exprs.size() == compilation.ast.expr_count());
}

TEST_CASE("compile adds the standard library after the main file", "[driver]") {
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Import std.core\nThrow Error(\"x\")");
    CHECK(compilation.diagnostics.diagnostics().empty());
    CHECK(compilation.file == fb::file_id_t{});
    CHECK(compilation.sources.get(fb::file_id_t{ 1 }).path().starts_with("std/"));
    CHECK(compilation.analysis.error_class != fb::type_id_t::INVALID);

    const std::expected<std::vector<fb::library_file_t>, std::string> library =
        fb::standard_library();
    REQUIRE(library);
    CHECK(std::ranges::is_sorted(*library, {}, &fb::library_file_t::path));
    CHECK(std::ranges::contains(*library, "std/core/error.fb", &fb::library_file_t::path));
}

TEST_CASE("each file is a module, named by its path", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.fb", .text = "" },
        { .path = "lib/shapes/circle.fb", .text = "" },
    };
    const fb::compilation_t compilation = fb::compile(
        "game/main.fb", "Import \"./lib/a\"\nImport \"./lib/shapes/circle\"\nLocal x = 1", library);
    const std::vector<fb::module_t>& modules = compilation.ast.modules().modules();
    REQUIRE(modules.size() == 3);
    CHECK(modules[0].name == "lib.a");
    CHECK(modules[1].name == "lib.shapes.circle");
    CHECK_FALSE(modules[1].is_main);

    // The main module is named by its file's stem, and comes last.
    CHECK(modules[2].name == "main");
    CHECK(modules[2].file == compilation.file);
    CHECK(modules[2].is_main);
    CHECK(compilation.ast.modules().main() == fb::module_id_t{ 2 });
    CHECK(fb::module_name("std/core/error.fb") == "std.core.error");
}

TEST_CASE("each module has its own names, then its imports'", "[driver]") {
    const std::vector<fb::library_file_t> library = with_std({
        {
            .path = "lib/a.fb",
            .text = "Import \"./b\"\nPublic Function Twice(x: Int): Int => Helper(x) * 2\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Public Function Helper(x: Int): Int => x\nPublic Const LIMIT = 10\n",
        },
    });
    const std::string imports = "Import std.core\nImport \"./lib/a\"\nImport \"./lib/b\"\n";
    const fb::compilation_t compilation =
        fb::compile("test.fb", imports + "Print(\"{Twice(LIMIT)}\")", library);
    CHECK(errors_of(compilation).empty());

    // A name the main module declares again hides the imported one for its own code, and the
    // imported module's code keeps calling its own.
    const fb::compilation_t hiding =
        fb::compile("test.fb",
                    imports + "Function Helper(x: Int): Int => x * 10\n"
                              "Local y = Helper(1) + Twice(2)\n"
                              "Global LIMIT = \"own\"\n"
                              "Print(LIMIT)",
                    library);
    CHECK(errors_of(hiding).empty());
    CHECK(declared_in(hiding, "Helper(1)") == "test.fb");
    CHECK(declared_in(hiding, "Helper(x)") == "lib/b.fb");
    CHECK(declared_in(hiding, "LIMIT") == "test.fb");

    // Overloads that take other types join the imported ones.
    const fb::compilation_t overloads = fb::compile(
        "test.fb", imports + "Function Helper(x: String): String => x\nLocal y = Helper(1)",
        library);
    CHECK(errors_of(overloads).empty());
    CHECK(declared_in(overloads, "Helper(1)") == "lib/b.fb");
}

TEST_CASE("two modules' types of one name stay apart", "[driver]") {
    const std::vector<fb::library_file_t> library = with_std({
        {
            .path = "lib/a.fb",
            .text = "Public Struct Node\n"
                    "    Public Field a: Int\n"
                    "End Struct\n"
                    "Public Function MakeA(): Node => Node(1)\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Public Class Node\n"
                    "    Public Field b: String = \"b\"\n"
                    "End Class\n"
                    "Public Function MakeB(): Node => Node()\n",
        },
    });

    // Each library module finds its own Node first.
    const std::string imports = "Import \"./lib/a\"\nImport \"./lib/b\"\n";
    const fb::compilation_t compilation =
        fb::compile("test.fb",
                    imports + "Import std.core\n"
                              "Local a = MakeA()\nLocal b = MakeB()\nPrint(\"{a.a} {b.b}\")",
                    library);
    CHECK(errors_of(compilation).empty());
    CHECK(qualified_type(compilation, "MakeA()") == "lib.a.Node");
    CHECK(qualified_type(compilation, "MakeB()") == "lib.b.Node");
    CHECK(declared_in(compilation, "Node(1)") == "lib/a.fb");
    CHECK(declared_in(compilation, "Node()") == "lib/b.fb");

    // The main module sees both, so a use that doesn't say which is an error.
    CHECK(errors_of(fb::compile("test.fb", imports + "Local n: Node? = Null", library)) ==
          std::vector<std::string>{
              "test.fb: Node: 'Node' is ambiguous here: lib.a and lib.b each declare it" });

    // Its own Node hides theirs, and a message showing two Nodes says whose each is.
    const fb::compilation_t own = fb::compile("test.fb",
                                              imports + "Import std.core\n"
                                                        "Struct Node\n"
                                                        "    Field c: Int\n"
                                                        "End Struct\n"
                                                        "Local n = Node(3)\n"
                                                        "Local a = MakeA()\n"
                                                        "Print(\"{n.c} {a.a}\")",
                                              library);
    CHECK(errors_of(own).empty());
    CHECK(qualified_type(own, "Node(3)") == "Node");
    CHECK(errors_of(fb::compile("test.fb",
                                imports + "Struct Node\n"
                                          "End Struct\n"
                                          "Local n: Node = MakeA()",
                                library)) ==
          std::vector<std::string>{ "test.fb: MakeA(): expected test.Node, found lib.a.Node" });
}

TEST_CASE("a use reaches what its own module's Extends add, or else its imports'", "[driver]") {
    // a and b each add a Twice to a's Node. b imports a, and c only names Node.
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Struct Node\n"
                    "    Public Field V: Int = 1\n"
                    "End Struct\n"
                    "Extend Node\n"
                    "    Public Method Twice(): Int => V * 2\n"
                    "End Extend\n"
                    "Public Function UseA(): Int => Node(1).Twice()\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Import \"./a\"\n"
                    "Extend Node\n"
                    "    Public Method Twice(): Int => V * 200\n"
                    "End Extend\n"
                    "Public Function UseB(): Int => Node(2).Twice()\n",
        },
        {
            .path = "lib/c.fb",
            .text = "Import \"./a\"\nPublic Function MakeNode(): Node => Node()\n",
        },
    };

    // b's own Twice hides a's in b's code.
    const fb::compilation_t both = fb::compile(
        "test.fb", "Import \"./lib/a\"\nImport \"./lib/b\"\nLocal n = UseA() + UseB()", library);
    CHECK(errors_of(both).empty());
    CHECK(declared_in(both, "Node(1).Twice()") == "lib/a.fb");
    CHECK(declared_in(both, "Node(2).Twice()") == "lib/b.fb");

    // Code that sees only b's reaches it, though a's comes first among Node's members.
    const fb::compilation_t one = fb::compile(
        "test.fb", "Import \"./lib/c\"\nImport \"./lib/b\"\nLocal n = MakeNode().Twice()", library);
    CHECK(errors_of(one).empty());
    CHECK(declared_in(one, "MakeNode().Twice()") == "lib/b.fb");
}

TEST_CASE("a name two imports give is ambiguous wherever it's first checked", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.fb", .text = "Public Const LIMIT = 1\n" },
        { .path = "lib/b.fb", .text = "Public Const LIMIT = 2\n" },
    };

    // The Const Function first runs while types are declared, for a size in one, where its
    // errors aren't kept; checking it again reports the name.
    const std::vector<std::string> errors =
        errors_of(fb::compile("test.fb",
                              "Import \"./lib/a\"\nImport \"./lib/b\"\n"
                              "Const Function Size(): Int => LIMIT + 2\n"
                              "Const N = Size()\n"
                              "Struct Holder\n"
                              "    Field cells: Int[N]\n"
                              "End Struct",
                              library));
    CHECK(std::ranges::count(errors, "test.fb: LIMIT: 'LIMIT' is ambiguous here: lib.a and lib.b "
                                     "each declare it") == 1);
}

TEST_CASE("a name two imports give is ambiguous where sema only tries it", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.fb", .text = "Public Class Node\nEnd Class\n" },
        { .path = "lib/b.fb", .text = "Public Class Node\nEnd Class\n" },
    };
    const std::vector<std::string> ambiguous{
        "test.fb: Node: 'Node' is ambiguous here: lib.a and lib.b each declare it",
    };

    // An Extend of one instance, and a type's attributes, try the name as a type first, and keep
    // what they find.
    const std::string imports = "Import \"./lib/a\"\nImport \"./lib/b\"\n";
    CHECK(errors_of(fb::compile("test.fb",
                                imports + "Class Box<T>\n"
                                          "End Class\n"
                                          "Extend Box<Node>\n"
                                          "    Method Hello(): String => \"hi\"\n"
                                          "End Extend",
                                library)) == ambiguous);
    CHECK(errors_of(fb::compile("test.fb", imports + "Attribute Mine()\nLocal has = Node.Has(Mine)",
                                library)) == ambiguous);
    CHECK(errors_of(fb::compile("test.fb",
                                imports + "Attribute Mine()\nLocal mine = Node.Attribute(Mine)",
                                library)) == ambiguous);
}

TEST_CASE("a type's own members hide a name two imports give", "[driver]") {
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Class Node\nEnd Class\nPublic Const LIMIT = 1\nPublic Global Spawn = 1",
        },
        {
            .path = "lib/b.fb",
            .text = "Public Class Node\nEnd Class\nPublic Const LIMIT = 2\nPublic Global Spawn = 2",
        },
    };

    // Its Methods, and a Func Field, are called by name, and so are ones named like the
    // built-in Spawn and Atomic.
    const std::string imports = "Import \"./lib/a\"\nImport \"./lib/b\"\n";
    const fb::compilation_t compilation =
        fb::compile("test.fb",
                    imports + "Struct P\n"
                              "    Field LIMIT: Func(Int): Int\n"
                              "    Method Node(x: Int): Int => x\n"
                              "    Method Spawn(x: Int): Int => x\n"
                              "    Method Atomic(x: Int): Int => x\n"
                              "    Method Sum(): Int\n"
                              "        Return Node(1) + Spawn(2) + Atomic(3) + LIMIT(4)\n"
                              "    End Method\n"
                              "End Struct",
                    library);
    CHECK(errors_of(compilation).empty());
    for (const std::string_view call : { "Node(1)", "Spawn(2)", "Atomic(3)" }) {
        CHECK(declared_in(compilation, call) == "test.fb");
    }

    // Outside them, the name is still ambiguous.
    CHECK(errors_of(fb::compile("test.fb", imports + "Local n = Node()", library)) ==
          std::vector<std::string>{
              "test.fb: Node: 'Node' is ambiguous here: lib.a and lib.b each declare it" });
}

TEST_CASE("imported Functions that take the same types are ambiguous", "[driver]") {
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Function Helper(x: Int): String => \"a\"\n"
                    "Public Function A(): String => Helper(1)\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Public Function Helper(x: Int): String => \"b\"\n"
                    "Public Function B(): String => Helper(2)\n",
        },
    };

    // Neither import hides the other, whichever loads first, so a call or a function value that
    // doesn't say which is an error. Each module's own code calls its own.
    const std::string imports = "Import \"./lib/a\"\nImport \"./lib/b\"\n";
    const fb::compilation_t compilation =
        fb::compile("test.fb",
                    imports + "Local s = A() + B()\n"
                              "Local t = Helper(0)\n"
                              "Local f: Func(Int): String = Helper",
                    library);
    CHECK(errors_of(compilation) == std::vector<std::string>{
                                        "test.fb: Helper: the call to 'Helper' is ambiguous",
                                        "test.fb: Helper: the function value 'Helper' is ambiguous",
                                    });
    CHECK(declared_in(compilation, "Helper(1)") == "lib/a.fb");
    CHECK(declared_in(compilation, "Helper(2)") == "lib/b.fb");

    // The main module's own Helper hides both.
    const fb::compilation_t own =
        fb::compile("test.fb",
                    imports + "Function Helper(x: Int): String => \"own\"\n"
                              "Local t = Helper(0)\n"
                              "Local f: Func(Int): String = Helper",
                    library);
    CHECK(errors_of(own).empty());
    CHECK(declared_in(own, "Helper(0)") == "test.fb");
}

TEST_CASE("a module's own generic Function hides an imported one of the same shape", "[driver]") {
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Function Pick<T>(items: T[], i: Int): T => items[i]\n"
                    "Public Function First(items: Int[]): Int => Pick(items, 0)\n",
        },
    };

    // Its type parameter has another name, but it takes the same types.
    const fb::compilation_t compilation =
        fb::compile("test.fb",
                    "Import \"./lib/a\"\n"
                    "Function Pick<U>(values: U[], at: Int): U => values[at + 1]\n"
                    "Local x = Pick([1, 2, 3], 0) + First([4, 5])",
                    library);
    CHECK(errors_of(compilation).empty());
    CHECK(declared_in(compilation, "Pick([1, 2, 3], 0)") == "test.fb");
    CHECK(declared_in(compilation, "Pick(items, 0)") == "lib/a.fb");

    // One whose constraints ask more is another overload, which hides nothing.
    const fb::compilation_t constrained = fb::compile("test.fb",
                                                      "Import \"./lib/a\"\n"
                                                      "Interface Ranked\n"
                                                      "End Interface\n"
                                                      "Struct P\n"
                                                      "    Field x: Int\n"
                                                      "End Struct\n"
                                                      "Function Pick<U>(values: U[], at: Int): U\n"
                                                      "    Where U: Ranked\n"
                                                      "    Return values[at + 1]\n"
                                                      "End Function\n"
                                                      "Local p = Pick([P(1)], 0)",
                                                      library);
    CHECK(errors_of(constrained).empty());
    CHECK(declared_in(constrained, "Pick([P(1)], 0)") == "lib/a.fb");
}

TEST_CASE("messages tell two modules' types of one name apart", "[driver]") {
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Struct Node\n"
                    "    Public Field n: Int\n"
                    "    Operator +(a: Node, b: Node): Node => Node(a.n + b.n)\n"
                    "End Struct\n"
                    "Public Interface Taker\n"
                    "    Method Take(n: Node): Node\n"
                    "End Interface\n"
                    "Public Class Pet\n"
                    "End Class\n"
                    "Public Function MakeA(): Node => Node(1)\n"
                    "Public Function MaybeA(): Node? => Node(1)\n"
                    "Public Function TakeA(n: Node?): Int => 1\n"
                    "Public Function Fill(ByRef n: Node)\n"
                    "    n = Node(5)\n"
                    "End Function\n"
                    "Public Function MakePet(): Pet => Pet()\n"
                    "Public Function Join(x: Node, y: Node): Int => x.n + y.n\n"
                    "Public Function Join(x: Int, y: Int): Int => x + y\n",
        },
    };
    const std::string own = "Import \"./lib/a\"\n"
                            "Struct Node\n"
                            "    Field n: Int\n"
                            "End Struct\n"
                            "Class Pet\n"
                            "End Class\n"
                            "Local mine = Node(1)\n"
                            "Local a = MakeA()\n";
    const auto errors = [&](const std::string_view code) {
        return errors_of(fb::compile("test.fb", own + std::string(code), library));
    };

    CHECK(errors("Fill(ByRef mine)") ==
          std::vector<std::string>{ "test.fb: mine: a ByRef argument must be exactly lib.a.Node, "
                                    "but this is test.Node" });
    CHECK(errors("Local c = True ? a : mine") ==
          std::vector<std::string>{ "test.fb: True ? a : mine: the results of '?' have different "
                                    "types, lib.a.Node and test.Node" });
    CHECK(errors("Local xs = [a, mine]") ==
          std::vector<std::string>{ "test.fb: mine: the elements of an array have different "
                                    "types, lib.a.Node and test.Node" });
    CHECK(errors("Local p = MakePet()\nLocal b = p Is Pet") ==
          std::vector<std::string>{ "test.fb: p Is Pet: lib.a.Pet and test.Pet aren't related, so "
                                    "this is always False" });
    CHECK(errors("Local j = Join(a, mine)") ==
          std::vector<std::string>{
              "test.fb: Join: no overload of 'Join' takes (lib.a.Node, test.Node)" });
    CHECK(errors("Local s = mine + a") ==
          std::vector<std::string>{
              "test.fb: mine + a: no Operator + takes (test.Node, lib.a.Node)" });

    // Inside another type, like a T?, too.
    CHECK(errors("Local n: Node = MaybeA()") ==
          std::vector<std::string>{ "test.fb: MaybeA(): expected test.Node, found lib.a.Node?" });
    CHECK(errors("Local t = TakeA(mine)") ==
          std::vector<std::string>{ "test.fb: mine: expected lib.a.Node?, found test.Node" });

    // And beside no other Node, when Node means the code's own here.
    CHECK(errors("Local i: Int = MakeA()") ==
          std::vector<std::string>{ "test.fb: MakeA(): expected Int32, found lib.a.Node" });
    CHECK(errors("Local p: Int = MakePet()") ==
          std::vector<std::string>{ "test.fb: MakePet(): expected Int32, found lib.a.Pet" });

    // A Method that doesn't match shows the Interface's types beside its own.
    CHECK(errors("Struct Mine Implements Taker\n"
                 "    Method Take(n: Node): Node => n\n"
                 "End Struct") ==
          std::vector<std::string>{ "test.fb: Taker: Mine's 'Take' doesn't match Taker's, which "
                                    "takes (lib.a.Node) and returns lib.a.Node" });
}

TEST_CASE("the reasons in messages tell two modules' types of one name apart", "[driver]") {
    // With std, whose Sendable the checks ask.
    std::expected<std::vector<fb::library_file_t>, std::string> library = fb::standard_library();
    REQUIRE(library);
    library->push_back({
        .path = "lib/a.fb",
        .text = "Import \"./b\"\n"
                "Public Class Node\n"
                "End Class\n"
                "Public Struct Wrapper\n"
                "    Public Field N: Node\n"
                "End Struct\n"
                "Public Function MakeWrapper(): Wrapper => Wrapper(Node())\n"
                "Public Struct Panel\n"
                "    Public Static Method Run(): Int => DrawB()\n"
                "    Public Static Method Tally(): Int => CountB()\n"
                "End Struct\n"
                "Public Function RunA(): Int => Panel.Run()\n"
                "Public Function TallyA(): Int => Panel.Tally()\n",
    });
    library->push_back({
        .path = "lib/b.fb",
        .text = "Public Struct Panel\n"
                "    Public Static Field Count: Int = 0\n"
                "    @MainThread Static Method Draw(): Int => 1\n"
                "End Struct\n"
                "Public Function DrawB(): Int => Panel.Draw()\n"
                "Public Function CountB(): Int => Panel.Count\n",
    });
    const auto errors = [&](const std::string_view code) {
        return errors_of(fb::compile(
            "test.fb", "Import \"./lib/a\"\nImport \"./lib/b\"\n" + std::string(code), *library));
    };

    // Which field misses a constraint on fields.
    CHECK(errors("Interface Node\n"
                 "End Interface\n"
                 "Function R<T>(x: T): Int\n"
                 "    For Field f: F In T\n"
                 "        Where F: Node\n"
                 "    End For\n"
                 "    Return 1\n"
                 "End Function\n"
                 "Local r = R(MakeWrapper())") ==
          std::vector<std::string>{ "test.fb: R: 'R' needs each field of Wrapper to implement "
                                    "test.Node, but its field 'N' is lib.a.Node, which doesn't" });

    // Why a type isn't Sendable, through the fields on the way.
    CHECK(errors("Import std.core\n"
                 "Struct Node\n"
                 "    Field W: Wrapper\n"
                 "End Struct\n"
                 "Function Send<T>(x: T): Int\n"
                 "    Where T: Sendable\n"
                 "    Return 1\n"
                 "End Function\n"
                 "Local k = Send(Node(MakeWrapper()))") ==
          std::vector<std::string>{
              "test.fb: Send: test.Node isn't Sendable, which 'Send' needs for T: its field 'W' is "
              "Wrapper, whose field 'N' is lib.a.Node, and a Class's objects stay on the thread "
              "that made them, unless it's a Shared Class" });

    // The calls on the way to what only the main thread may do.
    CHECK(errors("Local t = Spawn(Function() => RunA())\nLocal n = t.Wait()") ==
          std::vector<std::string>{
              "test.fb: Spawn: this work runs on another thread, but it calls 'RunA', which "
              "calls 'lib.a.Panel.Run', which calls 'DrawB', which calls 'lib.b.Panel.Draw', "
              "which is @MainThread" });
    CHECK(errors("Local t = Spawn(Function() => TallyA())\nLocal n = t.Wait()") ==
          std::vector<std::string>{
              "test.fb: Spawn: this work runs on another thread, but it calls 'TallyA', which "
              "calls 'lib.a.Panel.Tally', which calls 'CountB', which uses the Static Field "
              "'lib.b.Panel.Count', which only the main thread can, unless it's an Atomic or a "
              "Mutex" });
}

TEST_CASE("only the main module's top-level code runs", "[driver]") {
    const std::vector<fb::library_file_t> library = with_std({
        {
            .path = "lib/a.fb",
            .text = "Import std.core\n"
                    "Public Global count = 1\nPrint(\"x\")\nLocal y = 2\n",
        },
    });
    CHECK(errors_of(fb::compile("test.fb",
                                "Import std.core\n"
                                "Import \"./lib/a\"\nPrint(\"{count}\")",
                                library)) ==
          std::vector<std::string>{
              "lib/a.fb: Print(\"x\"): only the main module's top-level code runs, so this "
              "module only declares things",
              "lib/a.fb: Local y = 2: only the main module's top-level code runs, so this module "
              "only declares things",
          });
}

TEST_CASE("a library file's errors point into it", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.fb", .text = "Public Function F(): Int => \"a\"\n" },
    };
    CHECK(errors_of(fb::compile("test.fb", "Import \"./lib/a\"\nLocal x = 1", library)) ==
          std::vector<std::string>{ "lib/a.fb: \"a\": expected Int32, found String" });
}

TEST_CASE("a program without imports loads only std/core and std.collections.map", "[driver]") {
    const fb::compilation_t compilation = fb::compile("test.fb", "Local m = {\"a\": 1}");
    CHECK(errors_of(compilation).empty());
    std::vector<std::string> names;
    for (const fb::module_t& module : compilation.ast.modules().modules()) {
        names.push_back(module.name);
    }

    // std/core/'s initialize first, each after what it imports, as std.core.value does after
    // the Map it holds, and std.core.load after the Value it converts, and the main module last.
    CHECK(names == std::vector<std::string>{
                       "std.core.interfaces",
                       "std.core.assert",
                       "std.core.error",
                       "std.core.iterator",
                       "std.core.format",
                       "std.core.values",
                       "std.core.parse",
                       "std.core.print",
                       "std.core.range",
                       "std.collections.map",
                       "std.core.value",
                       "std.core.load",
                       "test",
                   });
}

TEST_CASE("a program links the bundled C libraries of the runtime functions it calls", "[driver]") {
    const auto bundled = [](const std::string_view text) {
        const fb::compilation_t compilation = fb::compile("test.fb", std::string(text));
        CHECK(errors_of(compilation).empty());
        std::vector<std::string_view> names;
        for (const fb::build_config::bundled_library_t* library :
             fb::bundled_libraries(fb::lower(compilation.ast, compilation.analysis))) {
            names.push_back(library->name);
        }

        return names;
    };

    // Writing to the console doesn't need the I/O thread; reading does.
    CHECK(bundled("Import std.core\nPrint(\"hello\")\nPrintError(1)\nFlush()").empty());
    CHECK(bundled("Import std.core\nPrint(Input() ?? \"none\")") ==
          std::vector<std::string_view>{ "libuv" });
    CHECK(bundled("Import std.core\nLocal line = Await InputAsync()") ==
          std::vector<std::string_view>{ "libuv" });
    // A lookup goes through it too; a Url or an address is std.net's own.
    CHECK(bundled("Import std.net\nLocal found = Try? Dns.Resolve(\"localhost\")") ==
          std::vector<std::string_view>{ "libuv" });
    CHECK(bundled("Import std.net\nLocal url = Url.Parse(\"https://example.com/\")\n"
                  "Local address = IpAddress.Parse(\"::1\")")
              .empty());
    // std.db is over SQLite, and a random Uuid comes from SecureRandom, over mbedTLS.
    CHECK(bundled("Import std.db\nLocal db = Try? Database.Open(\":memory:\")") ==
          std::vector<std::string_view>{ "SQLite" });
    CHECK(bundled("Import std.uuid\nLocal id = Uuid.Parse(\"\")").empty());
    CHECK(bundled("Import std.uuid\nLocal id = Uuid.V4()") ==
          std::vector<std::string_view>{ "mbedTLS" });
    // std.net.http's client links libcurl, and libuv and mbedTLS, which libcurl's part of the
    // runtime is built on, and zlib-ng, zstd, and nghttp2, which libcurl is, in the table's order;
    // its Headers and Bodies need none of them.
    CHECK(bundled("Import std.core\nImport std.net.http\nLocal h = Headers()\nh.Add(\"A\", \"b\")\n"
                  "Print(Body.Form({\"a\": \"b c\"}).Bytes.Count)")
              .empty());
    CHECK(bundled("Import std.net.http\nLocal client = HttpClient()") ==
          std::vector<std::string_view>{ "libuv", "mbedTLS", "zlib-ng", "zstd", "nghttp2",
                                         "libcurl" });
    // std.os is over libuv too, through a prefix of its own.
    CHECK(bundled("Import std.core\nImport std.os\nPrint(Env.Get(\"HOME\") ?? \"none\")") ==
          std::vector<std::string_view>{ "libuv" });
    // std.net.tls links mbedTLS's TLS, and mbedTLS, which it's a part of; a TLS stream over
    // something other than a socket needs no libuv.
    CHECK(bundled("Import std.net.tls\nLocal client = TlsClient()") ==
          std::vector<std::string_view>{ "mbedTLS", "mbedTLS's TLS" });
}

TEST_CASE("a bundled C library's notices have the texts of every license it names", "[driver]") {
    const auto notices = [](const std::string_view name) {
        const auto library = std::ranges::find(fb::build_config::bundled_libraries, name,
                                               &fb::build_config::bundled_library_t::name);
        REQUIRE(library != fb::build_config::bundled_libraries.end());
        std::ifstream file(fb::path_from_utf8(library->notice), std::ios::binary);
        REQUIRE(file);
        // CMake writes it with the OS's line breaks.
        std::string text(std::istreambuf_iterator<char>(file), {});
        std::erase(text, '\r');
        return std::pair{ std::string(library->license), text };
    };

    // libcurl's curlx/inet_ntop.c and inet_pton.c are ISC's.
    const auto [curl_license, curl] = notices("libcurl");
    CHECK(curl_license == "the curl and ISC licenses");
    CHECK(curl.contains("COPYRIGHT AND PERMISSION NOTICE"));
    CHECK(curl.contains("lib/curlx/inet_ntop.c:\n\n/*\n * Copyright (C) 1996-2022  Internet "
                        "Software Consortium."));
    CHECK(curl.contains("lib/curlx/inet_pton.c:\n\n/* This is from the BIND 4.9.4 release"));
    CHECK(curl.contains(" * SPDX-License-Identifier: ISC\n */\n\nlib/curlx/inet_pton.c"));
    CHECK(!curl.contains("#include"));
    // nghttp2's own, then those of the code others have copyrights in.
    const auto [nghttp2_license, nghttp2] = notices("nghttp2");
    CHECK(nghttp2_license == "the MIT license");
    CHECK(nghttp2.starts_with("The MIT License\n\nCopyright (c) 2012, 2014, 2015, 2016 Tatsuhiro"));
    CHECK(nghttp2.contains("ngtcp2 contributors"));
    CHECK(nghttp2.contains("nghttp3 contributors"));
    CHECK(nghttp2.contains("sfparse contributors"));
    CHECK(nghttp2.contains("lib/sfparse.c's UTF-8 decoder:\n\n/* Copyright (c) 2008-2010 Bjoern "
                           "Hoehrmann"));
    CHECK(!nghttp2.contains("#include"));
}

TEST_CASE("std.compress's formats link the C libraries they're over, with their notices",
          "[driver]") {
    const auto bundled = [](const std::string_view code) {
        const fb::compilation_t compilation = fb::compile(
            "test.fb", std::format("Import std.core\nImport std.compress\nLocal none: UInt8[] = "
                                   "[]\n{}\n",
                                   code));
        CHECK(errors_of(compilation).empty());
        std::vector<std::string_view> names;
        for (const fb::build_config::bundled_library_t* library :
             fb::bundled_libraries(fb::lower(compilation.ast, compilation.analysis))) {
            names.push_back(library->name);
        }

        return names;
    };
    using names_t = std::vector<std::string_view>;

    CHECK(bundled("Print(Gzip.Compress(none).Count, Deflate.Compress(none).Count)") ==
          names_t{ "zlib-ng" });
    CHECK(bundled("Print(Zstd.Compress(none).Count)") == names_t{ "zstd" });
    CHECK(bundled("Print(Lz4Compressor().Finish().Count)") == names_t{ "LZ4" });
    CHECK(bundled("Print((Try XZ.Decompress(none)).Count)") == names_t{ "liblzma" });
    CHECK(bundled("Print((Try SevenZip.Read(none)).Count)") == names_t{ "liblzma" });
    CHECK(bundled("Print(ZipWriter().Finish().Count)") == names_t{ "minizip-ng" });
    // A tar needs no library, but reading one decompresses it first, when it's compressed.
    CHECK(bundled("Print(TarWriter().Finish().Count)").empty());
    CHECK(bundled("Print((Try Tar.Read(none)).Count)") ==
          names_t{ "zlib-ng", "zstd", "LZ4", "liblzma" });

    // Each notice has its library's license, and minizip-ng's has those of the libraries a zip
    // links with it, after the names its files' comments give.
    const auto notice = [](const std::string_view name) {
        for (const fb::build_config::bundled_library_t& library :
             fb::build_config::bundled_libraries) {
            if (library.name == name) {
                std::ifstream file(fb::path_from_utf8(library.notice), std::ios::binary);
                return std::string{ std::istreambuf_iterator<char>(file), {} };
            }
        }

        return std::string();
    };
    CHECK(notice("zlib-ng").contains("Jean-loup Gailly and Mark Adler"));
    CHECK(notice("zstd").contains("Meta Platforms"));
    CHECK(notice("LZ4").contains("Yann Collet"));
    CHECK(notice("liblzma").contains("liblzma is under the BSD Zero Clause License (0BSD)"));
    const std::string minizip = notice("minizip-ng");
    CHECK(minizip.contains("Nathan Moinvaziri"));
    CHECK(minizip.contains("Gilles Vollant"));
    CHECK(minizip.contains("Jean-loup Gailly and Mark Adler"));
    CHECK(minizip.contains("Meta Platforms"));
    CHECK(minizip.contains("BSD Zero Clause License"));
}

TEST_CASE("std.image's formats link the C libraries they're over, with their notices", "[driver]") {
    const auto bundled = [](const std::string_view code) {
        const fb::compilation_t compilation = fb::compile(
            "test.fb", std::format("Import std.core\nImport std.math\nImport std.image\nLocal "
                                   "none: UInt8[] = []\nLocal image = Image(1, 1)\n{}\n",
                                   code));
        CHECK(errors_of(compilation).empty());
        std::vector<std::string_view> names;
        for (const fb::build_config::bundled_library_t* library :
             fb::bundled_libraries(fb::lower(compilation.ast, compilation.analysis))) {
            names.push_back(library->name);
        }

        return names;
    };
    using names_t = std::vector<std::string_view>;

    CHECK(bundled("Print((Try Png.Decode(none)).Width, (Try Jpeg.Decode(none)).Width)") ==
          names_t{ "stb_image" });
    CHECK(bundled("Print(Jpeg.Encode(image).Count, Bmp.Encode(image).Count)") ==
          names_t{ "stb_image" });
    CHECK(bundled("Print((Try Png.DecodeAnimation(none)).Count)") == names_t{ "stb_image" });
    // PNG's encoders are the runtime's own, over zlib-ng.
    CHECK(bundled("Print(Png.Encode(image).Count)") == names_t{ "zlib-ng" });
    CHECK(bundled("Print(Png.EncodeAnimation(Animation(1, 1)).Count)") == names_t{ "zlib-ng" });
    CHECK(bundled("Print((Try WebP.Decode(none)).Width, WebP.Encode(image).Count)") ==
          names_t{ "libwebp" });
    CHECK(bundled("Print((Try Svg.Parse(\"\")).Width)") == names_t{ "nanosvg" });
    // QOI and GIF are the runtime's own, and need no library.
    CHECK(bundled("Print(Qoi.Encode(image).Count, (Try Qoi.Decode(none)).Width)").empty());
    CHECK(bundled("Print(Gif.Encode(image).Count, (Try Gif.DecodeAnimation(none)).Count)").empty());
    // So is drawing.
    CHECK(bundled("Local canvas = Canvas(image)\ncanvas.FillCircle(Vec2(0, 0), 1, Color32.Black)\n"
                  "canvas.StrokePolyline([Vec2(0, 0), Vec2(1, 1)], Color32.Black)")
              .empty());
    // Image.Decode reads every format, so it links each decoder's library.
    CHECK(bundled("Print((Try Image.Decode(none)).Width)") == names_t{ "stb_image", "libwebp" });
    // Loading a file reads it through std.io, over libuv, which nothing else in std.image needs.
    CHECK(bundled("Print((Try Image.Load(\"a.png\")).Width)") ==
          names_t{ "libuv", "stb_image", "libwebp" });
    CHECK(bundled("Print((Try Font.Load(\"a.ttf\")).UnitsPerEm)") == names_t{ "libuv" });

    const auto notice = [](const std::string_view name) {
        for (const fb::build_config::bundled_library_t& library :
             fb::build_config::bundled_libraries) {
            if (library.name == name) {
                std::ifstream file(fb::path_from_utf8(library.notice), std::ios::binary);
                return std::string{ std::istreambuf_iterator<char>(file), {} };
            }
        }

        return std::string();
    };
    CHECK(notice("stb_image").contains("Copyright (c) 2017 Sean Barrett"));
    CHECK(notice("libwebp").contains("Copyright (c) 2010, Google Inc."));
    CHECK(notice("libwebp").contains("Additional IP Rights Grant (Patents)"));
    CHECK(notice("nanosvg").contains("Copyright (c) 2013-14 Mikko Mononen"));
}

TEST_CASE("the bundled C libraries are optimized in every build", "[driver]") {
    // The build's compile_commands.json has each C file's command. Those of a bundled library
    // that third_party/ gives -O2 keep it to the end, and have no -g3 after it.
    std::ifstream commands(FB_TEST_COMPILE_COMMANDS);
    if (!commands) {
        SKIP("this build has no compile_commands.json");
    }

    std::size_t optimized = 0;
    std::string command;
    for (std::string line; std::getline(commands, line);) {
        if (line.contains("\"command\":")) {
            command = line;
            continue;
        }
        if (!line.contains("\"file\":") || !line.contains("/_deps/") || !line.contains(".c\"")) {
            continue;
        }

        std::string_view last_optimization;
        std::string_view last_debug;
        bool bundled = false;
        for (const auto part : std::views::split(std::string_view(command), ' ')) {
            const std::string_view option(part.begin(), part.end());
            bundled = bundled || option == "-O2";
            if (option.starts_with("-O")) {
                last_optimization = option;
            } else if (option.starts_with("-g")) {
                last_debug = option;
            }
        }
        if (bundled) {
            INFO(line);
            CHECK(last_optimization != "-O0");
            CHECK(last_debug != "-g3");
            ++optimized;
        }
    }

    CHECK(optimized > 0);
}

TEST_CASE("resolve_import() maps dotted names and relative paths", "[driver]") {
    const auto paths = [](const std::string_view target, const bool quoted,
                          const std::string_view importer) {
        const std::expected<fb::import_paths_t, std::string> found =
            fb::resolve_import(target, quoted, importer);
        REQUIRE(found);
        return std::format("{} {}", found->file, found->folder.value_or("-"));
    };

    // A dotted name is a std file, or else the folder of that name.
    CHECK(paths("std.collections", false, "main.fb") == "std/collections.fb std/collections");
    CHECK(paths("std.collections.map", false, "ui/hud.fb") ==
          "std/collections/map.fb std/collections/map");

    // A quoted path is a file from the importer's folder.
    CHECK(paths("./ui/hud", true, "main.fb") == "ui/hud.fb -");
    CHECK(paths("ui/hud", true, "main.fb") == "ui/hud.fb -");
    CHECK(paths("../shared/log", true, "ui/hud.fb") == "shared/log.fb -");
    CHECK(paths("./map", true, "std/collections/set.fb") == "std/collections/map.fb -");

    // It stays in the main file's folder, or in std/ for std's own files.
    const auto refused = [](const std::string_view target, const bool quoted,
                            const std::string_view importer) {
        const std::expected<fb::import_paths_t, std::string> found =
            fb::resolve_import(target, quoted, importer);
        return found ? std::string() : found.error();
    };
    CHECK(refused("../x", true, "main.fb") ==
          "\"../x\" leaves the main file's folder, where the paths of this program's modules "
          "start");
    CHECK(refused("../../x", true, "std/core/error.fb") ==
          "\"../../x\" leaves std/, where the paths of this program's modules start");
    CHECK(refused("/abs/x", true, "main.fb") ==
          "\"/abs/x\" isn't a path from this file's folder, like \"./hud\" or \"../ui/hud\"");
    CHECK(refused("physics", false, "main.fb") ==
          "there's no module physics: a module's name starts with std, like std.collections, or "
          "a dependency's name in project.toml, and the program's own files are imported by "
          "their paths, like \"./physics\"");

    // std is the standard library's, as a whole and as a folder's name.
    CHECK(refused("std", false, "main.fb") ==
          "std is the standard library's folder; import one of its modules, like "
          "std.collections");
    CHECK(refused("./std/x", true, "main.fb") ==
          "\"./std/x\" names a file in a folder called std, which is the standard library's "
          "name");
}

TEST_CASE("a folder's import brings in its own files, not its subfolders'", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "std/extra/one.fb", .text = "Public Function One(): Int => 1\n" },
        { .path = "std/extra/two.fb", .text = "Public Function Two(): Int => 2\n" },
        { .path = "std/extra/deep/three.fb", .text = "Public Function Three(): Int => 3\n" },
    };
    const fb::compilation_t compilation =
        fb::compile("main.fb", "Import std.extra\nLocal n = One() + Two()", library);
    CHECK(errors_of(compilation).empty());
    std::vector<std::string> names;
    for (const fb::module_t& module : compilation.ast.modules().modules()) {
        names.push_back(module.name);
    }

    CHECK(names == std::vector<std::string>{ "std.extra.one", "std.extra.two", "main" });
    CHECK(errors_of(fb::compile("main.fb", "Import std.extra\nLocal n = Three()", library)) ==
          std::vector<std::string>{
              "main.fb: Three: 'Three' is in std.extra.deep, which this file doesn't import" });
}

TEST_CASE("a built-in type's Methods and Properties from std say which module they're in",
          "[driver]") {
    const std::vector<fb::library_file_t> library{
        {
            .path = "std/extra/words.fb",
            .text = "Extend String\n"
                    "    Public Property Size: Int\n"
                    "        Get => 1\n"
                    "    End Property\n"
                    "    Public Method Twice(): String => Self + Self\n"
                    "End Extend\n",
        },
    };
    CHECK(errors_of(
              fb::compile("main.fb", "Local n = \"a\".Size\nLocal t = \"a\".Twice()", library)) ==
          std::vector<std::string>{
              "main.fb: Size: 'Size' is in std.extra, which this file doesn't import",
              "main.fb: Twice: 'Twice' is in std.extra, which this file doesn't import",
          });
    CHECK(errors_of(fb::compile("main.fb", "Import std.extra\nLocal n = \"a\".Size", library))
              .empty());
}

TEST_CASE("a missing module lists the paths tried", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.fb", .text = "" },
    };
    CHECK(errors_of(fb::compile("test.fb", "Import std.nothing\nImport \"./lib/b\"", library)) ==
          std::vector<std::string>{
              "test.fb: std.nothing: there's no module std.nothing: no std/nothing.fb, and no "
              "folder std/nothing/ of modules",
              "test.fb: \"./lib/b\": there's no module \"./lib/b\": no lib/b.fb",
          });
}

TEST_CASE("two files can't be one module", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.b.fb", .text = "" },
        { .path = "lib/a/b.fb", .text = "" },
    };
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Import \"./lib/a.b\"\nImport \"./lib/a/b\"", library);
    CHECK(errors_of(compilation) ==
          std::vector<std::string>{ "test.fb: \"./lib/a/b\": lib/a/b.fb's module is named "
                                    "lib.a.b, and so is lib/a.b.fb's; rename one of them" });
}

TEST_CASE("imports load each file once, and initialize before the modules that import them",
          "[driver]") {
    // a and b import each other, and b imports c by its path. Main imports a, so b's
    // Globals initialize first, then a's.
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.fb", .text = "Import \"./b\"\nImport \"../lib/./b\"\n" },
        { .path = "lib/b.fb", .text = "Import \"./a\"\nImport \"./c\"\n" },
        { .path = "lib/c.fb", .text = "" },
        { .path = "lib/unused.fb", .text = "" },
    };
    const fb::compilation_t compilation =
        fb::compile("main.fb", "Import \"./lib/a\"\nImport \"./lib/c\"", library);
    CHECK(errors_of(compilation).empty());
    std::vector<std::string> names;
    for (const fb::module_t& module : compilation.ast.modules().modules()) {
        names.push_back(module.name);
    }

    CHECK(names == std::vector<std::string>{ "lib.c", "lib.b", "lib.a", "main" });
    CHECK(compilation.sources.size() == 4);

    // Each import says which modules it names. std/core/ is loaded, but only an Import of it
    // brings in its names.
    const fb::module_t& main = compilation.ast.modules().modules().back();
    REQUIRE(main.imports.size() == 2);
    CHECK(main.imports[0].modules == std::vector{ fb::module_id_t{ 2 } });
    CHECK(main.imports[1].modules == std::vector{ fb::module_id_t{ 0 } });
}

TEST_CASE("compile_file finds imported files beside the main file", "[driver]") {
    const std::filesystem::path folder = fb::temporary_path("modules", "");
    std::filesystem::create_directories(folder / "ui");
    const auto write = [&](const std::filesystem::path& path, const std::string_view text) {
        std::ofstream file(folder / path, std::ios::binary);
        file << text;
    };
    write("main.fb", "Import std.core\n"
                     "Import \"./ui/hud\"\nPrint(Title())\n");
    write("ui/hud.fb", "Public Function Title(): String => 1\n");

    const std::expected<fb::compilation_t, std::string> compiled =
        fb::compile_file(folder / "main.fb");
    REQUIRE(compiled);
    const std::vector<std::string> errors = errors_of(*compiled);
    REQUIRE(errors.size() == 1);
    // The imported file is named by its path beside the main file's.
    CHECK(errors.front() ==
          std::format("{}/ui/hud.fb: 1: expected String, found Int32",
                      compiled->source().path().substr(0, compiled->source().path().rfind('/'))));
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

TEST_CASE("a file reached by paths that differ only in letter case loads once", "[driver]") {
    const std::filesystem::path folder = fb::temporary_path("cases", "");
    std::filesystem::create_directories(folder / "ui");
    const auto write = [&](const std::filesystem::path& path, const std::string_view text) {
        std::ofstream file(folder / path, std::ios::binary);
        file << text;
    };
    write("main.fb",
          "Import \"./UI/Hud\"\nImport \"./ui/other\"\nLocal n: Hud.Node = MakeNode()\n");
    write("ui/hud.fb", "Public Class Node\nEnd Class\n");
    write("ui/other.fb", "Import \"./hud\"\nPublic Function MakeNode(): Node => Node()\n");

    // Only a disk that ignores letter case, as Windows' and macOS's usually do, has UI/Hud.fb.
    if (std::filesystem::exists(folder / "UI" / "Hud.fb")) {
        const std::expected<fb::compilation_t, std::string> compiled =
            fb::compile_file(folder / "main.fb");
        REQUIRE(compiled);
        // The Import that spells it otherwise gets a warning, since a disk that tells letter
        // cases apart wouldn't find it.
        CHECK(errors_of(*compiled) ==
              std::vector<std::string>{ std::format(
                  "{}: \"./UI/Hud\": the disk spells it \"./ui/hud\", in other letter cases, "
                  "which only a disk that ignores them finds",
                  compiled->source().path()) });
        std::vector<std::string> huds;
        for (const fb::module_t& module : compiled->ast.modules().modules()) {
            if (module.name.ends_with("Hud") || module.name.ends_with("hud")) {
                huds.push_back(module.name);
            }
        }

        // It's named by the path that reached it first.
        CHECK(huds == std::vector<std::string>{ "UI.Hud" });
    }

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

TEST_CASE("a .proto's import in other letter cases than the disk's gets a warning", "[driver]") {
    const std::filesystem::path folder = fb::temporary_path("proto_cases", "");
    std::filesystem::create_directories(folder / "protos" / "common");
    const auto write = [&](const std::filesystem::path& path, const std::string_view text) {
        std::ofstream file(folder / path, std::ios::binary);
        file << text;
    };
    write("main.fb", "Import \"protos/player.proto\"\nLocal p = Player()\n");
    write("protos/player.proto", "syntax = \"proto3\";\nimport \"Common/IDs.proto\";\n"
                                 "message Player { common.Id id = 1; }\n");
    write("protos/common/ids.proto",
          "syntax = \"proto3\";\npackage common;\nmessage Id { string value = 1; }\n");

    // Only a disk that ignores letter case finds Common/IDs.proto, as it does an Import's path.
    if (std::filesystem::exists(folder / "protos" / "Common" / "IDs.proto")) {
        const std::expected<fb::compilation_t, std::string> compiled =
            fb::compile_file(folder / "main.fb");
        REQUIRE(compiled);
        const std::vector<std::string> errors = errors_of(*compiled);
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().ends_with(
            "player.proto: \"Common/IDs.proto\": the disk spells it \"common/ids.proto\", in other "
            "letter cases, which only a disk that ignores them finds"));
    }

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

TEST_CASE("a main file given without its folder is in the current one", "[driver]") {
    const std::filesystem::path folder = fb::temporary_path("bare", "");
    std::filesystem::create_directories(folder / "ui");
    std::ofstream(folder / "main.fb", std::ios::binary)
        << "Import std.core\n"
           "Import \"./UI/Hud\"\nPrint(\"{Width()}\")\n";
    std::ofstream(folder / "ui" / "hud.fb", std::ios::binary)
        << "Public Function Width(): Int => 3\n";
    std::error_code ignored;
    const std::filesystem::path before = std::filesystem::current_path(ignored);
    std::filesystem::current_path(folder, ignored);
    const std::expected<fb::compilation_t, std::string> compiled = fb::compile_file("main.fb");
    std::filesystem::current_path(before, ignored);
    REQUIRE(compiled);
    if (std::filesystem::exists(folder / "UI" / "Hud.fb")) {
        CHECK(
            errors_of(*compiled) ==
            std::vector<std::string>{ "main.fb: \"./UI/Hud\": the disk spells it \"./ui/hud\", in "
                                      "other letter cases, which only a disk that ignores them "
                                      "finds" });
    } else {
        CHECK(errors_of(*compiled).size() == 2);
    }

    std::filesystem::remove_all(folder, ignored);
}

TEST_CASE("a build defines DEBUG or RELEASE, and the platform it's built on", "[driver]") {
#ifdef _WIN32
    constexpr std::string_view platform = "PLATFORM_WINDOWS";
#elifdef __APPLE__
    constexpr std::string_view platform = "PLATFORM_MACOS";
#else
    constexpr std::string_view platform = "PLATFORM_LINUX";
#endif
    CHECK(fb::defined_symbols(fb::build_mode_t::DEBUG) ==
          std::vector<std::string_view>{ "DEBUG", platform });
    CHECK(fb::defined_symbols(fb::build_mode_t::RELEASE) ==
          std::vector<std::string_view>{ "RELEASE", platform });

    // Each branch reads a name of its own that isn't declared, so the errors say which ones a
    // build takes: one mode and one platform.
    const std::filesystem::path path = fb::temporary_path("symbols", ".fb");
    std::string text;
    for (const std::string_view symbol :
         { "DEBUG", "RELEASE", "PLATFORM_WINDOWS", "PLATFORM_LINUX", "PLATFORM_MACOS" }) {
        text += std::format("#If {0}\nLocal is_{0} = taken_{0}\n#End If\n", symbol);
    }

    std::ofstream(path, std::ios::binary) << text;
    for (const auto& [mode, symbol] : {
             std::pair(fb::build_mode_t::DEBUG, "DEBUG"),
             std::pair(fb::build_mode_t::RELEASE, "RELEASE"),
         }) {
        const std::expected<fb::compilation_t, std::string> compiled = fb::compile_file(path, mode);
        REQUIRE(compiled);
        std::vector<std::string> taken;
        for (const fb::diagnostic_t& diagnostic : compiled->diagnostics.diagnostics()) {
            taken.push_back(diagnostic.message);
        }

        CHECK(taken == std::vector<std::string>{
                           std::format("'taken_{}' isn't declared", symbol),
                           std::format("'taken_{}' isn't declared", platform),
                       });
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("compile_file reports a file it can't read", "[driver]") {
    const std::expected<fb::compilation_t, std::string> compiled =
        fb::compile_file("no such file.fb");
    REQUIRE_FALSE(compiled);
    CHECK(compiled.error().starts_with("cannot read 'no such file.fb'"));
}

TEST_CASE("temporary paths are new each time", "[driver]") {
    const std::filesystem::path first = fb::temporary_path("hello", ".exe");
    const std::filesystem::path second = fb::temporary_path("hello", ".exe");
    CHECK(first != second);
    CHECK(first.extension() == ".exe");
    CHECK(first.stem().string().starts_with("hello-"));
    CHECK(first.parent_path().filename() == "futureblitz");
    CHECK(std::filesystem::is_directory(first.parent_path()));
}

TEST_CASE("std.math.big's Complex doesn't change once it's made", "[driver]") {
    CHECK(errors_of(fb::compile("main.fb",
                                "Import std.math.big\n"
                                "Local z = Complex(1, 2)\n"
                                "z.Real = 5.0\n",
                                with_std({}))) ==
          std::vector<std::string>{
              "main.fb: z.Real: 'Real' is ReadOnly; only Complex's New can change it",
          });
}

TEST_CASE("a temporary folder is removed once what holds its files lets go", "[driver]") {
    const std::filesystem::path folder = fb::temporary_path("held", "");
    std::filesystem::create_directories(folder);
    const std::filesystem::path file = folder / "program.exe";
    std::ofstream(file) << "built";
#ifdef _WIN32
    // As Windows holds a program's executable for a moment after it ends, or a scanner does
    // while it reads one: opened without sharing, so it can't be deleted until it's let go.
    int handle = -1;
    REQUIRE(_wsopen_s(&handle, file.c_str(), _O_RDONLY, _SH_DENYRW, _S_IREAD) == 0);
    std::error_code held;
    std::filesystem::remove_all(folder, held);
    CHECK(std::filesystem::exists(file));
    std::thread holder([handle] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        _close(handle);
    });
    CHECK(fb::remove_temporary(folder));
    holder.join();
#else
    CHECK(fb::remove_temporary(folder));
#endif
    CHECK_FALSE(std::filesystem::exists(folder));
    CHECK(fb::remove_temporary(folder));
}

TEST_CASE("a program named std isn't the standard library", "[driver]") {
    // So its code can't call the runtime, which only std/ does.
    CHECK(errors_of(fb::compile("std.fb",
                                "Extern \"runtime\"\n"
                                "    Function fb_string_live_count(): Int64\n"
                                "End Extern\n",
                                with_std({}))) ==
          std::vector<std::string>{ "std.fb: \"runtime\": only the standard library calls the "
                                    "runtime; C code's convention is \"C\"" });
}

namespace {

/// A folder of packages on disk, in the temporary folder: each file is written by its path in
/// it.
class packages_t {
public:
    packages_t() : m_folder(fb::temporary_path("packages", "")) {
        std::filesystem::create_directories(m_folder);
        // A package's files are named by their real paths, and a program's by the path it's given,
        // which differ where the temporary folder is reached through a link, as macOS's /var is
        // /private/var. The real one goes first, since it can hold the other.
        std::error_code ignored;
        for (const std::filesystem::path& root :
             { std::filesystem::weakly_canonical(m_folder, ignored), m_folder }) {
            std::string text = fb::to_utf8(root);
            std::ranges::replace(text, '\\', '/');
            m_roots.push_back(std::move(text));
        }
    }

    void remove() const {
        std::error_code ignored;
        std::filesystem::remove_all(m_folder, ignored);
    }

    void write(const std::string_view path, const std::string_view text) const {
        const std::filesystem::path file = m_folder / fb::path_from_utf8(path);
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

    [[nodiscard]] std::filesystem::path operator/(const std::string_view path) const {
        return m_folder / fb::path_from_utf8(path);
    }

    /// The compilation's diagnostics, as "<file>: <text>: message", with paths from the folder.
    [[nodiscard]] std::vector<std::string> messages(const fb::compilation_t& compilation) const {
        std::vector<std::string> messages;
        for (const std::string& error : errors_of(compilation)) {
            std::string shown = error;
            std::ranges::replace(shown, '\\', '/');
            for (const std::string& root : m_roots) {
                for (std::size_t at = shown.find(root); at != std::string::npos;
                     at = shown.find(root)) {
                    shown.replace(at, root.size() + 1, "");
                }
            }

            messages.push_back(shown);
        }

        return messages;
    }

private:
    std::filesystem::path m_folder;
    std::vector<std::string> m_roots;
};

/// Makes `link` a link to the folder `target`: a symlink, or where making one needs privileges,
/// as on Windows, a junction. False when the system makes neither.
bool make_link(const std::filesystem::path& target, const std::filesystem::path& link) {
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
    if (!error) {
        return true;
    }

#ifdef _WIN32
    const std::string command = std::format(
        R"(mklink /J "{}" "{}" > NUL)", std::filesystem::path(link).make_preferred().string(),
        std::filesystem::path(target).make_preferred().string());
    // Only cmd makes a junction, which needs no privileges as a symlink does.
    return std::system(command.c_str()) == 0 && // NOLINT(bugprone-command-processor)
           std::filesystem::exists(link, error);
#else
    return false;
#endif
}

/// Makes `link` a link to the file `target`, as cmd's mklink does where a program needs
/// privileges for it, as on Windows. False when the system makes none.
bool make_file_link(const std::filesystem::path& target, const std::filesystem::path& link) {
    std::error_code error;
    std::filesystem::create_symlink(target, link, error);
    if (!error) {
        return true;
    }

#ifdef _WIN32
    const std::string command = std::format(
        R"(mklink "{}" "{}" > NUL)", std::filesystem::path(link).make_preferred().string(),
        std::filesystem::path(target).make_preferred().string());
    return std::system(command.c_str()) == 0 && // NOLINT(bugprone-command-processor)
           std::filesystem::exists(link, error);
#else
    return false;
#endif
}

constexpr std::string_view lib_toml = "[project]\nname = \"lib\"\nversion = \"0.1.0\"\n";

constexpr std::string_view app_toml = "[project]\n"
                                      "name = \"app\"\n"
                                      "version = \"1.0.0\"\n"
                                      "\n"
                                      "[dependencies]\n"
                                      "lib = { path = \"../lib\" }\n";

} // namespace

TEST_CASE("a file reached through a link in the program's folder loads once", "[driver]") {
    const packages_t files;
    files.write("ui/hud.fb", "Public Struct Box\n    Public Field v: Int32\nEnd Struct\n");
    files.write("b.fb", "Import \"./alias/hud\"\nPublic Function Make(): Box => Box(1)\n");
    files.write("main.fb", "Import \"./ui/hud\"\nImport \"./b\"\nLocal a: Box = Make()\n");
    if (make_link(files / "ui", files / "alias")) {
        const std::expected<fb::compilation_t, std::string> compiled =
            fb::compile_file(files / "main.fb");
        REQUIRE(compiled);
        CHECK(files.messages(*compiled).empty());
    }

    files.remove();
}

TEST_CASE("a folder reached by a name whose other letters differ in case loads once", "[driver]") {
    const packages_t files;
    files.write("été/x.fb", "Public Struct Box\n    Public Field v: Int32\nEnd Struct\n");
    files.write("b.fb", "Import \"./été/x\"\nPublic Function Make(): Box => Box(1)\n");
    files.write("main.fb", "Import \"./ÉTÉ/x\"\nImport \"./b\"\nLocal a: Box = Make()\n");
    // Only a disk that ignores letter case, as Windows' and macOS's usually do, has ÉTÉ/x.fb.
    if (std::filesystem::exists(files / "ÉTÉ/x.fb")) {
        const std::expected<fb::compilation_t, std::string> compiled =
            fb::compile_file(files / "main.fb");
        REQUIRE(compiled);
        // The Import that spells it otherwise gets a warning, as one in ASCII does.
        CHECK(files.messages(*compiled) ==
              std::vector<std::string>{
                  "main.fb: \"./ÉTÉ/x\": the disk spells it \"./été/x\", in other letter cases, "
                  "which only a disk that ignores them finds" });
    }

    files.remove();
}

TEST_CASE("a package imports its dependencies by their names, whose Package names stay theirs",
          "[driver]") {
    const packages_t packages;
    packages.write("lib/project.toml", lib_toml);
    packages.write("lib/a.fb", "Package Function Helper(): Int => 1\n"
                               "Public Function Twice(): Int => Helper() * 2\n");
    packages.write("lib/tools/b.fb",
                   "Import \"../a\"\nPublic Function Thrice(): Int => Helper() * 3\n");
    packages.write("app/project.toml", app_toml);
    packages.write("app/main.fb", "Import std.core\n"
                                  "Import lib\n"
                                  "Import lib.tools\n"
                                  "Print(\"{Twice()} {tools.Thrice()} {lib.Twice()}\")\n");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    CHECK(packages.messages(*app).empty());
    REQUIRE(app->package);
    const fb::package_info_t package = app->package.value_or(fb::package_info_t{});
    CHECK(package.name == "app");
    CHECK(package.version == "1.0.0");
    CHECK(package.program == "app");
    CHECK_FALSE(package.library);

    // Its modules are named by the dependency, and each package is its own.
    std::vector<std::string> names;
    for (const fb::module_t& module : app->ast.modules().modules()) {
        if (!module.name.starts_with("std.")) {
            names.push_back(std::format("{} in {}", module.name,
                                        app->ast.modules().package_name(module.package)));
        }
    }

    CHECK(names ==
          std::vector<std::string>{ "lib.a in lib", "lib.tools.b in lib", "app.main in app" });

    // A dependency's Package names are its own, a name that isn't a dependency's names
    // nothing, and a quoted path stays in its package.
    packages.write("app/main.fb", "Import lib\n"
                                  "Import extra\n"
                                  "Import \"../outside\"\n"
                                  "Local n = Helper()\n");
    packages.write("lib/c.fb", "Import \"../app/main\"\n");
    const std::expected<fb::compilation_t, std::string> wrong = fb::compile_file(packages / "app");
    REQUIRE(wrong);
    const std::string extra =
        "app/main.fb: extra: there's no module extra: a module's name starts with std, like "
        "std.collections, this package's name, app, or a dependency's name in project.toml (lib); "
        "its own files are imported by their paths too, like \"./extra\"";
    const std::string outside = "app/main.fb: \"../outside\": \"../outside\" leaves the package's "
                                "folder, where the paths of this program's modules start";
    const std::string helper =
        "app/main.fb: Helper: 'Helper' is Package in lib/a.fb, so only the package lib can use it";
    const std::string back = "lib/c.fb: \"../app/main\": \"../app/main\" leaves the lib "
                             "package's folder, where the paths of this program's modules start";
    CHECK(packages.messages(*wrong) == std::vector<std::string>{ extra, outside, helper, back });
    packages.remove();
}

TEST_CASE("a package's own folders can be named like its dependencies, or std", "[driver]") {
    const packages_t packages;
    packages.write("lib/project.toml", lib_toml);
    packages.write("lib/a.fb", "Public Function Twice(): Int => 2\n");
    packages.write("app/project.toml", app_toml);
    packages.write("app/lib/own.fb", "Public Function Own(): Int => 7\n");
    packages.write("app/std/extra.fb", "Public Function Extra(): Int => 9\n");
    packages.write("app/main.fb", "Import std.core\n"
                                  "Import lib\n"
                                  "Import \"./lib/own\"\n"
                                  "Import app.std.extra\n"
                                  "Print(\"{Twice()} {Own()} {Extra()}\")\n");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    CHECK(packages.messages(*app).empty());

    // Every package's modules start with its name, so its own folders are its own.
    std::vector<std::string> names;
    for (const fb::module_t& module : app->ast.modules().modules()) {
        if (!module.name.starts_with("std.")) {
            names.push_back(std::format("{} in {}", module.name,
                                        app->ast.modules().package_name(module.package)));
        }
    }

    CHECK(names == std::vector<std::string>{ "lib.a in lib", "app.lib.own in app",
                                             "app.std.extra in app", "app.main in app" });

    // A main file in such a folder is the package's too.
    packages.write("app/lib/tool.fb",
                   "Import std.core\n"
                   "Import \"./own\"\nImport lib\nPrint(\"{Own()} {Twice()}\")\n");
    const std::expected<fb::compilation_t, std::string> tool =
        fb::compile_file(packages / "app/lib/tool.fb");
    REQUIRE(tool);
    CHECK(packages.messages(*tool).empty());
    CHECK(tool->package.value_or(fb::package_info_t{}).name == "app");
    CHECK(tool->package.value_or(fb::package_info_t{}).program == "tool");
    packages.remove();
}

TEST_CASE("another package's files in a package's folder are only imported by its name",
          "[driver]") {
    const packages_t packages;
    packages.write("app/project.toml", "[project]\n"
                                       "name = \"app\"\n"
                                       "version = \"1.0.0\"\n"
                                       "\n"
                                       "[dependencies]\n"
                                       "math = { path = \"vendor/math\" }\n");
    packages.write("app/vendor/math/project.toml",
                   "[project]\nname = \"math\"\nversion = \"0.1.0\"\n");
    packages.write("app/vendor/math/v.fb", "Public Function Bump(): Int => 1\n");
    packages.write("app/vendor/other/project.toml",
                   "[project]\nname = \"other\"\nversion = \"0.1.0\"\n");
    packages.write("app/vendor/other/o.fb", "Public Function Other(): Int => 2\n");
    packages.write("app/main.fb", "Import std.core\n"
                                  "Import math\n"
                                  "Import \"./vendor/math/v\"\n"
                                  "Import \"./vendor/other/o\"\n"
                                  "Import app.vendor.other\n"
                                  "Print(\"{Bump()}\")\n");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    const std::string math = "app/main.fb: \"./vendor/math/v\": \"./vendor/math/v\" names a file "
                             "of the math package, in its folder; import its modules by its name";
    const std::string other =
        "app/main.fb: \"./vendor/other/o\": \"./vendor/other/o\" names a file of the package in "
        "'app/vendor/other', which has a project.toml of its own; make it a dependency, and "
        "import its modules by its name";
    const std::string folder =
        "app/main.fb: app.vendor.other: app.vendor.other names a folder of the package in "
        "'app/vendor/other', which has a project.toml of its own; make it a dependency, and "
        "import its modules by its name";
    CHECK(packages.messages(*app) == std::vector<std::string>{ math, other, folder });
    packages.remove();

    // Without a project.toml, the program has no dependencies.
    const packages_t plain;
    plain.write("main.fb", "Import \"./vendor/other/o\"\n");
    plain.write("vendor/other/project.toml", "[project]\nname = \"other\"\nversion = \"0.1.0\"\n");
    plain.write("vendor/other/o.fb", "Public Function Other(): Int => 2\n");
    const std::expected<fb::compilation_t, std::string> alone = fb::compile_file(plain / "main.fb");
    REQUIRE(alone);
    CHECK(plain.messages(*alone) ==
          std::vector<std::string>{
              "main.fb: \"./vendor/other/o\": \"./vendor/other/o\" names a file of the package in "
              "'vendor/other', which has a project.toml of its own; a program imports a package's "
              "modules by its name, once its own project.toml depends on it" });
    plain.remove();
}

TEST_CASE("a folder Import brings in neither a package's main file nor the program's main module",
          "[driver]") {
    const packages_t packages;
    // A package with a main file, however project.toml spells it, is an executable, which can't
    // be a dependency.
    packages.write("lib/project.toml",
                   "[project]\nname = \"lib\"\nversion = \"0.1.0\"\nmain = \"./run.fb\"\n");
    packages.write("lib/run.fb", "Import std.core\n"
                                 "Print(\"lib\")\n");
    packages.write("lib/a.fb", "Public Function A(): Int => 1\n");
    packages.write("app/project.toml", app_toml);
    packages.write("app/main.fb", "Import std.core\n"
                                  "Import lib\nPrint(\"{A()}\")\n");
    const std::string executable =
        "app/project.toml: lib: lib is an executable, so it can't be a dependency; a package "
        "that programs use is a library, type = \"library\" in its project.toml, without 'main'";
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    CHECK(packages.messages(*app) == std::vector<std::string>{ executable });
    if (std::filesystem::exists(packages / "lib/RUN.fb")) {
        packages.write("lib/project.toml",
                       "[project]\nname = \"lib\"\nversion = \"0.1.0\"\nmain = \"RUN.fb\"\n");
        const std::expected<fb::compilation_t, std::string> cased =
            fb::compile_file(packages / "app");
        REQUIRE(cased);
        CHECK(packages.messages(*cased) == std::vector<std::string>{ executable });
    }

    // As a library, its run.fb is a module like the others.
    packages.write("lib/project.toml",
                   "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    packages.write("lib/run.fb", "Public Function Run(): Int => 2\n");
    packages.write("app/main.fb", "Import std.core\n"
                                  "Import lib\nPrint(\"{A() + Run()}\")\n");
    const std::expected<fb::compilation_t, std::string> library =
        fb::compile_file(packages / "app");
    REQUIRE(library);
    CHECK(packages.messages(*library).empty());

    // An executable needs its main file.
    packages.write("lib/project.toml", "[project]\nname = \"lib\"\nversion = \"0.1.0\"\n"
                                       "type = \"executable\"\nmain = \"start.fb\"\n");
    const std::expected<fb::compilation_t, std::string> started =
        fb::compile_file(packages / "lib");
    REQUIRE(started);
    CHECK(packages.messages(*started) ==
          std::vector<std::string>{ "lib/project.toml: \"start.fb\": there's no start.fb in the "
                                    "package's folder, which 'main' names" });
    packages.write("lib/project.toml",
                   "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"executable\"\n");
    const std::expected<fb::compilation_t, std::string> unmain = fb::compile_file(packages / "lib");
    REQUIRE(unmain);
    CHECK(packages.messages(*unmain) ==
          std::vector<std::string>{ "lib/project.toml: \"executable\": an executable needs its "
                                    "main file, but there's no main.fb in the package's folder" });

    // Built from another main file, the program's main module isn't one of its folder's modules
    // either, and neither is project.toml's.
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n");
    packages.write("app/ui/hud.fb", "Import app\nPublic Function Hud(): Int => Common()\n");
    packages.write("app/shared.fb", "Public Function Common(): Int => 2\n");
    packages.write("app/tool.fb", "Import std.core\n"
                                  "Import \"./ui/hud\"\nPrint(\"{Hud()}\")\n");
    const std::expected<fb::compilation_t, std::string> tool =
        fb::compile_file(packages / "app/tool.fb");
    REQUIRE(tool);
    CHECK(packages.messages(*tool).empty());
    std::vector<std::string> imported;
    for (const fb::module_t& module : tool->ast.modules().modules()) {
        if (module.name != "app.ui.hud") {
            continue;
        }

        for (const fb::module_import_t& line : module.imports) {
            for (const fb::module_id_t target : line.modules) {
                imported.push_back(tool->ast.modules().get(target).name);
            }
        }
    }

    CHECK(imported == std::vector<std::string>{ "app.shared" });
    packages.remove();
}

TEST_CASE("a .fb file in a package's folder, or under it, builds in that package", "[driver]") {
    const packages_t packages;
    packages.write("lib/project.toml", lib_toml);
    packages.write("lib/a.fb", "Public Function Twice(n: Int): Int => n * 2\n");
    packages.write("app/project.toml", app_toml);
    packages.write("app/main.fb", "Import std.core\n"
                                  "Print(\"main\")\n");
    packages.write("app/src/tool.fb", "Import std.core\n"
                                      "Import lib\nPrint(\"{Twice(4)}\")\n");
    const std::expected<fb::compilation_t, std::string> tool =
        fb::compile_file(packages / "app/src/tool.fb");
    REQUIRE(tool);
    CHECK(packages.messages(*tool).empty());
    REQUIRE(tool->package);
    CHECK(tool->package.value_or(fb::package_info_t{}).name == "app");
    CHECK(tool->package.value_or(fb::package_info_t{}).program == "tool");

    // A folder without a project.toml isn't a package.
    const std::expected<fb::compilation_t, std::string> folder =
        fb::compile_file(packages / "app/src");
    REQUIRE_FALSE(folder);
    CHECK(folder.error().contains("there's no project.toml in '"));
    CHECK(folder.error().ends_with("'; give a package's folder, or a .fb file"));
    packages.remove();
}

TEST_CASE("a package's dependencies are packages of the names it gives, without cycles",
          "[driver]") {
    const packages_t packages;
    packages.write("app/main.fb", "Import std.core\n"
                                  "Print(\"app\")\n");
    const auto check = [&](const std::string_view dependencies) {
        packages.write("app/project.toml", std::format("[project]\nname = \"app\"\nversion = "
                                                       "\"1.0.0\"\n\n[dependencies]\n{}",
                                                       dependencies));
        const std::expected<fb::compilation_t, std::string> app =
            fb::compile_file(packages / "app");
        REQUIRE(app);
        return packages.messages(*app);
    };

    CHECK(check("lib = { path = \"../nowhere\" }\n") ==
          std::vector<std::string>{ "app/project.toml: \"../nowhere\": there's no project.toml in "
                                    "'nowhere', which lib names" });

    packages.write("lib/project.toml", "[project]\nname = \"library\"\nversion = \"0.1.0\"\n");
    CHECK(check("lib = { path = \"../lib\" }\n") ==
          std::vector<std::string>{ "app/project.toml: lib: 'lib' names the package in 'lib', "
                                    "which its project.toml names library" });

    packages.write("lib/project.toml", "[project]\n"
                                       "name = \"lib\"\n"
                                       "version = \"0.1.0\"\n"
                                       "\n"
                                       "[dependencies]\n"
                                       "app = { path = \"../app\" }\n");
    CHECK(check("lib = { path = \"../lib\" }\n") ==
          std::vector<std::string>{ "lib/project.toml: app: packages can't depend on each other "
                                    "in a cycle: app -> lib -> app" });

    packages.write("lib/project.toml", lib_toml);
    packages.write("other/project.toml", "[project]\nname = \"lib\"\nversion = \"0.2.0\"\n");
    CHECK(check("lib = { path = \"../lib\" }\nmore = { path = \"../more\" }\n").size() == 1);
    packages.write("more/project.toml", "[project]\n"
                                        "name = \"more\"\n"
                                        "version = \"0.1.0\"\n"
                                        "\n"
                                        "[dependencies]\n"
                                        "lib = { path = \"../other\" }\n");
    CHECK(check("lib = { path = \"../lib\" }\nmore = { path = \"../more\" }\n") ==
          std::vector<std::string>{ "more/project.toml: lib: two packages are named lib: the "
                                    "one in 'lib', and the one in 'other'" });

    // Every dependency on one folder names its package alike, whatever path it takes there.
    packages.write("more/project.toml", "[project]\n"
                                        "name = \"more\"\n"
                                        "version = \"0.1.0\"\n"
                                        "\n"
                                        "[dependencies]\n"
                                        "other = { path = \"../app/../lib\" }\n");
    CHECK(check("lib = { path = \"../lib\" }\nmore = { path = \"../more\" }\n") ==
          std::vector<std::string>{ "more/project.toml: other: 'other' names the package in "
                                    "'lib', which its project.toml names lib" });

    // A folder spelled in other letter cases, on a disk that ignores them, is the same one.
    if (std::filesystem::exists(packages / "LIB")) {
        packages.write("more/project.toml", "[project]\n"
                                            "name = \"more\"\n"
                                            "version = \"0.1.0\"\n"
                                            "\n"
                                            "[dependencies]\n"
                                            "lib = { path = \"../LIB\" }\n");
        CHECK(check("lib = { path = \"../lib\" }\nmore = { path = \"../more\" }\n").empty());
    }

    packages.remove();
}

TEST_CASE("a library package's check covers its files, and it can't run", "[driver]") {
    const packages_t packages;
    packages.write("lib/project.toml", lib_toml);
    packages.write("lib/a.fb", "Public Function Twice(n: Int): Int => n * 2\n");
    packages.write("lib/deep/b.fb", "Public Function Wrong(): Int => \"text\"\n");
    packages.write("lib/build/old.fb", "This isn't checked\n");
    packages.write("lib/.hidden/old.fb", "This isn't checked\n");
    packages.write("lib/examples/demo/project.toml",
                   "[project]\nname = \"demo\"\nversion = \"0.1.0\"\n");
    packages.write("lib/examples/demo/main.fb", "This isn't checked, being another package's\n");
    packages.write("lib/deep/a.fb", "Public Function Again(): Int => 3\n");
    packages.write("lib/c.fb", "Print(\"top-level code\")\n");
    const std::expected<fb::compilation_t, std::string> lib = fb::compile_file(packages / "lib");
    REQUIRE(lib);
    REQUIRE(lib->package);
    CHECK(lib->package.value_or(fb::package_info_t{}).library);
    CHECK(packages.messages(*lib) ==
          std::vector<std::string>{
              "lib/c.fb: Print(\"top-level code\"): only the main module's top-level code runs, "
              "so this module only declares things",
              "lib/deep/b.fb: \"text\": expected Int32, found String",
          });

    // Each file is a module of its own, though two have one name.
    std::vector<std::string> names;
    for (const fb::module_t& module : lib->ast.modules().modules()) {
        if (!module.name.starts_with("std.")) {
            names.push_back(module.name);
        }
    }

    CHECK(names == std::vector<std::string>{ "lib.a", "lib.c", "lib.deep.a", "lib.deep.b",
                                             "lib.(library)" });

    // A file named like the check's empty main file is one of them too.
    packages.write("lib/(library).fb", "Public Function Bad(): Int => \"text\"\n");
    const std::expected<fb::compilation_t, std::string> named = fb::compile_file(packages / "lib");
    REQUIRE(named);
    CHECK(std::ranges::contains(packages.messages(*named),
                                "lib/(library).fb: \"text\": expected Int32, found String"));
    packages.remove();
}

TEST_CASE("a project.toml's mistakes stop the build, at its lines", "[driver]") {
    const packages_t packages;
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"one\"\n");
    packages.write("app/main.fb", "Print(\"app\")\n");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    CHECK(packages.messages(*app) ==
          std::vector<std::string>{ "app/project.toml: \"one\": a package's version is three "
                                    "numbers, like \"0.1.0\", not \"one\"" });

    // Without 'main', a package without main.fb is a library, but the file it names has to be
    // there.
    packages.write("app/project.toml",
                   "[project]\nname = \"app\"\nversion = \"1.0.0\"\nmain = \"mian.fb\"\n");
    const std::expected<fb::compilation_t, std::string> typo = fb::compile_file(packages / "app");
    REQUIRE(typo);
    CHECK(packages.messages(*typo) ==
          std::vector<std::string>{ "app/project.toml: \"mian.fb\": there's no mian.fb in the "
                                    "package's folder, which 'main' names" });
    packages.remove();
}

TEST_CASE("a package links its [native] libraries and its dependencies', in order", "[driver]") {
    const packages_t packages;
    const auto toml = [](const std::string_view name, const std::string_view dependencies,
                         const std::string_view natives) {
        return std::format("[project]\nname = \"{}\"\nversion = \"0.1.0\"\n\n[dependencies]\n{}"
                           "\n[native]\n{}",
                           name, dependencies, natives);
    };
    // app depends on a and b, which both depend on c: each package's archives come before those
    // of the packages it depends on, and a system library comes once.
    constexpr std::string_view archive = "!<arch>\n";
    packages.write("app/project.toml",
                   toml("app", "a = { path = \"../a\" }\nb = { path = \"../b\" }\n",
                        "own = \"libs/own\"\nm = { system = \"m\" }\n"));
    packages.write("app/libs/libown.a", archive);
    packages.write("app/main.fb", "Import std.core\n"
                                  "Import a\nImport b\nPrint(\"{A()} {B()}\")\n");
    packages.write("a/project.toml",
                   toml("a", "c = { path = \"../c\" }\n", "x = \"a.a\"\ny = \"lib/x\"\n"));
    packages.write("a/a.a", archive);
    packages.write("a/lib/x.lib", archive);
    packages.write("a/a.fb", "Public Function A(): Int => 1\n");
    packages.write("b/project.toml", toml("b", "c = { path = \"../c\" }\n",
                                          "x = \"b\"\nm = { system = \"m\" }\n"
                                          "z = { system = \"z\" }\n"));
    packages.write("b/b.a", archive);
    packages.write("b/b.fb", "Public Function B(): Int => 2\n");
    // A file named in full is found whatever its extension, in any letter case.
    packages.write("c/project.toml",
                   toml("c", "", "x = \"c\"\nwhole = \"libs/Whole.Lib\"\nbare = \"libs/bare\"\n"));
    packages.write("c/libc.a", archive);
    packages.write("c/libs/Whole.Lib", archive);
    packages.write("c/libs/bare", "!<thin>\n");
    packages.write("c/c.fb", "Public Function C(): Int => 3\n");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    CHECK(packages.messages(*app).empty());
    std::vector<std::string> archives;
    for (const std::filesystem::path& linked : app->natives.archives) {
        archives.push_back(fb::to_utf8(linked.parent_path().filename() / linked.filename()));
    }

    CHECK(archives == std::vector<std::string>{ "libs/libown.a", "a/a.a", "lib/x.lib", "b/b.a",
                                                "c/libc.a", "libs/Whole.Lib", "libs/bare" });
    CHECK(app->natives.system == std::vector<std::string>{ "m", "z" });

    // An archive project.toml names has to be there, and be an archive.
    std::filesystem::create_directories(packages / "c/libs/folder.a");
    std::filesystem::create_directories(packages / "c/libs/plain");
    packages.write("c/junk.a", "not an archive");
    packages.write(
        "c/project.toml",
        toml("c", "",
             std::format("x = \"libs/c\"\ny = \"c.lib\"\njunk = \"junk\"\n"
                         "dynamic = \"libs/x.so\"\nfolder = \"libs/folder.a\"\n"
                         "plain = \"libs/plain\"\nslash = \"libs/\"\nfar = \"{}\"\n",
                         fb::to_utf8(std::filesystem::weakly_canonical(packages / "c") / "far"))));
    std::string far = fb::to_utf8(std::filesystem::weakly_canonical(packages / "c") / "far");
    std::ranges::replace(far, '\\', '/');
    const std::expected<fb::compilation_t, std::string> missing =
        fb::compile_file(packages / "app");
    REQUIRE(missing);
    std::vector<std::string> messages = packages.messages(*missing);
    CHECK(messages.size() == 8);
    messages.resize(7);
    const std::string none = "c/project.toml: \"libs/c\": \"libs/c\" names no static library: "
                             "there's no libs/c, libs/libc.a, libs/c.a, or libs/c.lib, from the "
                             "package's folder";
    const std::string full = "c/project.toml: \"c.lib\": \"c.lib\" names no static library: "
                             "there's no c.lib, from the package's folder";
    const std::string junk = "c/project.toml: \"junk\": \"junk\" names junk.a, which isn't a "
                             "static library: it doesn't start as an archive does";
    const std::string dynamic = "c/project.toml: \"libs/x.so\": \"libs/x.so\" is a dynamic "
                                "library, which [native] takes as dynamic = { dynamic = "
                                "\"libs/x\" }";
    const std::string folder =
        R"(c/project.toml: "libs/folder.a": "libs/folder.a" is a folder, not a static library)";
    // A folder is one whatever its name, and a path that ends in '/' names one.
    const std::string plain =
        R"(c/project.toml: "libs/plain": "libs/plain" is a folder, not a static library)";
    const std::string slash =
        R"(c/project.toml: "libs/": "libs/" is a folder, not a static library)";
    CHECK(messages == std::vector<std::string>{ none, full, junk, dynamic, folder, plain, slash });
    // A path from elsewhere is shown as it's written.
    CHECK(fb::compile_file(packages / "app")
              .transform([&](const fb::compilation_t& compiled) {
                  return errors_of(compiled).back().ends_with(
                      std::format("names no static library: there's no {}, {}/libfar.a, {}.a, or "
                                  "{}.lib",
                                  far, far.substr(0, far.size() - 4), far, far));
              })
              .value_or(false));
    packages.remove();
}

TEST_CASE("a package in a folder of any name links its C libraries", "[driver]") {
    const packages_t packages;
    // 游戏, which the ANSI code page that MinGW's tools read their arguments in can't spell.
    const std::string folder = "\xe6\xb8\xb8\xe6\x88\x8f";
    packages.write(folder + "/project.toml", "[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n"
                                             "[native]\nvecmath = \"libs/vecmath\"\n");
    packages.write(folder + "/main.fb", "Import std.core\n"
                                        "Extern \"C\" Lib \"vecmath\"\n"
                                        "    Function vm_add(a: Int32, b: Int32): Int32\n"
                                        "End Extern\n"
                                        "Print(\"{vm_add(2, 3)}\")\n");
    const std::filesystem::path archive(FB_TEST_VECMATH);
    std::filesystem::create_directories(packages / (folder + "/libs"));
    std::filesystem::copy_file(archive, packages / (folder + "/libs") / archive.filename());
    const std::expected<fb::compilation_t, std::string> game = fb::compile_file(packages / folder);
    REQUIRE(game);
    REQUIRE(packages.messages(*game).empty());

    // The executable goes in a folder of that name too.
    std::filesystem::path program = packages / (folder + "/build/" + folder);
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(game->ast, game->analysis);
    const std::expected<fb::process_result_t, std::string> ran =
        fb::build_program(module, *game, program, fb::build_mode_t::DEBUG).and_then([&] {
            return fb::run_process(program, {});
        });
    INFO((ran ? ran->output : ran.error()));
    REQUIRE(ran);
    CHECK(ran->output == "5\n");
    packages.remove();
}

TEST_CASE("a system library the linker can't find is an error at its project.toml line",
          "[driver]") {
    const packages_t packages;
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                                       "[native]\nnope = { system = \"fb_no_such_library\" }\n");
    packages.write("app/main.fb", "Import std.core\n"
                                  "Print(\"app\")\n");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    REQUIRE(packages.messages(*app).empty());
    const fb::mir_module_t module = fb::lower(app->ast, app->analysis);
    const std::expected<void, std::string> built =
        fb::build_program(module, *app, fb::temporary_path("nope", fb::executable_extension),
                          fb::build_mode_t::DEBUG);
    REQUIRE_FALSE(built);
    const fb::link_errors_t found = fb::link_errors(*app, built.error());
    CHECK(found.complete);
    REQUIRE(found.errors.size() == 1);
    const fb::diagnostic_t& error = found.errors.front();
    CHECK(app->sources.get(error.span.file).slice(error.span) == "\"fb_no_such_library\"");
    CHECK(error.message == "the linker can't find the system library 'fb_no_such_library'");
    packages.remove();
}

namespace {

/// The file a [native] dynamic library's path names on this target, and the name its build
/// copies it beside the executable under: tests/native's dyncalc has a SONAME on Linux.
#ifdef _WIN32
constexpr std::string_view dyncalc_file = "dyncalc.dll";
constexpr std::string_view dyncalc_beside = "dyncalc.dll";
constexpr std::string_view library_kind = "a DLL";
#elifdef __APPLE__
constexpr std::string_view dyncalc_file = "libdyncalc.dylib";
constexpr std::string_view dyncalc_beside = "libdyncalc.dylib";
constexpr std::string_view library_kind = "a dylib";
#else
constexpr std::string_view dyncalc_file = "libdyncalc.so";
constexpr std::string_view dyncalc_beside = "libdyncalc.so.1";
constexpr std::string_view library_kind = "a shared object";
#endif

/// A dynamic library's file for this target, with nothing in it, named `beside` on Linux and
/// macOS, as an executable records it.
std::string empty_library(const std::string_view beside) {
    const std::string_view target = fb::build_target();
    if (target.starts_with("windows")) {
        return fb_test::pe_library(fb_test::pe_amd64);
    }

    if (target.starts_with("macos")) {
        return fb_test::mach_o_library(
            target == "macos-arm64" ? fb_test::mach_o_arm64 : fb_test::mach_o_x86_64, beside);
    }

    return fb_test::elf_library(
        target == "linux-arm64" ? fb_test::elf_aarch64 : fb_test::elf_x86_64, beside);
}

/// The file a dynamic library `name` is on this target: name.dll, libname.so, or
/// libname.dylib.
std::string library_file(const std::string& name) {
    const std::string_view target = fb::build_target();
    if (target.starts_with("windows")) {
        return name + ".dll";
    }

    return "lib" + name + (target.starts_with("macos") ? ".dylib" : ".so");
}

/// A dynamic library's file for another CPU than this target's.
std::string other_cpu_library() {
    const std::string_view target = fb::build_target();
    if (target.starts_with("windows")) {
        return fb_test::pe_library(fb_test::pe_arm64);
    }

    if (target.starts_with("macos")) {
        return fb_test::mach_o_library(target == "macos-arm64" ? fb_test::mach_o_x86_64
                                                               : fb_test::mach_o_arm64,
                                       "@rpath/libdyncalc.dylib");
    }

    return fb_test::elf_library(
        target == "linux-arm64" ? fb_test::elf_x86_64 : fb_test::elf_aarch64, "");
}

/// An executable's file for this target, which isn't a dynamic library.
std::string executable_image() {
    const std::string_view target = fb::build_target();
    if (target.starts_with("windows")) {
        return fb_test::pe_library(fb_test::pe_amd64, false);
    }

    if (target.starts_with("macos")) {
        return fb_test::mach_o_library(
            target == "macos-arm64" ? fb_test::mach_o_arm64 : fb_test::mach_o_x86_64, "x", 2);
    }

    return fb_test::elf_library(
        target == "linux-arm64" ? fb_test::elf_aarch64 : fb_test::elf_x86_64, "", 2);
}

/// The calc package, whose [native] dyncalc is tests/native's dynamic library in its
/// native/<target>/ folder, and the app package, a program that uses it.
void write_calc(const packages_t& packages) {
    packages.write("calc/project.toml",
                   "[project]\nname = \"calc\"\nversion = \"0.1.0\"\n\n"
                   "[native]\ndyncalc = { dynamic = \"native/{target}/dyncalc\" }\n");
    packages.write("calc/calc.fb",
                   "Package Extern \"C\" Lib \"dyncalc\"\n"
                   "    Function dc_add(a: Int32, b: Int32): Int32\n"
                   "    Function dc_name(): String\n"
                   "    Function dc_count(): Int64\n"
                   "End Extern\n"
                   "Public Function Add(a: Int32, b: Int32): Int32 => dc_add(a, b)\n"
                   "Public Function Name(): String => dc_name()\n"
                   "Public Function Count(): Int64 => dc_count()\n");
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                                       "[dependencies]\ncalc = { path = \"../calc\" }\n");
    packages.write("app/main.fb", "Import std.core\n"
                                  "Import calc\n"
                                  "Print(\"{Add(2, 3)} {Name()} {Count()} {Count()}\")\n");
    const std::filesystem::path folder =
        packages / ("calc/native/" + std::string(fb::build_target()));
    std::filesystem::create_directories(folder);
    std::filesystem::copy_file(std::filesystem::path(FB_TEST_DYNCALC),
                               folder / fb::path_from_utf8(dyncalc_file));
#ifdef _WIN32
    // Its import library, libdyncalc.dll.a, goes beside it.
    const std::filesystem::path import(FB_TEST_DYNCALC_LINKED);
    std::filesystem::copy_file(import, folder / import.filename());
#endif
}

} // namespace

TEST_CASE("a package's dynamic library goes beside the program, which runs from there",
          "[driver]") {
    const packages_t packages;
    write_calc(packages);
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    REQUIRE(packages.messages(*app).empty());
    REQUIRE(app->natives.dynamic.size() == 1);
    CHECK(app->natives.dynamic.front().beside == dyncalc_beside);

    // The library is only in its package's folder and beside the program, which finds it there
    // from anywhere: beside it on Windows, through $ORIGIN on Linux, and @loader_path on macOS.
    std::filesystem::path program = packages / "app/build/app";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(app->ast, app->analysis);
    for (int build = 0; build < 2; ++build) {
        const std::expected<fb::process_result_t, std::string> ran =
            fb::build_program(module, *app, program, fb::build_mode_t::DEBUG).and_then([&] {
                return fb::run_process(program, {});
            });
        INFO((ran ? ran->output : ran.error()));
        REQUIRE(ran);
        CHECK(ran->exit_code == 0);
        CHECK(ran->output == "5 dyncalc 1 2\n");
        const std::filesystem::path copy =
            program.parent_path() / fb::path_from_utf8(dyncalc_beside);
        CHECK(std::filesystem::file_size(copy) ==
              std::filesystem::file_size(std::filesystem::path(FB_TEST_DYNCALC)));
        // What the next build finds there is replaced.
        std::ofstream(copy, std::ios::binary) << "stale";
    }

    // The executable can't have the name the library goes beside it as, which would replace it.
    const std::filesystem::path over = packages / "app/build" / fb::path_from_utf8(dyncalc_beside);
    const std::expected<void, std::string> replaced =
        fb::build_program(module, *app, over, fb::build_mode_t::DEBUG);
    REQUIRE_FALSE(replaced);
    CHECK(replaced.error().contains(
        std::format("would have the name of {}, which goes beside it as {}; give it another name",
                    fb::to_utf8(app->natives.dynamic.front().file), dyncalc_beside)));
    packages.remove();
}

TEST_CASE("a package's dynamic library has to be there for the target, as the OS's kind",
          "[driver]") {
    const packages_t packages;
    write_calc(packages);
    const std::string target(fb::build_target());
    const std::string folder = "calc/native/" + target + "/";
    const std::string shown = "native/" + target + "/" + std::string(dyncalc_file);
    const std::string at = "calc/project.toml: \"native/{target}/dyncalc\": ";
    const auto messages = [&] {
        const std::expected<fb::compilation_t, std::string> app =
            fb::compile_file(packages / "app");
        return app ? packages.messages(*app) : std::vector<std::string>{ app.error() };
    };

    // A target the package has no binary for names the package.
    std::filesystem::remove_all(packages / folder);
    CHECK(messages() == std::vector<std::string>{ std::format(
                            "{}the calc package has no dynamic library \"dyncalc\" for {}: "
                            "there's no {}, from its folder",
                            at, target, shown) });

    // What's there has to be a dynamic library, for this target's CPU.
    packages.write(folder + std::string(dyncalc_file), "not a library");
    packages.write(folder + "libdyncalc.dll.a", "!<arch>\n");
    CHECK(messages() == std::vector<std::string>{ std::format(
                            "{}\"native/{{target}}/dyncalc\" names {}, which isn't a dynamic "
                            "library: it doesn't start as {} does",
                            at, shown, library_kind) });
    packages.write(folder + std::string(dyncalc_file), other_cpu_library());
    CHECK(messages() == std::vector<std::string>{ std::format(
                            "{}\"native/{{target}}/dyncalc\" names {}, which is {} for another "
                            "CPU than {}'s",
                            at, shown, library_kind, target) });
    packages.write(folder + std::string(dyncalc_file), executable_image());
    CHECK(messages() == std::vector<std::string>{ std::format(
                            "{}\"native/{{target}}/dyncalc\" names {}, which is an executable, "
                            "not a dynamic library",
                            at, shown) });

#ifdef _WIN32
    // A DLL needs its import library, an archive, beside it.
    packages.write(folder + std::string(dyncalc_file), empty_library(""));
    std::filesystem::remove(packages / (folder + "libdyncalc.dll.a"));
    CHECK(messages() == std::vector<std::string>{ std::format(
                            "{}the calc package's {} has no import library beside it: there's no "
                            "dyncalc.lib, libdyncalc.dll.a, or libdyncalc.a",
                            at, shown) });
    packages.write(folder + "dyncalc.lib", "not an archive");
    CHECK(messages() == std::vector<std::string>{ std::format(
                            "{}\"native/{{target}}/dyncalc\" names {}, whose import library "
                            "native/{}/dyncalc.lib isn't one: it doesn't start as an archive does",
                            at, shown, target) });
#elifdef __APPLE__
    // The executable finds it beside it through its install name.
    packages.write(folder + std::string(dyncalc_file),
                   empty_library("/usr/local/lib/libdyncalc.dylib"));
    CHECK(messages() ==
          std::vector<std::string>{ std::format(
              "{}the calc package's {} has the install name /usr/local/lib/libdyncalc.dylib, but "
              "the program finds it beside the executable, as @rpath/libdyncalc.dylib or a name "
              "in @loader_path; link it with -install_name @rpath/libdyncalc.dylib",
              at, shown) });
    for (const std::string_view name :
         { "@rpath/libother.dylib", "@loader_path/sub/x.dylib", "@loader_path/", "@loader_path/.",
           "@loader_path/..", "libdyncalc.dylib" }) {
        packages.write(folder + std::string(dyncalc_file), empty_library(name));
        CHECK(messages().size() == 1);
    }

    packages.write(folder + std::string(dyncalc_file),
                   empty_library("@loader_path/libother.dylib"));
    const std::expected<fb::compilation_t, std::string> other = fb::compile_file(packages / "app");
    REQUIRE(other);
    REQUIRE(other->natives.dynamic.size() == 1);
    CHECK(other->natives.dynamic.front().beside == "libother.dylib");
#else
    // The executable finds it by its SONAME, beside it, or without one by its file's name.
    for (const std::string_view name : { "/opt/libdyncalc.so", ".", ".." }) {
        packages.write(folder + std::string(dyncalc_file), empty_library(name));
        CHECK(messages() == std::vector<std::string>{ std::format(
                                "{}the calc package's {} has the SONAME {}, which isn't a file's "
                                "name, but the program finds it beside the executable by one",
                                at, shown, name) });
    }

    packages.write(folder + std::string(dyncalc_file), empty_library(""));
    const std::expected<fb::compilation_t, std::string> unnamed =
        fb::compile_file(packages / "app");
    REQUIRE(unnamed);
    REQUIRE(unnamed->natives.dynamic.size() == 1);
    CHECK(unnamed->natives.dynamic.front().beside == "libdyncalc.so");
#endif

    packages.remove();
}

TEST_CASE("a [native] path's {target} is the build's target", "[driver]") {
    const packages_t packages;
    const std::string target(fb::build_target());
    const auto messages = [&] {
        const std::expected<fb::compilation_t, std::string> app =
            fb::compile_file(packages / "app");
        return app ? packages.messages(*app) : std::vector<std::string>{ app.error() };
    };

    // A static library's too, which a target without one names the package for.
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                                       "[native]\nvecmath = \"libs/{target}/vecmath\"\n");
    packages.write("app/main.fb", "Import std.core\n"
                                  "Extern \"C\" Lib \"vecmath\"\n"
                                  "    Function vm_add(a: Int32, b: Int32): Int32\n"
                                  "End Extern\n"
                                  "Print(\"{vm_add(2, 3)}\")\n");
    CHECK(messages() ==
          std::vector<std::string>{ std::format(
              "app/project.toml: \"libs/{{target}}/vecmath\": the app package has no static "
              "library \"vecmath\" for {}: there's no libs/{}/vecmath, libs/{}/libvecmath.a, "
              "libs/{}/vecmath.a, or libs/{}/vecmath.lib, from its folder",
              target, target, target, target, target) });
    const std::filesystem::path archive(FB_TEST_VECMATH);
    std::filesystem::create_directories(packages / ("app/libs/" + target));
    std::filesystem::copy_file(archive, packages / ("app/libs/" + target) / archive.filename());
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    REQUIRE(packages.messages(*app).empty());
    REQUIRE(app->natives.archives.size() == 1);
    CHECK(app->natives.archives.front().parent_path().filename() == target);

    // A dynamic library's path leaves out what differs by OS, and can't be a folder's.
    packages.write("app/project.toml",
                   "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                   "[native]\nx = { dynamic = \"native/{target}/x.dll\" }\n"
                   "y = { dynamic = \"native/{target}/\" }\nz = \"libs/z.so\"\n");
    CHECK(messages() ==
          std::vector<std::string>{
              "app/project.toml: \"native/{target}/x.dll\": a dynamic library's path leaves out "
              "its file's \"lib\" and extension, so it's the same on every OS, like "
              "\"native/{target}/x\"",
              R"(app/project.toml: "native/{target}/": "native/{target}/" is a folder, not a )"
              "dynamic library",
              "app/project.toml: \"libs/z.so\": \"libs/z.so\" is a dynamic library, which "
              "[native] takes as z = { dynamic = \"libs/z\" }",
          });

    // The path it says to write is in the folder written, without the file's "lib" and
    // extension, or an import library's, but for a DLL's name, which is its file's.
    packages.write("app/native/" + target + "/real.dll", "MZ");
    packages.write("app/project.toml",
                   "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                   "[native]\na = { dynamic = \"native/{target}/libengine.so\" }\n"
                   "b = { dynamic = \"native/{target}/libdyncalc.dll\" }\n"
                   "c = \"libs/libengine.dylib\"\nd = { dynamic = \"libs/dyncalc.so\" }\n"
                   "e = { dynamic = 'native\\{target}\\dyncalc.lib' }\n"
                   "f = { dynamic = \"native/{target}/libdyncalc.dll.a\" }\n"
                   "g = { dynamic = \"native/{target}\" }\nh = \"native/{target}/real.dll\"\n");
    // What's at each path, and the path it says to write.
    const auto unlike = [](const std::string_view written, const std::string_view like) {
        return std::format("app/project.toml: {}: a dynamic library's path leaves out its file's "
                           "\"lib\" and extension, so it's the same on every OS, like \"{}\"",
                           written, like);
    };
    const auto takes = [](const std::string_view written, const std::string_view name,
                          const std::string_view like) {
        return std::format("app/project.toml: \"{}\": \"{}\" is a dynamic library, which [native] "
                           "takes as {} = {{ dynamic = \"{}\" }}",
                           written, written, name, like);
    };
    const std::string folder = R"(app/project.toml: "native/{target}": "native/{target}" is a )"
                               "folder, not a dynamic library";
    CHECK(messages() ==
          std::vector<std::string>{
              unlike("\"native/{target}/libengine.so\"", "native/{target}/engine"),
              unlike("\"native/{target}/libdyncalc.dll\"", "native/{target}/libdyncalc"),
              takes("libs/libengine.dylib", "c", "libs/engine"),
              unlike("\"libs/dyncalc.so\"", "libs/dyncalc"),
              // Its ''s are shown as the '/'s they are.
              unlike("'native/{target}/dyncalc.lib'", "native/{target}/dyncalc"),
              unlike("\"native/{target}/libdyncalc.dll.a\"", "native/{target}/dyncalc"),
              folder,
              takes("native/{target}/real.dll", "h", "native/{target}/real"),
          });
    packages.remove();
}

TEST_CASE("two dynamic libraries can't go beside the program by one name", "[driver]") {
    const packages_t packages;
    const std::string target(fb::build_target());
    const auto library = [&](const std::string& package, const std::string& name) {
        const std::string folder = package + "/native/" + target + "/";
        const std::string beside = library_file(name);
        packages.write(folder + beside,
                       empty_library(target.starts_with("macos") ? "@rpath/" + beside : ""));
        packages.write(folder + "lib" + name + ".dll.a", "!<arch>\n");
    };
    // \xC3\xA9 is é, and \xC3\x89 É.
    packages.write("app/project.toml",
                   "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                   "[dependencies]\nlib = { path = \"../lib\" }\n\n"
                   "[native]\nengine = { dynamic = \"native/{target}/engine\" }\n"
                   "accent = { dynamic = \"native/{target}/\xC3\xA9lan\" }\n");
    packages.write("app/main.fb", "Import std.core\nImport lib\nPrint(\"app\")\n");
    library("app", "engine");
    library("app", "\xC3\xA9lan");
    // In any letter case, as a Windows or macOS disk has them.
    packages.write("lib/project.toml",
                   "[project]\nname = \"lib\"\nversion = \"0.1.0\"\n\n"
                   "[native]\nother = { dynamic = \"native/{target}/Engine\" }\n"
                   "capital = { dynamic = \"native/{target}/\xC3\x89lan\" }\n");
    packages.write("lib/lib.fb", "Public Function F(): Int => 1\n");
    library("lib", "Engine");
    library("lib", "\xC3\x89lan");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    CHECK(packages.messages(*app) ==
          std::vector<std::string>{
              std::format("lib/project.toml: \"native/{{target}}/Engine\": the lib package's "
                          "\"other\" goes beside the executable as {}, as the app package's "
                          "\"engine\" does",
                          library_file("Engine")),
              std::format("lib/project.toml: \"native/{{target}}/\xC3\x89lan\": the lib "
                          "package's \"capital\" goes beside the executable as {}, as the app "
                          "package's \"accent\" does",
                          library_file("\xC3\x89lan")),
          });
    packages.remove();
}

TEST_CASE("Lib names one of its package's [native] libraries", "[driver]") {
    const packages_t packages;
    packages.write("lib/project.toml", "[project]\nname = \"lib\"\nversion = \"0.1.0\"\n\n"
                                       "[native]\nm = { system = \"m\" }\n");
    packages.write("lib/a.fb", "Extern \"C\" Lib \"m\"\n"
                               "    Function cbrt(x: Float64): Float64\n"
                               "End Extern\n"
                               "Public Function Root(x: Float64): Float64 => cbrt(x)\n");
    packages.write("app/project.toml", app_toml);
    packages.write("app/main.fb", "Import std.core\n"
                                  "Import lib\n"
                                  "Extern \"C\" Lib \"m\"\n"
                                  "    Function sqrt(x: Float64): Float64\n"
                                  "End Extern\n"
                                  "Print(\"{Root(8.0)} {sqrt(4.0)}\")\n");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    // A dependency's are its own.
    CHECK(packages.messages(*app) ==
          std::vector<std::string>{ "app/main.fb: \"m\": the app package's project.toml has no "
                                    "[native] library \"m\"" });
    packages.remove();

    // Without a project.toml, there are none.
    CHECK(errors_of(
              fb::compile("main.fb", "Extern \"C\" Lib \"vecmath\"\nEnd Extern\n", with_std({}))) ==
          std::vector<std::string>{ "main.fb: \"vecmath\": 'Lib' names one of a package's [native] "
                                    "libraries, and this program has no project.toml" });
}

TEST_CASE("a C symbol no library defines is an error at its first Extern Function", "[driver]") {
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Extern \"C\"\n"
                    "    Function Missing(): Int32 = \"no_such_c_symbol\"\n"
                    "    Function also_missing(): Int32\n"
                    "    Function Again(): Int32 = \"also_missing\"\n"
                    "    Function Hidden(): Int32 = \"_hidden\"\n"
                    "End Extern\n"
                    "Print(\"{Missing()} {also_missing()} {Again()}\")\n",
                    with_std({}));
    REQUIRE(errors_of(compilation).empty());
    const auto shown = [&](const std::vector<fb::diagnostic_t>& diagnostics) {
        std::vector<std::string> messages;
        messages.reserve(diagnostics.size());
        for (const fb::diagnostic_t& diagnostic : diagnostics) {
            messages.push_back(std::format(
                "{}: {}", compilation.sources.get(diagnostic.span.file).slice(diagnostic.span),
                diagnostic.message));
        }

        return messages;
    };
    const std::vector<std::string> expected{
        "\"no_such_c_symbol\": the C symbol 'no_such_c_symbol' isn't in the C runtime, or any "
        "library the program links",
        "also_missing: the C symbol 'also_missing' isn't in the C runtime, or any library the "
        "program links",
    };
    const fb::link_errors_t both =
        fb::link_errors(compilation, "linking failed:\n"
                                     "ld.lld: error: undefined symbol: also_missing\n"
                                     ">>> referenced by main.o:(main)\n"
                                     "ld.lld: error: undefined symbol: "
                                     "no_such_c_symbol\r\n"
                                     ">>> referenced by main.o:(main)\n"
                                     "collect2.exe: error: ld returned 1 exit status\n");
    CHECK(shown(both.errors) == expected);
    CHECK(both.complete);

    // Anything else the linker reports stays as it said, beside the errors at the program's lines.
    const fb::link_errors_t other = fb::link_errors(
        compilation, "linking failed:\nld.lld: error: undefined symbol: some_other_symbol\n");
    CHECK(other.errors.empty());
    CHECK_FALSE(other.complete);
    CHECK_FALSE(
        fb::link_errors(compilation, "linking failed:\nld.lld: error: no input files\n").complete);
    const fb::link_errors_t mixed = fb::link_errors(
        compilation, "linking failed:\nld.lld: error: undefined symbol: also_missing\n"
                     "ld.lld: error: undefined symbol: inner_missing\n");
    CHECK(mixed.errors.size() == 1);
    CHECK_FALSE(mixed.complete);

    // A symbol that starts with '_' is its own, and only Apple's C symbols have an '_' before
    // their name.
    CHECK(
        shown(fb::link_errors(compilation, "ld.lld: error: undefined symbol: _hidden\n").errors) ==
        std::vector<std::string>{ "\"_hidden\": the C symbol '_hidden' isn't in the C runtime, or "
                                  "any library the program links" });
#ifndef __APPLE__
    CHECK_FALSE(
        fb::link_errors(compilation, "ld.lld: error: undefined symbol: _also_missing\n").complete);
#endif

    // And the linker says so, as link_errors() reads it.
    const std::filesystem::path program = fb::temporary_path("missing", fb::executable_extension);
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const std::expected<void, std::string> built =
        fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG);
    REQUIRE_FALSE(built);
    CHECK(shown(fb::link_errors(compilation, built.error()).errors) == expected);
}

TEST_CASE("every C symbol that no library defines is reported", "[driver]") {
    // More than lld's 20 errors.
    std::string program = "Extern \"C\"\n";
    std::string calls = "Local n = 0\n";
    for (int i = 0; i < 25; ++i) {
        program += std::format("    Function gone_{}(): Int32\n", i);
        calls += std::format("n += gone_{}()\n", i);
    }

    const fb::compilation_t compilation =
        fb::compile("main.fb", program + "End Extern\n" + calls, with_std({}));
    REQUIRE(errors_of(compilation).empty());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const std::expected<void, std::string> built =
        fb::build_program(module, compilation, fb::temporary_path("gone", fb::executable_extension),
                          fb::build_mode_t::DEBUG);
    REQUIRE_FALSE(built);
    const fb::link_errors_t found = fb::link_errors(compilation, built.error());
    CHECK(found.errors.size() == 25);
    CHECK(found.complete);
}

TEST_CASE("a quoted Import names a file, and only a .fb file builds", "[driver]") {
    const packages_t packages;
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n");
    packages.write("app/main.fb", "Import \"./sub/x\"\n");
    packages.write("app/sub/x.fb", "Import \"..\"\nImport \".\"\nImport \"./\"\n");
    packages.write("app.fb", "Public Function Leak(): Int => 1\n");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    const auto folder = [](const std::string_view path) {
        return std::format(R"(app/sub/x.fb: "{}": "{}" names a folder; a quoted path names a .fb )"
                           R"(file, without ".fb", like "./hud")",
                           path, path);
    };
    CHECK(packages.messages(*app) ==
          std::vector<std::string>{ folder(".."), folder("."), folder("./") });

    const std::expected<fb::compilation_t, std::string> manifest =
        fb::compile_file(packages / "app/project.toml");
    REQUIRE_FALSE(manifest);
    CHECK(manifest.error().ends_with("project.toml' isn't a .fb file; give a .fb file, or a "
                                     "package's folder"));
    packages.remove();
}

TEST_CASE("a dependency's project.toml is checked as its own package's is", "[driver]") {
    const packages_t packages;
    packages.write("app/main.fb", "Print(\"app\")\n");
    const auto toml = [](const std::string_view name, const std::string_view rest) {
        return std::format("[project]\nname = \"{}\"\nversion = \"0.1.0\"\n{}", name, rest);
    };

    // A path to a file isn't a package's folder.
    packages.write("app/project.toml",
                   toml("app", "\n[dependencies]\nlib = { path = \"../lib.fb\" }\n"));
    packages.write("lib.fb", "");
    const std::expected<fb::compilation_t, std::string> file = fb::compile_file(packages / "app");
    REQUIRE(file);
    CHECK(packages.messages(*file) ==
          std::vector<std::string>{ "app/project.toml: \"../lib.fb\": 'lib.fb' is a file, not a "
                                    "package's folder, which lib names" });

    // A main file project.toml names has to be there.
    packages.write("app/project.toml",
                   toml("app", "\n[dependencies]\nlib = { path = \"../lib\" }\n"));
    packages.write("lib/project.toml", toml("lib", "main = \"mian.fb\"\n"));
    const std::expected<fb::compilation_t, std::string> main = fb::compile_file(packages / "app");
    REQUIRE(main);
    CHECK(packages.messages(*main) ==
          std::vector<std::string>{ "lib/project.toml: \"mian.fb\": there's no mian.fb in the "
                                    "package's folder, which 'main' names" });

    // A package that two depend on, with a mistake, is reported once.
    packages.write("app/project.toml", toml("app", "\n[dependencies]\nb = { path = \"../b\" }\n"
                                                   "c = { path = \"../c\" }\n"));
    packages.write("b/project.toml", toml("b", "\n[dependencies]\nd = { path = \"../d\" }\n"));
    packages.write("c/project.toml", toml("c", "\n[dependencies]\nd = { path = \"../d\" }\n"));
    packages.write("d/project.toml", "[project]\nname = \"d\"\nversion = \"x\"\n");
    const std::expected<fb::compilation_t, std::string> twice = fb::compile_file(packages / "app");
    REQUIRE(twice);
    CHECK(packages.messages(*twice) ==
          std::vector<std::string>{ "d/project.toml: \"x\": a package's version is three numbers, "
                                    "like \"0.1.0\", not \"x\"" });
    packages.remove();
}

TEST_CASE("a package's project.toml that can't be read stops the build", "[driver]") {
    const packages_t packages;
    packages.write("app/project.toml", app_toml);
    packages.write("app/main.fb", "Print(\"app\")\n");
    const std::filesystem::path manifest = packages / "app/project.toml";
#ifdef _WIN32
    // Another program can hold it open without sharing it.
    int handle = -1;
    (void)_wsopen_s(&handle, manifest.c_str(), _O_RDONLY, _SH_DENYRW, _S_IREAD);
#else
    std::filesystem::permissions(manifest, std::filesystem::perms::none);
#endif
    // Not where nothing stops this from reading it, as for a POSIX root.
    if (!std::ifstream(manifest)) {
        for (const std::string_view input : { "app", "app/main.fb" }) {
            const std::expected<fb::compilation_t, std::string> app =
                fb::compile_file(packages / input);
            REQUIRE_FALSE(app);
            CHECK(app.error().starts_with("cannot open '"));
        }
    }

#ifdef _WIN32
    _close(handle);
#else
    std::filesystem::permissions(manifest, std::filesystem::perms::owner_all);
#endif
    packages.remove();
}

TEST_CASE("a hint names a package that the code's package doesn't depend on", "[driver]") {
    // app depends on mid, which depends on lib, so the program loads lib, which app can't import.
    const packages_t packages;
    packages.write("lib/project.toml", lib_toml);
    packages.write("lib/a.fb", "Public Function Twice(): Int => 2\n");
    packages.write("mid/project.toml", "[project]\nname = \"mid\"\nversion = \"0.1.0\"\n\n"
                                       "[dependencies]\nlib = { path = \"../lib\" }\n");
    packages.write("mid/m.fb", "Import lib\nPublic Function Four(): Int => Twice() * 2\n");
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n\n"
                                       "[dependencies]\nmid = { path = \"../mid\" }\n");
    packages.write("app/main.fb", "Import mid\nLocal n = Four() + Twice()\n");
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    CHECK(packages.messages(*app) ==
          std::vector<std::string>{ "app/main.fb: Twice: 'Twice' is in lib.a, of the package lib, "
                                    "which this one doesn't depend on" });
    packages.remove();
}

TEST_CASE("a program in a folder named std isn't the standard library", "[driver]") {
    // Its files are its own package's, so they can't call the runtime either.
    const packages_t packages;
    packages.write("std/main.fb", "Import \"./rt\"\n");
    packages.write("std/rt.fb", "Extern \"runtime\"\n"
                                "    Function fb_string_live_count(): Int64\n"
                                "End Extern\n");
    const std::expected<fb::compilation_t, std::string> program =
        fb::compile_file(packages / "std/main.fb");
    REQUIRE(program);
    CHECK(packages.messages(*program) ==
          std::vector<std::string>{ "std/rt.fb: \"runtime\": only the standard library calls the "
                                    "runtime; C code's convention is \"C\"" });
    packages.remove();
}

TEST_CASE("an Import doesn't reach outside its package through a link", "[driver]") {
    const packages_t packages;
    packages.write("lib/project.toml", lib_toml);
    packages.write("lib/tools/a.fb", "Public Function Twice(): Int => 2\n");
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n");
    packages.write("app/main.fb", "Import std.core\n"
                                  "Import \"./shared/a\"\n"
                                  "Import app.shared\n"
                                  "Print(\"{Twice()}\")\n");
    if (!make_link(packages / "lib/tools", packages / "app/shared")) {
        packages.remove();
        SKIP("this system makes no links here");
    }

    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    CHECK(packages.messages(*app) ==
          std::vector<std::string>{
              "app/main.fb: \"./shared/a\": \"./shared/a\" is a link to 'lib/tools/a.fb', outside "
              "the package's folder; an Import stays in its package's files",
              "app/main.fb: app.shared: app.shared is a link to 'lib/tools', outside the package's "
              "folder; an Import stays in its package's files",
          });
    // Only the link goes, not what it leads to.
    std::error_code ignored;
    std::filesystem::remove(packages / "app/shared", ignored);
    packages.remove();
}

TEST_CASE("a module's file ends in .fb, in its letter case", "[driver]") {
    // A disk that ignores letter cases finds Up.FB for "./Up", which gets a warning, as the other
    // letter cases do; a disk that doesn't finds nothing.
    const packages_t packages;
    packages.write("quoted/Up.FB", "Public Function F(): Int => 7\n");
    packages.write("quoted/main.fb", "Import \"./Up\"\nLocal n = F()\n");
    if (std::filesystem::exists(packages / "quoted/Up.fb")) {
        const std::expected<fb::compilation_t, std::string> quoted =
            fb::compile_file(packages / "quoted/main.fb");
        REQUIRE(quoted);
        CHECK(packages.messages(*quoted) ==
              std::vector<std::string>{ "quoted/main.fb: \"./Up\": the disk spells its file Up.FB, "
                                        "in other letter cases, which only a disk that ignores "
                                        "them finds; a module's file ends in .fb" });
    }

    // A folder's Import finds no module in a folder of .FB files, on any disk, and says why.
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n");
    packages.write("app/sub/Down.FB", "Public Function G(): Int => 8\n");
    packages.write("app/main.fb", "Import app.sub\n");
    const std::expected<fb::compilation_t, std::string> folder = fb::compile_file(packages / "app");
    REQUIRE(folder);
    CHECK(packages.messages(*folder) ==
          std::vector<std::string>{ "app/main.fb: app.sub: there's no module app.sub: app/sub/ has "
                                    "Down.FB, but a module's file ends in .fb" });
    packages.remove();
}

TEST_CASE("a folder Import whose files are links leaving the package is one error", "[driver]") {
    // The folder is there, with a file in it, so it isn't reported missing too.
    const packages_t packages;
    packages.write("outside/a.fb", "Public Function Twice(): Int => 2\n");
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n");
    packages.write("app/main.fb", "Import app.shared\n");
    std::filesystem::create_directories(packages / "app/shared");
    if (!make_file_link(packages / "outside/a.fb", packages / "app/shared/a.fb")) {
        packages.remove();
        SKIP("this system makes no file links here");
    }

    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    CHECK(packages.messages(*app) ==
          std::vector<std::string>{
              "app/main.fb: app.shared: app/shared/a.fb is a link to 'outside/a.fb', outside the "
              "package's folder; an Import stays in its package's files",
          });
    std::error_code ignored;
    std::filesystem::remove(packages / "app/shared/a.fb", ignored);
    packages.remove();
}

TEST_CASE("Embed, EmbedText, and EmbedFolder read the program's files when it's built",
          "[driver]") {
    std::vector<fb::library_file_t> files{
        { .path = "data/hello.txt", .text = "hi" },
        {
            .path = "data/raw.bin",
            .text = std::string{ static_cast<char>(0xFF), static_cast<char>(0x00) },
        },
        { .path = "data/levels/a.txt", .text = "a" },
        { .path = "data/levels/.hidden", .text = "h" },
        { .path = "data/big.bin", .text = std::string((std::size_t{ 64 } * 1024 * 1024) + 1, 'x') },
        // Each is under the limit, but not the two together.
        { .path = "data/heavy/a.bin", .text = std::string(std::size_t{ 33 } * 1024 * 1024, 'a') },
        { .path = "data/heavy/b.bin", .text = std::string(std::size_t{ 33 } * 1024 * 1024, 'b') },
        {
            .path = "lib/reader.fb",
            .text = "Public Function Read(): String => EmbedText(\"../data/hello.txt\")\n",
        },
    };
    const std::vector<fb::library_file_t> library = with_std(std::move(files));

    // A path goes from the call's file's folder, and the compilation lists each file it embeds
    // once.
    const fb::compilation_t good = fb::compile("main.fb",
                                               "Import std.core\n"
                                               "Import \"./lib/reader\"\n"
                                               "Const Text = EmbedText(\"data/hello.txt\")\n"
                                               "Local raw = Embed(\"data/raw.bin\")\n"
                                               "Local levels = EmbedFolder(\"data/levels\")\n"
                                               "Print(Read(), raw, levels.Count)\n",
                                               library);
    CHECK(errors_of(good).empty());
    std::vector<std::string> embedded = good.embedded;
    std::ranges::sort(embedded);
    CHECK(embedded ==
          std::vector<std::string>{ "data/hello.txt", "data/levels/a.txt", "data/raw.bin" });

    // A folder of the program's own code isn't data, and the main file's folder would bring the
    // last build's executable along.
    const fb::compilation_t bad = fb::compile("main.fb",
                                              "Import \"./lib/reader\"\n"
                                              "Local a = Embed(\"data/missing.png\")\n"
                                              "Local b = Embed(\"../outside.txt\")\n"
                                              "Local c = Embed(\"/etc/passwd\")\n"
                                              "Local d = Embed(\"data/levels\")\n"
                                              "Local e = EmbedFolder(\"data/hello.txt\")\n"
                                              "Local f = EmbedText(\"data/raw.bin\")\n"
                                              "Local g = Embed(\"data/big.bin\")\n"
                                              "Local h = Embed(\"std/core/error.fb\")\n"
                                              "Local name = \"data/hello.txt\"\n"
                                              "Local i = Embed(name)\n"
                                              "Local j = Embed()\n"
                                              "Local k = EmbedFolder(\"data/..\")\n"
                                              "Local l = EmbedFolder(\"lib\")\n"
                                              "Local m = EmbedFolder(\"data/heavy\")\n",
                                              library);
    const std::vector<std::string> errors = errors_of(bad);
    REQUIRE(errors.size() == 13);
    CHECK(errors[0] ==
          "main.fb: \"data/missing.png\": \"data/missing.png\" isn't a file: there's nothing "
          "at data/missing.png");
    CHECK(errors[1] ==
          "main.fb: \"../outside.txt\": \"../outside.txt\" leaves the main file's folder, "
          "which a program's embedded files are in");
    CHECK(errors[2] ==
          "main.fb: \"/etc/passwd\": \"/etc/passwd\" isn't a path from this file's folder, "
          "like \"data/logo.png\" or \"../art/logo.png\"");
    CHECK(errors[3] == "main.fb: \"data/levels\": \"data/levels\" is a folder; EmbedFolder puts a "
                       "folder's files in the program");
    CHECK(errors[4] ==
          "main.fb: \"data/hello.txt\": \"data/hello.txt\" is a file; Embed puts a file's "
          "bytes in the program");
    CHECK(errors[5] ==
          "main.fb: \"data/raw.bin\": \"data/raw.bin\" isn't UTF-8 text, at byte 0, so it "
          "can't be a String; Embed gives its bytes");
    CHECK(errors[6] ==
          "main.fb: \"data/big.bin\": \"data/big.bin\" is a file more than 64 MiB, the most "
          "one embed puts in the program; read one that large when the program runs");
    CHECK(errors[7] ==
          "main.fb: \"std/core/error.fb\": \"std/core/error.fb\" is in a folder called std, "
          "which is the standard library's name");
    CHECK(errors[8] == "main.fb: name: Embed's path is a String known at compile time, like "
                       "\"data/logo.png\"");
    CHECK(errors[9] ==
          "main.fb: Embed(): Embed takes one argument, the path of a file from this file's "
          "folder, like Embed(\"data/logo.png\")");
    CHECK(errors[10] == "main.fb: \"data/..\": \"data/..\" holds the program's own code, main.fb; "
                        "EmbedFolder puts a folder of data in the program");
    CHECK(errors[11] == "main.fb: \"lib\": \"lib\" holds the program's own code, lib/reader.fb; "
                        "EmbedFolder puts a folder of data in the program");
    CHECK(errors[12] ==
          "main.fb: \"data/heavy\": \"data/heavy\" is a folder whose files are more than 64 MiB, "
          "the most one embed puts in the program; read one that large when the program runs");
}

TEST_CASE("an embed doesn't reach outside its package through a link, nor into another package",
          "[driver]") {
    const packages_t packages;
    packages.write("outside/secret.txt", "s");
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n");
    packages.write("app/data/kept.txt", "k");
    packages.write("app/inner/project.toml", "[project]\nname = \"inner\"\nversion = \"1.0.0\"\n");
    packages.write("app/inner/notes.txt", "n");
    packages.write("app/main.fb", "Local a = Embed(\"shared/secret.txt\")\n"
                                  "Local b = EmbedFolder(\"shared\")\n"
                                  "Local c = EmbedFolder(\"data\")\n"
                                  "Local d = Embed(\"inner/notes.txt\")\n"
                                  "Local e = EmbedFolder(\"inner\")\n");
    if (!make_link(packages / "outside", packages / "app/shared") ||
        !make_file_link(packages / "outside/secret.txt", packages / "app/data/secret.txt")) {
        std::error_code ignored;
        std::filesystem::remove(packages / "app/shared", ignored);
        packages.remove();
        SKIP("this system makes no links here");
    }

    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    const std::vector<std::string> messages = packages.messages(*app);
    REQUIRE(messages.size() == 5);
    CHECK(messages[0] == "app/main.fb: \"shared/secret.txt\": \"shared/secret.txt\" is a link to "
                         "'outside/secret.txt', outside the package's folder");
    CHECK(
        messages[1] ==
        "app/main.fb: \"shared\": \"shared\" is a link to 'outside', outside the package's folder");
    CHECK(messages[2] == "app/main.fb: \"data\": \"secret.txt\" in \"data\" is a link to "
                         "'outside/secret.txt', outside the package's folder");
    CHECK(messages[3] == "app/main.fb: \"inner/notes.txt\": \"inner/notes.txt\" is in the package "
                         "in 'app/inner', whose files only its own code embeds");
    CHECK(messages[4] == "app/main.fb: \"inner\": \"inner\" is in the package in 'app/inner', "
                         "whose files only its own code embeds");
    CHECK(app->embedded.empty());
    // Only the links go, not what they lead to.
    std::error_code ignored;
    std::filesystem::remove(packages / "app/shared", ignored);
    std::filesystem::remove(packages / "app/data/secret.txt", ignored);
    packages.remove();
}

TEST_CASE("EmbedFolder on disk leaves hidden files out, and refuses code, links, and packages in "
          "its folder, wherever they really are",
          "[driver]") {
    const packages_t packages;
    packages.write("app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n");
    packages.write("app/sub/mod.fb", "Public Function F(): Int => 1\n");
    packages.write("app/data/a.txt", "a");
    packages.write("app/data/deep/b.txt", "bb");
    packages.write("app/data/.hidden", "h");
    packages.write("app/data/.drafts/c.txt", "c");
    packages.write("app/loops/a.txt", "a");
    packages.write("app/store/kept.txt", "k");
    packages.write("app/store/inner/project.toml",
                   "[project]\nname = \"inner\"\nversion = \"1.0.0\"\n");
    packages.write("app/store/inner/notes.txt", "n");
    packages.write("app/main.fb", "Import \"./sub/mod\"\n"
                                  "Local a = EmbedFolder(\"data\")\n"
                                  "Local b = EmbedFolder(\"store\")\n"
                                  "Local c = EmbedFolder(\"loops\")\n"
                                  "Local d = EmbedFolder(\"code\")\n");
    const auto remove = [&] {
        std::error_code ignored;
        std::filesystem::remove(packages / "app/loops/again", ignored);
        std::filesystem::remove(packages / "app/code", ignored);
        packages.remove();
    };
    // A link back to its own folder is a cycle, and one to code is code.
    if (!make_link(packages / "app/loops", packages / "app/loops/again") ||
        !make_link(packages / "app/sub", packages / "app/code")) {
        remove();
        SKIP("this system makes no links here");
    }

    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    const std::vector<std::string> messages = packages.messages(*app);
    REQUIRE(messages.size() == 3);
    CHECK(messages[0] ==
          "app/main.fb: \"store\": \"store\" holds the package in 'app/store/inner', "
          "whose files only its own code embeds");
    CHECK(messages[1] == "app/main.fb: \"loops\": \"again\" in \"loops\" is a link, which "
                         "EmbedFolder doesn't follow, so the program is the same wherever it's "
                         "built; embed what it leads to by its own path");
    CHECK(messages[2] == "app/main.fb: \"code\": \"code\" holds the program's own code, "
                         "app/sub/mod.fb; EmbedFolder puts a folder of data in the program");
    // The folder that's data has its files by their paths in it, with '/', in order.
    REQUIRE(app->analysis.embeds.size() == 1);
    const std::vector<fb::embedded_file_t>& data = app->analysis.embeds.front();
    REQUIRE(data.size() == 2);
    CHECK(data[0].path == "a.txt");
    CHECK(*data[0].bytes == "a");
    CHECK(data[1].path == "deep/b.txt");
    CHECK(*data[1].bytes == "bb");

    // A disk that ignores letter cases finds a path spelled in others, which gets a warning, as
    // an Import's does, and still finds the code in a folder so spelled.
    if (std::filesystem::exists(packages / "app/DATA")) {
        packages.write("app/main.fb", "Import \"./sub/mod\"\n"
                                      "Local a = Embed(\"DATA/A.TXT\")\n"
                                      "Local b = EmbedFolder(\"SUB\")\n");
        const std::expected<fb::compilation_t, std::string> respelled =
            fb::compile_file(packages / "app");
        REQUIRE(respelled);
        CHECK(packages.messages(*respelled) ==
              std::vector<std::string>{
                  "app/main.fb: \"DATA/A.TXT\": the disk spells it \"data/a.txt\", in other "
                  "letter cases, which only a disk that ignores them finds",
                  "app/main.fb: \"SUB\": the disk spells it \"sub\", in other letter cases, "
                  "which only a disk that ignores them finds",
                  "app/main.fb: \"SUB\": \"SUB\" holds the program's own code, app/sub/mod.fb; "
                  "EmbedFolder puts a folder of data in the program",
              });
    }

    remove();
}

TEST_CASE("an embedded file is read once, and its embeds share its bytes", "[driver]") {
    const std::vector<fb::library_file_t> library = with_std({
        { .path = "data/one.bin", .text = "one" },
    });
    // An argument is checked again for each overload a call tries, and an outer call's tries
    // check the inner ones again.
    const fb::compilation_t compilation = fb::compile(
        "main.fb",
        "Function F(a: UInt8[], b: Int8): UInt8[] => a\n"
        "Function F(a: UInt8[], b: Int16): UInt8[] => a\n"
        "Function F(a: UInt8[], b: Int64): UInt8[] => a\n"
        "Local x = F(F(F(Embed(\"data/one.bin\"), 1 As Int64), 1 As Int64), 1 As Int64)\n"
        "Local y = Embed(\"data/one.bin\")\n"
        "Local z = EmbedFolder(\"data\")\n",
        library);
    REQUIRE(errors_of(compilation).empty());
    const std::vector<std::vector<fb::embedded_file_t>>& embeds = compilation.analysis.embeds;
    // Both Embeds have one place, and the folder's file has the same bytes.
    REQUIRE(embeds.size() == 2);
    CHECK(embeds[0].front().path.empty());
    CHECK(embeds[1].front().path == "one.bin");
    CHECK(embeds[0].front().bytes == embeds[1].front().bytes);
    CHECK(compilation.embedded == std::vector<std::string>{ "data/one.bin" });
}

namespace {

/// The physics library the interface file tests build: Public declarations, generic, @Inline, and
/// Const code, which the interface file keeps, and the Private code it reaches.
void write_physics(const packages_t& packages, const std::string_view folder) {
    packages.write(std::format("{}/project.toml", folder),
                   "[project]\nname = \"physics\"\nversion = \"0.2.0\"\ntype = \"library\"\n");
    packages.write(std::format("{}/shapes.fb", folder),
                   "Import std.core\n"
                   "// A comment, which the interface file leaves out.\n"
                   "Public Const Pi: Float64 = 3.25\n"
                   "Public Const Label = \"a \\\"{{x}}\\\" \\u{E9}\"\n"
                   "Public Global Count = 0\n"
                   "Public Global Unit = Circle(1.0)\n"
                   "Public Class Circle\n"
                   "    Public Field Radius: Float64\n"
                   "    Field secret: Int = 7\n"
                   "    Public New(r: Float64)\n"
                   "        Radius = r\n"
                   "    End New\n"
                   "    Public Method Area(): Float64 => Pi * Radius * Radius\n"
                   "    @Inline Public Method Twice(): Float64 => Twofold(Radius)\n"
                   "    Method Hidden(): Int => secret\n"
                   "End Class\n"
                   "Function Twofold(x: Float64): Float64 => x * 2.0\n"
                   "Function Unused(): Int => 3\n"
                   "Public Function Larger<T: Comparable>(a: T, b: T): T\n"
                   "#If DEBUG\n"
                   "    Return Pick(a > b, a, b)\n"
                   "#Else\n"
                   "    Return Secret()\n"
                   "#End If\n"
                   "End Function\n"
                   "Function Pick<T>(first: Bool, a: T, b: T): T => first ? a : b\n");
    packages.write(std::format("{}/world.fb", folder),
                   "Import \"./shapes\"\n"
                   "Public Struct World\n"
                   "    Public Field Gravity: Float64 = -9.5\n"
                   "    Public Static Field Worlds: Int\n"
                   "    Public Property Bodies: Int\n"
                   "        Get => 1\n"
                   "    End Property\n"
                   "End Struct\n"
                   "Public Function Make(): Circle => Circle(2.0)\n");
}

/// The diagnostics of app/main.fb, `main`, built in `mode` with the dependency `name` in the
/// folder `library`. A message names where a declaration is, its file or the interface file, so
/// each of the library's `files` and its interface file are shown alike, as "<name>".
std::vector<std::string> app_messages(const packages_t& packages, const std::string_view name,
                                      const std::string_view library, const std::string_view main,
                                      const fb::build_mode_t mode,
                                      const std::vector<std::string_view>& files) {
    packages.write("app/project.toml",
                   std::format("[project]\nname = \"app\"\nversion = \"1.0.0\"\n\n"
                               "[dependencies]\n{} = {{ path = \"../{}\" }}\n",
                               name, library));
    packages.write("app/main.fb", main);
    const std::expected<fb::compilation_t, std::string> app =
        fb::compile_file(packages / "app", mode);
    if (!app) {
        return { app.error() };
    }

    const std::string interface = std::format("{}/lib/{}/{}/{}.fbi", library, fb::build_target(),
                                              fb::mode_folder(mode), name);
    std::vector<std::string_view> shown_as_one = files;
    shown_as_one.emplace_back(interface);
    std::vector<std::string> shown = packages.messages(*app);
    for (std::string& message : shown) {
        for (const std::string_view file : shown_as_one) {
            for (std::size_t at = message.find(file); at != std::string::npos;
                 at = message.find(file)) {
                message.replace(at, file.size(), std::format("<{}>", name));
            }
        }
    }

    return shown;
}

} // namespace

TEST_CASE("a library's interface file holds what programs compile against", "[driver]") {
    const packages_t packages;
    write_physics(packages, "physics");
    const std::expected<fb::compilation_t, std::string> physics =
        fb::compile_file(packages / "physics");
    REQUIRE(physics);
    CHECK(packages.messages(*physics).empty());
    fb::diagnostic_bag_t diagnostics;
    const std::string text =
        fb::write_interface(*physics, fb::build_mode_t::DEBUG, diagnostics).value_or("");
    REQUIRE_FALSE(text.empty());
    CHECK(diagnostics.diagnostics().empty());

    const fb::interface_header_t header =
        fb::read_interface_header(text).value_or(fb::interface_header_t{});
    CHECK(header.name == "physics");
    CHECK(header.version == "0.2.0");
    // Each module is its Imports and what programs compile against, without comments or the
    // lines an #If leaves out.
    const std::string body = text.substr(text.find('\n') + 1);
    CHECK(body == "// module physics/shapes.fb 18\n"
                  "Import std.core\n"
                  "Public Const Pi: Float64 = 3.25\n"
                  "Public Const Label: String = \"a \\\"{{x}}\\\" \\u{E9}\"\n"
                  "Public Global Count: Int32 = ...\n"
                  "Public Global Unit: Circle = ...\n"
                  "Public Class Circle\n"
                  "    Public Field Radius: Float64\n"
                  "    Field secret: Int = 7\n"
                  "    Public New(r: Float64) => ...\n"
                  "    Public Method Area(): Float64 => ...\n"
                  "    @Inline Public Method Twice(): Float64 => Twofold(Radius)\n"
                  "    Method Hidden(): Int => ...\n"
                  "End Class\n"
                  "Function Twofold(x: Float64): Float64 => ...\n"
                  "Public Function Larger<T: Comparable>(a: T, b: T): T\n"
                  "    Return Pick(a > b, a, b)\n"
                  "End Function\n"
                  "Function Pick<T>(first: Bool, a: T, b: T): T => first ? a : b\n"
                  "// module physics/world.fb 9\n"
                  "Import \"./shapes\"\n"
                  "Public Struct World\n"
                  "    Public Field Gravity: Float64 = -9.5\n"
                  "    Public Static Field Worlds: Int = ...\n"
                  "    Public Property Bodies: Int\n"
                  "        Get => ...\n"
                  "    End Property\n"
                  "End Struct\n"
                  "Public Function Make(): Circle => ...\n");

    // The same library, prebuilt: its interface file in place of its source.
    packages.write("prebuilt/project.toml",
                   "[project]\nname = \"physics\"\nversion = \"0.2.0\"\ntype = \"library\"\n");
    packages.write(std::format("prebuilt/lib/{}/debug/physics.fbi", fb::build_target()), text);
    const std::expected<fb::compilation_t, std::string> checked =
        fb::compile_file(packages / "prebuilt");
    REQUIRE(checked);
    CHECK(packages.messages(*checked).empty());

    // A program checks against it as it does against the source.
    const std::string main =
        "Import std.core\n"
        "Import physics\n"
        "Local c = Circle(3.0)\n"
        "Print(\"{c.Area()} {c.Twice()} {Larger(1, 2)} {Count} {Pi} {Label}\")\n"
        "Print(\"{Unit.Radius} {Make().Radius} {World().Gravity}\")\n"
        "Print(\"{World.Worlds} {World().Bodies}\")\n"
        "Local h = c.Hidden()\n"
        "Local d = Twofold(1.0)\n"
        "Local s = c.secret\n"
        "Local n: Float64 = Larger(1, 2)\n";
    // A kept type's Private members are there, still Private.
    const std::vector<std::string_view> files{ "physics/shapes.fb", "physics/world.fb" };
    const std::vector<std::string> from_source =
        app_messages(packages, "physics", "physics", main, fb::build_mode_t::DEBUG, files);
    REQUIRE(from_source.size() == 3);
    CHECK(from_source.front() == "app/main.fb: Hidden: 'Hidden' is private to <physics>");
    CHECK(app_messages(packages, "physics", "prebuilt", main, fb::build_mode_t::DEBUG, files) ==
          from_source);
    packages.remove();
}

TEST_CASE("an interface file keeps what its kept code refers to, written as it parses",
          "[driver]") {
    const packages_t packages;
    packages.write("reach/project.toml",
                   "[project]\nname = \"reach\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    // A Private generic and @Inline Function that share their names with a Public type's
    // members, which nothing kept calls, stay closed.
    packages.write("reach/reach.fb", "Import std.core\n"
                                     "Public Class Asset\n"
                                     "    Public Field Key: Int = 1\n"
                                     "    Public Method Text(): String => \"asset\"\n"
                                     "End Class\n"
                                     "Function Text<T: Printable>(x: T): String => \"secret {x}\"\n"
                                     "@Inline Function Key(): Int => 1234 * 5678\n"
                                     "Public Function Show(): String => Text(Key())\n"
                                     "Public Class E1 Extends Error\n"
                                     "    Public New()\n"
                                     "        Super.New(\"e1\")\n"
                                     "    End New\n"
                                     "End Class\n"
                                     "Function Work(n: Int): Int Throws E1\n"
                                     "    If n < 0 Then Throw E1()\n"
                                     "    Return n\n"
                                     "End Function\n"
                                     "Async Function Later(): Int Throws E1 => Try Work(1)\n"
                                     "Public Global MakeTask = Function() => Later()\n");
    const std::expected<fb::compilation_t, std::string> reach =
        fb::compile_file(packages / "reach");
    REQUIRE(reach);
    CHECK(packages.messages(*reach).empty());
    fb::diagnostic_bag_t diagnostics;
    const std::string text =
        fb::write_interface(*reach, fb::build_mode_t::DEBUG, diagnostics).value_or("");
    REQUIRE_FALSE(text.empty());
    CHECK_FALSE(text.contains("secret"));
    CHECK_FALSE(text.contains("1234"));
    // A Func's result that's a Task with a Throws is in parentheses, as the source writes it.
    CHECK(text.contains("Public Global MakeTask: Func(): (Task<Int32> Throws E1) = ...\n"));
    packages.write("prebuilt/project.toml",
                   "[project]\nname = \"reach\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    packages.write(std::format("prebuilt/lib/{}/debug/reach.fbi", fb::build_target()), text);
    const std::expected<fb::compilation_t, std::string> checked =
        fb::compile_file(packages / "prebuilt");
    REQUIRE(checked);
    CHECK(packages.messages(*checked).empty());
    packages.remove();
}

TEST_CASE("an interface file keeps each kind of declaration as programs need it", "[driver]") {
    const packages_t packages;
    packages.write("kinds/project.toml", "[project]\nname = \"kinds\"\nversion = \"1.2.3\"\n");
    packages.write("kinds/a.fb", "Import std.core\n"
                                 "Public Enum Color\n"
                                 "    RED\n"
                                 "    GREEN = 5\n"
                                 "End Enum\n"
                                 "Public Alias Meters = Float64\n"
                                 "Public Interface Named\n"
                                 "    Method Name(): String\n"
                                 "End Interface\n"
                                 "Public Attribute Tag(Text: String)\n"
                                 "Public Class Base Implements Named\n"
                                 "    Protected Field level: Int\n"
                                 "    New()\n"
                                 "        level = 1\n"
                                 "    End New\n"
                                 "    Public Method Name(): String => \"base\"\n"
                                 "    Protected Method Grow()\n"
                                 "        level++\n"
                                 "    End Method\n"
                                 "End Class\n"
                                 "Public Function MakeBase(): Base => Base()\n"
                                 "Extend Int32\n"
                                 "    Public Method Squared(): Int32 => Self * Self\n"
                                 "    Method Cubed(): Int32 => Self * Self * Self\n"
                                 "End Extend\n"
                                 "Public Extern \"C\"\n"
                                 "    Function abs(x: Int32): Int32\n"
                                 "End Extern\n"
                                 "Public Const Mode: String = DebugOrRelease()\n"
                                 "Const Function DebugOrRelease(): String\n"
                                 "#If RELEASE\n"
                                 "    Return \"release\"\n"
                                 "#Else\n"
                                 "    Return \"debug\"\n"
                                 "#End If\n"
                                 "End Function\n");
    // A folder's name needn't be an identifier, so a hidden Import names a module by its path.
    packages.write("kinds/my-parts/c.fb", "Import \"../a\"\n"
                                          "Public Global Far = MakeBase()\n");
    packages.write("kinds/b.fb", "Import \"./a\"\n"
                                 "Import \"./my-parts/c\"\n"
                                 "Public Global Home = MakeBase()\n"
                                 "Public Global Colors = [Color.RED]\n");
    const std::expected<fb::compilation_t, std::string> kinds =
        fb::compile_file(packages / "kinds", fb::build_mode_t::RELEASE);
    REQUIRE(kinds);
    CHECK(packages.messages(*kinds).empty());
    fb::diagnostic_bag_t diagnostics;
    const std::string text =
        fb::write_interface(*kinds, fb::build_mode_t::RELEASE, diagnostics).value_or("");
    REQUIRE_FALSE(text.empty());
    CHECK(text.substr(text.find('\n') + 1) == "// module kinds/a.fb 25\n"
                                              "Import std.core\n"
                                              "Public Enum Color\n"
                                              "    RED\n"
                                              "    GREEN = 5\n"
                                              "End Enum\n"
                                              "Public Alias Meters = Float64\n"
                                              "Public Interface Named\n"
                                              "    Method Name(): String\n"
                                              "End Interface\n"
                                              "Public Attribute Tag(Text: String)\n"
                                              "Public Class Base Implements Named\n"
                                              "    Protected Field level: Int\n"
                                              "    New() => ...\n"
                                              "    Public Method Name(): String => ...\n"
                                              "    Protected Method Grow() => ...\n"
                                              "End Class\n"
                                              "Public Function MakeBase(): Base => ...\n"
                                              "Extend Int32\n"
                                              "    Public Method Squared(): Int32 => ...\n"
                                              "    Method Cubed(): Int32 => ...\n"
                                              "End Extend\n"
                                              "Public Extern \"C\"\n"
                                              "    Function abs(x: Int32): Int32\n"
                                              "End Extern\n"
                                              "Public Const Mode: String = \"release\"\n"
                                              "// module kinds/my-parts/c.fb 3\n"
                                              "Import \"../a\"\n"
                                              "Import \"../a\" As __fbi1\n"
                                              "Public Global Far: __fbi1.Base = ...\n"
                                              "// module kinds/b.fb 5\n"
                                              "Import \"./a\"\n"
                                              "Import \"./my-parts/c\"\n"
                                              "Import \"./a\" As __fbi1\n"
                                              "Public Global Home: __fbi1.Base = ...\n"
                                              "Public Global Colors: __fbi1.Color[] = ...\n");

    // Programs see the same: a Private New stays one, and so does an Extend's Private Method.
    packages.write("prebuilt/project.toml", "[project]\nname = \"kinds\"\nversion = \"1.2.3\"\n");
    packages.write(std::format("prebuilt/lib/{}/release/kinds.fbi", fb::build_target()), text);
    const std::string main = "Import std.core\n"
                             "Import kinds\n"
                             "Class Leaf Extends Base\n"
                             "    Method Bump()\n"
                             "        Grow()\n"
                             "        level += 1\n"
                             "    End Method\n"
                             "End Class\n"
                             "Local m: Meters = 2.5\n"
                             "Print(\"{Color.GREEN} {m} {Home.Name()} {Colors.Count} {Mode}\")\n"
                             "Print(\"{(3).Squared()} {abs(-2)}\")\n"
                             "Local b = Base()\n"
                             "Local c = (3).Cubed()\n";
    const std::vector<std::string_view> files{ "kinds/a.fb", "kinds/b.fb", "kinds/my-parts/c.fb" };
    const std::vector<std::string> from_source =
        app_messages(packages, "kinds", "kinds", main, fb::build_mode_t::RELEASE, files);
    REQUIRE(from_source.size() == 3);
    CHECK(from_source.back() == "app/main.fb: Cubed: 'Cubed' is private to <kinds>");
    CHECK(app_messages(packages, "kinds", "prebuilt", main, fb::build_mode_t::RELEASE, files) ==
          from_source);
    packages.remove();
}

TEST_CASE("a prebuilt library is its interface file, which has to be fb's and the package's",
          "[driver]") {
    const packages_t packages;
    write_physics(packages, "physics");
    const std::expected<fb::compilation_t, std::string> physics =
        fb::compile_file(packages / "physics");
    REQUIRE(physics);
    fb::diagnostic_bag_t diagnostics;
    const std::string text =
        fb::write_interface(*physics, fb::build_mode_t::DEBUG, diagnostics).value_or("");
    REQUIRE_FALSE(text.empty());

    const std::string interface = std::format("lib/{}/debug/physics.fbi", fb::build_target());
    const std::string main = "Import std.core\nImport physics\nPrint(\"{Pi}\")\n";
    const auto checked = [&](const std::string_view toml, const std::string_view written) {
        packages.write("prebuilt/project.toml", toml);
        packages.write("prebuilt/" + interface, written);
        return app_messages(packages, "physics", "prebuilt", main, fb::build_mode_t::DEBUG, {});
    };

    const std::string toml =
        "[project]\nname = \"physics\"\nversion = \"0.2.0\"\ntype = \"library\"\n";
    CHECK(checked(toml, text).empty());

    // A program checks against the interface file alone, and builds with the binary beside it.
    packages.write("app/main.fb", main);
    const std::expected<fb::compilation_t, std::string> app = fb::compile_file(packages / "app");
    REQUIRE(app);
    REQUIRE(app->interfaces.size() == 1);
    const fb::mir_module_t module = fb::lower(app->ast, app->analysis);
    const std::expected<void, std::string> built =
        fb::build_program(module, *app, packages / "app/build/app.exe", fb::build_mode_t::DEBUG);
    REQUIRE_FALSE(built);
    CHECK(built.error().ends_with("physics.fblib, the prebuilt library's binary beside its "
                                  "interface file"));

    // Its first line names the build, which has to be the package's.
    const std::string named = std::format(
        "prebuilt/project.toml: \"physics\": {} is the interface of physics v0.2.0, but "
        "project.toml says physics v0.3.0",
        interface);
    CHECK(checked("[project]\nname = \"physics\"\nversion = \"0.3.0\"\n", text) ==
          std::vector<std::string>{ named });

    // A file that isn't one fb writes, or whose modules aren't the package's, isn't used.
    const std::string broken = std::format(
        "prebuilt/project.toml: \"physics\": {} isn't an interface file that fb writes", interface);
    CHECK(checked(toml, "Public Const Pi = 3\n") == std::vector<std::string>{ broken });
    CHECK(checked(toml, text.substr(0, text.find('\n') + 1) + "// module physics/a.fb 9\n") ==
          std::vector<std::string>{ broken });
    std::string other = text;
    other.replace(other.find("physics/world.fb"), 7, "chemist");
    CHECK(checked(toml, other) == std::vector<std::string>{ broken });
    std::string twice = text;
    twice.replace(twice.find("physics/world.fb"), 16, "physics/shapes.fb");
    CHECK(checked(toml, twice) == std::vector<std::string>{ broken });

    // A package is its source or prebuilt, not both.
    packages.write("prebuilt/extra.fb", "Public Const E = 1\n");
    CHECK(checked(toml, text) ==
          std::vector<std::string>{ std::format(
              "prebuilt/project.toml: \"physics\": physics has both .fb source and a prebuilt "
              "build, {}; a package is one or the other",
              interface) });
    packages.remove();
}

TEST_CASE("code an interface file keeps can't embed the library's files", "[driver]") {
    const packages_t packages;
    packages.write("art/project.toml", "[project]\nname = \"art\"\nversion = \"0.1.0\"\n");
    packages.write("art/logo.txt", "logo");
    packages.write("art/a.fb",
                   "Import std.core\n"
                   "Public Function Logo(): String => EmbedText(\"logo.txt\")\n"
                   "@Inline Public Function Inlined(): String => EmbedText(\"logo.txt\")\n"
                   "Public Function Generic<T>(x: T): String => EmbedText(\"logo.txt\")\n");
    const std::expected<fb::compilation_t, std::string> art = fb::compile_file(packages / "art");
    REQUIRE(art);
    CHECK(packages.messages(*art).empty());
    fb::diagnostic_bag_t diagnostics;
    CHECK_FALSE(fb::write_interface(*art, fb::build_mode_t::DEBUG, diagnostics));
    std::vector<std::string> errors;
    for (const fb::diagnostic_t& diagnostic : diagnostics.diagnostics()) {
        errors.emplace_back(art->sources.get(diagnostic.span.file).slice(diagnostic.span));
    }

    // Logo's body stays in the library, where it embeds as it does in a program.
    CHECK(errors ==
          std::vector<std::string>{ "EmbedText(\"logo.txt\")", "EmbedText(\"logo.txt\")" });
    REQUIRE_FALSE(diagnostics.diagnostics().empty());
    CHECK(diagnostics.diagnostics().front().message ==
          "the library's interface file keeps this code, which programs compile themselves, so it "
          "can't embed the library's files; embed them in code that stays in the library");
    packages.remove();
}

TEST_CASE("programs check against an interface file as they do against the library's source",
          "[driver]") {
    const packages_t packages;
    packages.write("deep/project.toml", "[project]\nname = \"deep\"\nversion = \"0.4.0\"\n");
    // Operators, an Interface a Private Method implements, and an Extend that only implements.
    packages.write("deep/money.fb",
                   "Import std.core\n"
                   "Import \"./threads\"\n"
                   "Public Struct Money\n"
                   "    Public Field Cents: Int\n"
                   "    Operator +(a: Money, b: Money): Money => Money(a.Cents + b.Cents)\n"
                   "    Operator ==(a: Money, b: Money): Bool => a.Cents == b.Cents\n"
                   "    Operator [](i: Int): Int => Cents + i\n"
                   "    Operator []=(i: Int, value: Int)\n"
                   "        Cents = value - i\n"
                   "    End Operator\n"
                   "End Struct\n"
                   "Public Interface Named\n"
                   "    Method Name(): String\n"
                   "End Interface\n"
                   "Public Class Tagged Implements Named\n"
                   "    Method Name(): String => \"tagged\"\n"
                   "End Class\n"
                   "Public Class Plain\n"
                   "End Class\n"
                   "Extend Plain Implements Named\n"
                   "    Method Name(): String => \"plain\"\n"
                   "End Extend\n"
                   "Public Struct __fbi1\n"
                   "End Struct\n"
                   "Public Global Far = Inner(4)\n");
    // Generic Extends, and a kept body whose For In calls a Private Class's Iterate.
    packages.write("deep/generic.fb",
                   "Import std.core\n"
                   "Import std.collections\n"
                   "Public Struct Box<T>\n"
                   "    Public Field Item: T\n"
                   "End Struct\n"
                   "Extend Box<T>\n"
                   "    Public Method Twice(): (T, T) => (Item, Item)\n"
                   "End Extend\n"
                   "Extend T[]\n"
                   "    Public Method Second(): T => Self[1]\n"
                   "End Extend\n"
                   "Extend Box<T>\n"
                   "    Where T: Numeric\n"
                   "    Public Method Sum(): T => Item + Item\n"
                   "End Extend\n"
                   "Class Bag\n"
                   "    Field items: Int[]\n"
                   "    New()\n"
                   "        items = [1, 2, 3]\n"
                   "    End New\n"
                   "    Method Iterate(): ArrayIterator<Int> => items.Iterate()\n"
                   "End Class\n"
                   "Public Function Total<T>(x: T): Int\n"
                   "    Local sum = 0\n"
                   "    For v In Bag()\n"
                   "        sum += v\n"
                   "    End For\n"
                   "    Return sum\n"
                   "End Function\n");
    // A Class's Field default, which its New runs in the binary, can embed; a New calls the
    // base's New with arguments; and what only the main thread can run.
    packages.write("deep/logo.txt", "logo");
    packages.write("deep/threads.fb", "Import std.core\n"
                                      "Public Class Art\n"
                                      "    Field logo: String = EmbedText(\"logo.txt\")\n"
                                      "    Public Method Logo(): String => logo\n"
                                      "End Class\n"
                                      "Public Class Inner\n"
                                      "    Public Field V: Int\n"
                                      "    Public New(v: Int)\n"
                                      "        V = v\n"
                                      "    End New\n"
                                      "End Class\n"
                                      "Public Class Outer Extends Inner\n"
                                      "    Public New()\n"
                                      "        Super.New(1)\n"
                                      "    End New\n"
                                      "    Method Secret(): Int => 2\n"
                                      "End Class\n"
                                      "Global score: Int = 0\n"
                                      "Public Function Bump(): Int\n"
                                      "    score += 1\n"
                                      "    Return score\n"
                                      "End Function\n"
                                      "@MainThread Public Function Draw()\n"
                                      "End Function\n"
                                      "Public Function Paint()\n"
                                      "    Draw()\n"
                                      "End Function\n"
                                      "Public Struct Stats\n"
                                      "    Public Field N: Int\n"
                                      "    Public Property Score: Int\n"
                                      "        Get => score\n"
                                      "        Set(value)\n"
                                      "            score = value\n"
                                      "        End Set\n"
                                      "    End Property\n"
                                      "End Struct\n"
                                      "Public Global Common = Inner(3)\n");
    const std::expected<fb::compilation_t, std::string> deep = fb::compile_file(packages / "deep");
    REQUIRE(deep);
    CHECK(packages.messages(*deep).empty());
    fb::diagnostic_bag_t diagnostics;
    const std::string text =
        fb::write_interface(*deep, fb::build_mode_t::DEBUG, diagnostics).value_or("");
    CHECK(diagnostics.diagnostics().empty());
    REQUIRE_FALSE(text.empty());
    // A hidden Import's qualifier is one the module's own names aren't.
    CHECK(text.contains("\nImport \"./threads\" As __fbi2\nPublic Struct Money\n"));
    CHECK(text.contains("\nPublic Global Far: __fbi2.Inner = ...\n"));
    CHECK(text.contains("\n@MainThreadOnly(\"uses the Global 'score', which only the main thread "
                        "can, unless it's an Atomic or a Mutex\") Public Function Bump(): Int => "
                        "...\n"));
    packages.write("prebuilt/project.toml", "[project]\nname = \"deep\"\nversion = \"0.4.0\"\n");
    packages.write(std::format("prebuilt/lib/{}/debug/deep.fbi", fb::build_target()), text);
    const std::expected<fb::compilation_t, std::string> checked =
        fb::compile_file(packages / "prebuilt");
    REQUIRE(checked);
    CHECK(packages.messages(*checked).empty());

    const std::string main =
        "Import std.core\n"
        "Import deep\n"
        "Local m = Money(5) + Money(6)\n"
        "m[1] = 9\n"
        "Print(\"{m.Cents} {m == Money(8)} {m[2]}\")\n"
        "Local n: Named = Tagged()\n"
        "Local p: Named = Plain()\n"
        "Print(\"{n.Name()} {p.Name()}\")\n"
        "Local b = Box<Int>(Item: 2)\n"
        "Print(\"{b.Twice()} {[1, 2].Second()} {b.Sum()} {Total(1)} {Art().Logo()}\")\n"
        "Print(\"{Outer().V} {Common.V}\")\n"
        "Class Leaf Extends Outer\n"
        "    Method Secret(): Int => 3\n"
        "End Class\n"
        "Local bumped = Spawn(Function() => Bump())\n"
        "Local painted = Spawn(Function() => Paint())\n"
        "Local read = Spawn(Function() => Stats().Score)\n"
        "Local tagged = Tagged()\n"
        "Print(\"{tagged.Name()}\")\n";
    const std::vector<std::string_view> files{
        "deep/money.fb",
        "deep/generic.fb",
        "deep/threads.fb",
    };
    const std::vector<std::string> from_source =
        app_messages(packages, "deep", "deep", main, fb::build_mode_t::DEBUG, files);
    // Overriding a Private Method, three works on other threads, and a Private Name.
    CHECK(from_source.size() == 5);
    CHECK(app_messages(packages, "deep", "prebuilt", main, fb::build_mode_t::DEBUG, files) ==
          from_source);
    packages.remove();

    // Only an interface file has @MainThreadOnly.
    CHECK(
        errors_of(fb::compile("main.fb", "@MainThreadOnly(\"x\") Function F()\nEnd Function\n")) ==
        std::vector<std::string>{ "main.fb: MainThreadOnly: '@MainThreadOnly' isn't an "
                                  "attribute; an Attribute declaration makes one" });
}

TEST_CASE("a library's .proto files go in its interface file as the modules fb makes of them",
          "[driver]") {
    const packages_t packages;
    packages.write("pro/project.toml", "[project]\nname = \"pro\"\nversion = \"0.1.0\"\n");
    packages.write("pro/sub/game.proto", "syntax = \"proto3\";\n"
                                         "import \"sub/score.proto\";\n"
                                         "message Player { string name = 1; Score score = 2; }\n");
    packages.write("pro/sub/score.proto", "syntax = \"proto3\";\n"
                                          "message Score { int32 points = 1; }\n");
    packages.write("pro/sub/deep.fb", "Import \"game.proto\"\n"
                                      "Public Global Hero = Player()\n"
                                      "Public Function Make(): Player => Player(name: \"a\")\n");
    packages.write("pro/top.fb", "Import \"./sub/deep\"\n"
                                 "Public Global Top = Make()\n");
    const std::expected<fb::compilation_t, std::string> pro = fb::compile_file(packages / "pro");
    REQUIRE(pro);
    CHECK(packages.messages(*pro).empty());
    fb::diagnostic_bag_t diagnostics;
    const std::string text =
        fb::write_interface(*pro, fb::build_mode_t::DEBUG, diagnostics).value_or("");
    REQUIRE_FALSE(text.empty());
    CHECK(text.contains("\n// module pro/sub/game.proto "));
    CHECK(text.contains("\nImport \"pro/sub/score.proto\" As score_\n"));
    CHECK(text.contains("\nImport \"../sub/game.proto\" As __fbi1\n"));
    CHECK(text.contains("\nImport \"./sub/game.proto\" As __fbi1\n"));

    packages.write("prebuilt/project.toml", "[project]\nname = \"pro\"\nversion = \"0.1.0\"\n");
    packages.write(std::format("prebuilt/lib/{}/debug/pro.fbi", fb::build_target()), text);
    const std::string main = "Import std.core\n"
                             "Import pro\n"
                             "Import pro.sub.deep\n"
                             "Print(\"{Top.name} {Hero.score.points} {Make().name}\")\n"
                             "Local wrong: Int = Hero\n";
    const std::vector<std::string_view> files{ "pro/top.fb", "pro/sub/deep.fb" };
    const std::vector<std::string> from_source =
        app_messages(packages, "pro", "pro", main, fb::build_mode_t::DEBUG, files);
    CHECK(from_source.size() == 2);
    CHECK(app_messages(packages, "pro", "prebuilt", main, fb::build_mode_t::DEBUG, files) ==
          from_source);
    packages.remove();
}

TEST_CASE("an interface file can't write a type whose module no Import can name", "[driver]") {
    const packages_t packages;
    packages.write("b/project.toml", "[project]\nname = \"b\"\nversion = \"0.1.0\"\n");
    packages.write("b/my-k.fb", "Public Class K\nEnd Class\nPublic Function MakeK(): K => K()\n");
    packages.write("a/project.toml", "[project]\nname = \"a\"\nversion = \"0.1.0\"\n\n"
                                     "[dependencies]\nb = { path = \"../b\" }\n");
    packages.write("a/a.fb", "Import b\nPublic Global G = MakeK()\n");
    const std::expected<fb::compilation_t, std::string> a = fb::compile_file(packages / "a");
    REQUIRE(a);
    CHECK(packages.messages(*a).empty());
    fb::diagnostic_bag_t diagnostics;
    CHECK_FALSE(fb::write_interface(*a, fb::build_mode_t::DEBUG, diagnostics));
    REQUIRE(diagnostics.diagnostics().size() == 1);
    CHECK(diagnostics.diagnostics().front().message ==
          "the interface file can't write the type of 'G', since its module, b.my-k, has no name "
          "an Import takes; write the type");
    packages.remove();
}

TEST_CASE("a prebuilt package's own build, interface, and modules are checked", "[driver]") {
    const packages_t packages;
    write_physics(packages, "physics");
    const std::expected<fb::compilation_t, std::string> physics =
        fb::compile_file(packages / "physics");
    REQUIRE(physics);
    fb::diagnostic_bag_t diagnostics;
    const std::string text =
        fb::write_interface(*physics, fb::build_mode_t::DEBUG, diagnostics).value_or("");
    REQUIRE_FALSE(text.empty());
    const std::string toml =
        "[project]\nname = \"physics\"\nversion = \"0.2.0\"\ntype = \"library\"\n";
    packages.write("prebuilt/project.toml", toml);

    // Only the debug build is there, so a release build has none.
    packages.write(std::format("prebuilt/lib/{}/debug/physics.fbi", fb::build_target()), text);
    const std::string main = "Import std.core\nImport physics\nPrint(\"{Pi}\")\n";
    CHECK(app_messages(packages, "physics", "prebuilt", main, fb::build_mode_t::RELEASE, {}) ==
          std::vector<std::string>{
              std::format("prebuilt/project.toml: \"physics\": physics is prebuilt, but not for "
                          "this build: there's no lib/{}/release/physics.fbi",
                          fb::build_target()) });

    // A copy with Windows' line breaks reads the same.
    std::string crlf;
    for (const char c : text) {
        crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    }

    packages.write(std::format("prebuilt/lib/{}/debug/physics.fbi", fb::build_target()), crlf);
    CHECK(app_messages(packages, "physics", "prebuilt", main, fb::build_mode_t::DEBUG, {}).empty());

    // Checking the prebuilt package checks its interface file's modules.
    std::string broken = text;
    broken.replace(broken.find("Public Function Make(): Circle"), 30,
                   "Public Function Make(): Nothing");
    packages.write(std::format("prebuilt/lib/{}/debug/physics.fbi", fb::build_target()), broken);
    const std::expected<fb::compilation_t, std::string> checked =
        fb::compile_file(packages / "prebuilt");
    REQUIRE(checked);
    REQUIRE(checked->package);
    CHECK_FALSE(checked->package.value_or(fb::package_info_t{}).interface.empty());
    CHECK(packages.messages(*checked) ==
          std::vector<std::string>{
              std::format("prebuilt/lib/{}/debug/physics.fbi: Nothing: 'Nothing' isn't a type",
                          fb::build_target()) });

    // A missing module is in the interface file, not a folder.
    packages.write(std::format("prebuilt/lib/{}/debug/physics.fbi", fb::build_target()), text);
    CHECK(app_messages(packages, "physics", "prebuilt", "Import physics.nothing\n",
                       fb::build_mode_t::DEBUG, {}) ==
          std::vector<std::string>{ "app/main.fb: physics.nothing: there's no module "
                                    "physics.nothing: no physics/nothing.fb or folder "
                                    "physics/nothing/ in <physics>" });
    packages.remove();
}

TEST_CASE("a package's kind doesn't hang on its main file's letter case", "[driver]") {
    const packages_t packages;
    // Main.fb isn't main.fb, on a disk that ignores letter case too, so this is a library.
    packages.write("casedep/project.toml", "[project]\nname = \"casedep\"\nversion = \"0.1.0\"\n");
    packages.write("casedep/Main.fb", "Public Function M(): Int => 3\n");
    const std::string main = "Import std.core\nImport casedep\nPrint(\"{M()}\")\n";
    CHECK(app_messages(packages, "casedep", "casedep", main, fb::build_mode_t::DEBUG, {}).empty());

    // A dependency whose type is "executable" can't be one, though it has no main file yet.
    packages.write("exe/project.toml",
                   "[project]\nname = \"exe\"\nversion = \"0.1.0\"\ntype = \"executable\"\n");
    CHECK(app_messages(packages, "exe", "exe", main, fb::build_mode_t::DEBUG, {}) ==
          std::vector<std::string>{
              "app/project.toml: exe: exe is an executable, so it can't be a dependency; a "
              "package that programs use is a library, type = \"library\" in its project.toml" });

    // In a library, main.fb is a module, which names a program built from it.
    packages.write("lib/project.toml",
                   "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    packages.write("lib/main.fb", "Import std.core\nPrint(\"main\")\n");
    const std::expected<fb::compilation_t, std::string> built =
        fb::compile_file(packages / "lib/main.fb");
    REQUIRE(built);
    CHECK(built->package.value_or(fb::package_info_t{}).program == "main");
    packages.write("lib/main.fb", "Public Function Main(): Int => 1\n");
    const std::expected<fb::compilation_t, std::string> library =
        fb::compile_file(packages / "lib");
    REQUIRE(library);
    CHECK(packages.messages(*library).empty());
    CHECK(library->package.value_or(fb::package_info_t{}).library);
    packages.remove();
}
