#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// These run the real fb, with its output going to a pipe like in a script.

namespace {

/// Runs fb and collects its exit code and its stdout and stderr together.
fb::process_result_t fb_run(const std::vector<std::string>& arguments) {
    const std::expected<fb::process_result_t, std::string> result =
        fb::run_process(FB_CLI, arguments);
    REQUIRE(result);
    return *result;
}

/// tests/programs/<name>
std::string program(const std::string_view name) {
    return fb::to_utf8(std::filesystem::path(FB_TEST_PROGRAMS) / name);
}

/// Writes a source file into the test output folder.
std::filesystem::path write_source(const std::string_view name, const std::string_view text) {
    const std::filesystem::path path = std::filesystem::path(FB_TEST_OUTPUT) / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

void check_usage_error(const std::vector<std::string>& arguments, const std::string_view message) {
    const fb::process_result_t result = fb_run(arguments);
    INFO(result.output);
    CHECK(result.exit_code == 2);
    CHECK(result.output.contains(message));
}

} // namespace

TEST_CASE("fb --version prints the version", "[cli]") {
    const fb::process_result_t result = fb_run({ "--version" });
    CHECK(result.exit_code == 0);
    CHECK(result.output == std::format("fb {}\n", fb::build_config::version));
}

TEST_CASE("fb shows help", "[cli]") {
    const fb::process_result_t result = fb_run({});
    CHECK(result.exit_code == 0);
    CHECK(result.output.contains("Usage: fb [--color <when>] <command> [options]"));
    CHECK(result.output.contains("--show <stages>"));

    CHECK(fb_run({ "help", "build" }).output.contains("Usage: fb build"));
    CHECK(result.output.contains(
        "bindgen              Turn a C header into a FutureBlitz module, with fb-bindgen"));
    CHECK(
        fb_run({ "bindgen", "--help" }).output.contains("Usage: fb bindgen <header.h> [options]"));
    const std::string run_help = fb_run({ "run", "--help" }).output;
    CHECK(run_help.contains("Usage: fb run"));
    CHECK_FALSE(run_help.contains("--output"));
}

TEST_CASE("bad usage exits with code 2", "[cli]") {
    check_usage_error({ "frobnicate" }, "error: unknown command 'frobnicate'");
    check_usage_error({ "check", "a.fb", "b.fb" }, "unexpected argument 'b.fb'");
    check_usage_error({ "check", "a.fb", "--nope" }, "unknown option '--nope'");
    check_usage_error({ "check", "a.fb", "-o", "a.exe" }, "'-o' only works with 'fb build'");
    check_usage_error({ "check", "a.fb", "--show", "bytes" }, "unknown stage 'bytes'");
    check_usage_error({ "check", "a.fb", "--show" }, "'--show' needs a value");
    check_usage_error({ "check", "a.fb", "--show=" }, "'--show' needs stages");
    check_usage_error({ "help", "check", "extra" }, "unexpected argument 'extra'");
    check_usage_error({ "--version", "extra" }, "unexpected argument 'extra'");
    check_usage_error({ "build", "a.fb", "-o", "" }, "'-o' takes the executable's file name");
    check_usage_error({ "build", "a.fb", "-o", "out/" }, "'-o' takes the executable's file name");
    check_usage_error({ "build", "a.fb", "--", "x" }, "only 'fb run' runs");
    check_usage_error({ "check", "a.fb", "--color=sometimes" },
                      "'--color' takes auto, always, or never, not 'sometimes'");
    // A file that can't be read is bad usage too, not a compile error.
    check_usage_error({ "check", "missing.fb" }, "error: cannot read 'missing.fb'");
}

TEST_CASE("fb check reports errors with exit code 1", "[cli]") {
    const fb::process_result_t good = fb_run({ "check", program("hello.fb") });
    CHECK(good.exit_code == 0);
    // Status verbs are right-aligned in 12 columns.
    CHECK(good.output.contains("    Checking "));
    CHECK(good.output.contains("    Finished check in "));

    const std::filesystem::path source = write_source("bad.fb", "Local x: Int = \"a\"\n");
    const fb::process_result_t bad = fb_run({ "check", fb::to_utf8(source) });
    CHECK(bad.exit_code == 1);
    // Int is Int32, and 1:16 is where "a" starts.
    CHECK(bad.output.contains("bad.fb:1:16: error: expected Int32, found String\n"));
    CHECK(bad.output.contains("bad.fb has 1 error\n"));
}

TEST_CASE("fb build writes an executable that runs", "[cli]") {
    std::filesystem::path executable = std::filesystem::path(FB_TEST_OUTPUT) / "built" / "hello";
    executable += fb::executable_extension;
    std::filesystem::remove(executable);

    const fb::process_result_t built =
        fb_run({ "build", program("hello.fb"), "-o", fb::to_utf8(executable) });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    CHECK(built.output.contains("   Compiling "));
    CHECK(built.output.contains("    Finished debug build in "));

    const std::expected<fb::process_result_t, std::string> ran = fb::run_process(executable, {});
    REQUIRE(ran);
    CHECK(ran->output == "hello\n");
}

TEST_CASE("fb build puts the executable next to the source", "[cli]") {
    const std::filesystem::path source = write_source("next_to.fb", "Print(\"here\")\n");
    std::filesystem::path executable = source;
    executable.replace_extension(fb::executable_extension);
    std::filesystem::remove(executable);

    const fb::process_result_t result = fb_run({ "build", fb::to_utf8(source), "--release" });
    INFO(result.output);
    CHECK(result.exit_code == 0);
    CHECK(result.output.contains("    Finished release build in "));
    CHECK(std::filesystem::exists(executable));
}

TEST_CASE("fb build stops at compile errors", "[cli]") {
    const std::filesystem::path source = write_source("broken.fb", "Print(missing)\n");
    const fb::process_result_t result = fb_run({ "build", fb::to_utf8(source) });
    CHECK(result.exit_code == 1);
    CHECK(result.output.contains("error: couldn't build "));
    CHECK(result.output.contains("broken.fb because of 1 error\n"));
}

TEST_CASE("fb run runs the program and passes its exit code on", "[cli]") {
    const fb::process_result_t hello = fb_run({ "run", program("hello.fb") });
    CHECK(hello.exit_code == 0);
    CHECK(hello.output.contains(
        std::format("     Running hello{}\nhello\n", fb::executable_extension)));

    // The program prints "before", then overflows an Int. Panics exit with code 101.
    const fb::process_result_t panic = fb_run({ "run", program("panic_overflow.fb") });
    CHECK(panic.exit_code == 101);
    CHECK(panic.output.contains("before\npanic: integer overflow in '+' at "));
    CHECK(panic.output.contains(
        std::format("error: panic_overflow{} exited with code 101, which a panic gives\n",
                    fb::executable_extension)));
}

TEST_CASE("fb --show prints the stages it names", "[cli]") {
    const auto show = [](const std::string& stages) {
        const fb::process_result_t result =
            fb_run({ "check", program("hello.fb"), "--show", stages });
        CHECK(result.exit_code == 0);
        return result.output;
    };

    CHECK(show("tokens").contains("1:1   identifier      Print\n"));
    CHECK(show("ast").contains("\n(call Print \"hello\")\n"));
    CHECK(show("types").contains("\n(call Print \"hello\": String)\n"));
    CHECK(show("mir").contains("\nfunction main\n"));
    CHECK(show("llvm").contains("\ndefine "));
    // One stage prints without a header.
    CHECK_FALSE(show("ast").contains("──"));

    // Several stages get headers, once each and in pipeline order, however they're given.
    const std::string both =
        fb_run({ "check", program("hello.fb"), "--show", "mir", "--show=ast,mir" }).output;
    const std::size_t ast = both.find("── ast ──\n");
    const std::size_t mir = both.find("── mir ──\n");
    CHECK(ast != std::string::npos);
    CHECK(mir != std::string::npos);
    CHECK(ast < mir);
    CHECK(both.find("── mir ──", mir + 1) == std::string::npos);
}

TEST_CASE("fb --show still prints the front end when there are errors", "[cli]") {
    const std::filesystem::path source = write_source("show_errors.fb", "Local x: Int = \"a\"\n");
    const fb::process_result_t result = fb_run({ "check", fb::to_utf8(source), "--show", "all" });
    CHECK(result.exit_code == 1);
    // Tokens, the tree, and types print. MIR needs a program without errors, so it doesn't.
    CHECK(result.output.contains("── tokens ──\n"));
    CHECK(result.output.contains("(local x: Int = \"a\")\n"));
    CHECK(result.output.contains("(local x: Int = \"a\": String)\n"));
    CHECK_FALSE(result.output.contains("── mir ──"));
}

TEST_CASE("fb --color turns ANSI colors on and off", "[cli]") {
    const std::vector<std::string> check_hello = { "check", program("hello.fb"), "--show", "ast" };
    std::vector<std::string> always = check_hello;
    always.emplace_back("--color=always");
    const std::string colored = fb_run(always).output;
    // A bold green status verb, then a magenta head and a cyan name in the tree.
    CHECK(colored.contains("\x1b[1;32m    Checking\x1b[0m "));
    CHECK(colored.contains("(\x1b[35mcall\x1b[0m \x1b[36mPrint\x1b[0m"));

    std::vector<std::string> never = check_hello;
    never.insert(never.end(), { "--color", "never" });
    CHECK_FALSE(fb_run(never).output.contains('\x1b'));

    // A pipe isn't a terminal, so the default is no color here.
    CHECK_FALSE(fb_run(check_hello).output.contains('\x1b'));
}

TEST_CASE("fb runs and builds a package from its folder", "[cli]") {
    // A copy, so building doesn't write into the source tree.
    const std::filesystem::path packages = std::filesystem::path(FB_TEST_OUTPUT) / "packages";
    std::filesystem::remove_all(packages);
    std::filesystem::create_directories(packages);
    for (const std::string_view name : { "game", "physics" }) {
        std::filesystem::copy(std::filesystem::path(FB_TEST_PACKAGES) / name, packages / name,
                              std::filesystem::copy_options::recursive);
    }

    const std::string game = fb::to_utf8(packages / "game");
    const fb::process_result_t ran = fb_run({ "run", game });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("   Compiling game v0.1.0 ("));
    CHECK(ran.output.contains("     Running game"));
    CHECK(ran.output.contains("at 3, 1.5\narea 12\n2\n"));

    // fb build writes it to the package's build/ folder, named after the package.
    std::filesystem::path executable = packages / "game" / "build" / "game";
    executable += fb::executable_extension;
    const fb::process_result_t built = fb_run({ "build", game });
    INFO(built.output);
    CHECK(built.exit_code == 0);
    CHECK(std::filesystem::exists(executable));

    // A library is checked, but has nothing to run.
    const std::string physics = fb::to_utf8(packages / "physics");
    const fb::process_result_t checked = fb_run({ "check", physics });
    CHECK(checked.exit_code == 0);
    CHECK(checked.output.contains("    Checking physics v0.2.0 ("));
    // Its views are of its own files, not the placeholder a library's check has for a main file.
    const fb::process_result_t shown =
        fb_run({ "check", physics, "--show", "tokens,ast,types", "--color", "never" });
    CHECK(shown.exit_code == 0);
    CHECK(shown.output.contains("vector.fb\n"));
    CHECK(shown.output.contains("(struct Public Vec"));
    CHECK(shown.output.contains("(class Public Body"));
    CHECK_FALSE(shown.output.contains("(library)"));
    check_usage_error({ "check", physics, "--show", "mir" },
                      "physics is a library, with no program of its own to show as MIR or LLVM IR");
    const fb::process_result_t library = fb_run({ "run", physics });
    CHECK(library.exit_code == 2);
    CHECK(library.output.contains("error: physics is a library, with no main file to run"));

    // A folder that isn't a package's is bad usage.
    check_usage_error({ "check", fb::to_utf8(packages) }, "there's no project.toml in '");
}

TEST_CASE("fb runs a package that links a C library", "[cli]") {
    const std::filesystem::path packages =
        std::filesystem::path(FB_TEST_OUTPUT) / "native_packages";
    std::filesystem::remove_all(packages);
    std::filesystem::create_directories(packages);
    for (const std::string_view name : { "native_calls", "vmath" }) {
        std::filesystem::copy(std::filesystem::path(FB_TEST_PACKAGES) / name, packages / name,
                              std::filesystem::copy_options::recursive);
    }

    // The vmath package links tests/native's vecmath from its libs/ folder.
    const std::filesystem::path archive(FB_TEST_VECMATH);
    std::filesystem::create_directories(packages / "vmath" / "libs");
    std::filesystem::copy_file(archive, packages / "vmath" / "libs" / archive.filename());
    const fb::process_result_t ran = fb_run({ "run", fb::to_utf8(packages / "native_calls") });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("5 4000000005 6 2.5\nTrue False 2 0\n"));

    // Without it, project.toml's line says so.
    std::filesystem::remove_all(packages / "vmath" / "libs");
    const fb::process_result_t missing = fb_run({ "check", fb::to_utf8(packages / "vmath") });
    CHECK(missing.exit_code == 1);
    CHECK(missing.output.contains(
        "project.toml:6:11: error: \"libs/vecmath\" names no static library: there's no "
        "libs/vecmath, libs/libvecmath.a, libs/vecmath.a, or libs/vecmath.lib, from the package's "
        "folder\n"));
}

TEST_CASE("fb bindgen runs fb-bindgen, which writes a C header's module", "[cli]") {
#ifdef FB_TEST_BINDGEN
    const std::filesystem::path header =
        write_source("bindgen/shapes.h", "int area(int width, int height);\n");
    const std::filesystem::path output = header.parent_path() / "shapes.fb";
    std::filesystem::remove(output);
    const fb::process_result_t made =
        fb_run({ "bindgen", fb::to_utf8(header), "-o", fb::to_utf8(output), "--lib", "shapes" });
    INFO(made.output);
    CHECK(made.exit_code == 0);
    std::ifstream file(output, std::ios::binary);
    const std::string text{ std::istreambuf_iterator<char>(file),
                            std::istreambuf_iterator<char>() };
    CHECK(text.ends_with("Public Extern \"C\" Lib \"shapes\"\n"
                         "    Function area(width: Int32, height: Int32): Int32\n"
                         "End Extern\n"));

    // What goes wrong is fb-bindgen's to say, an error a line, as `fb bindgen`.
    const std::filesystem::path broken =
        write_source("bindgen/broken.h", "int broken(int x;\nint also(;\n");
    const fb::process_result_t failed =
        fb_run({ "bindgen", fb::to_utf8(broken), "-o", fb::to_utf8(output) });
    CHECK(failed.exit_code == 1);
    CHECK(failed.output.contains("broken.h:1:17: expected ')'\nerror: "));
    CHECK(failed.output.contains("broken.h:2:10: expected parameter declarator\n"));
    check_usage_error({ "bindgen" }, "error: fb bindgen takes a C header, like 'fb bindgen "
                                     "vecmath.h'");
    check_usage_error({ "bindgen", "a.h", "--nope" }, "error: unknown option '--nope'");
    check_usage_error({ "bindgen", "a.h", "--nope" },
                      "Run 'fb bindgen --help' to see the options.");

    // fb's own --color is fb's, and isn't passed on.
    const fb::process_result_t colored = fb_run({
        "bindgen",
        "--color",
        "never",
        fb::to_utf8(header),
        "--color=always",
        "-o",
        fb::to_utf8(output),
    });
    INFO(colored.output);
    CHECK(colored.exit_code == 0);
    check_usage_error({ "bindgen", "a.h", "--color" }, "error: '--color' needs a value");
    check_usage_error({ "bindgen", "a.h", "--color", "pink" },
                      "error: '--color' takes auto, always, or never, not 'pink'");

    // Empty values say nothing, -o goes apart from its value, -o can name a new folder, and the
    // library's name is one project.toml can have.
    check_usage_error({ "bindgen", "a.h", "-D", "" }, "'-D' needs a macro's name");
    check_usage_error({ "bindgen", "a.h", "--target", "" }, "'--target' needs a target");
    check_usage_error({ "bindgen", "a.h", "--std=" }, "'--std' needs a C standard");
    check_usage_error({ "bindgen", "-optimize", "a.h" }, "unknown option '-optimize'");
    const std::filesystem::path nested = header.parent_path() / "new" / "folder" / "shapes.fb";
    std::filesystem::remove_all(header.parent_path() / "new");
    CHECK(fb_run({ "bindgen", fb::to_utf8(header), "-o", fb::to_utf8(nested) }).exit_code == 0);
    CHECK(std::filesystem::exists(nested));
    const std::filesystem::path unnamed = write_source("bindgen/b\xC3\xA4r.h", "int f(int);\n");
    check_usage_error({ "bindgen", fb::to_utf8(unnamed) },
                      "isn't a name a [native] library can have");
#else
    SKIP("fb-bindgen is built only where CMake finds libclang");
#endif
}

TEST_CASE("fb build reports a C symbol that no library defines at its Extern Function", "[cli]") {
    const std::filesystem::path source =
        write_source("missing_c.fb", "Extern \"C\"\n"
                                     "    Function no_such_c_symbol(): Int32\n"
                                     "End Extern\n"
                                     "Print(\"{no_such_c_symbol()}\")\n");
    std::filesystem::path executable = std::filesystem::path(FB_TEST_OUTPUT) / "missing_c";
    executable += fb::executable_extension;
    for (const std::string_view command : { "build", "run" }) {
        std::vector<std::string> arguments{ std::string(command), fb::to_utf8(source) };
        if (command == "build") {
            arguments.insert(arguments.end(), { "-o", fb::to_utf8(executable) });
        }

        const fb::process_result_t built = fb_run(arguments);
        INFO(built.output);
        CHECK(built.exit_code == 1);
        CHECK(built.output.contains("missing_c.fb:2:14: error: the C symbol 'no_such_c_symbol' "
                                    "isn't in the C runtime, or any library the program links\n"));
        CHECK(built.output.contains("missing_c.fb because of 1 error\n"));
        CHECK(built.output.contains("error: couldn't link "));
        CHECK_FALSE(built.output.contains("ld.lld"));
    }
}

TEST_CASE("fb's help, --color, and -- go where people put them", "[cli]") {
    // "fb help help" is fb's own help, "fb --help check" check's, and --color goes before the
    // command too.
    CHECK(fb_run({ "help", "help" }).output.contains("Commands:"));
    CHECK(fb_run({ "--help", "check" }).output.contains("Usage: fb check"));
    const fb::process_result_t colored =
        fb_run({ "--color", "never", "check", program("hello.fb") });
    CHECK(colored.exit_code == 0);
    CHECK_FALSE(colored.output.contains('\x1b'));

    // What follows -- goes to the program.
    const fb::process_result_t ran = fb_run({ "run", program("hello.fb"), "--", "a", "--b" });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("hello\n"));
}

TEST_CASE("fb without a file or folder uses the package the current folder is in", "[cli]") {
    const std::filesystem::path root = std::filesystem::path(FB_TEST_OUTPUT) / "no_input";
    std::filesystem::remove_all(root);
    write_source("no_input/app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n");
    write_source("no_input/app/main.fb", "Print(\"from app\")\n");
    std::filesystem::create_directories(root / "app" / "ui" / "deep");
    std::filesystem::create_directories(root / "alone");
    const std::filesystem::path before = std::filesystem::current_path();

    // From a folder in the package, however deep, it's the package.
    std::filesystem::current_path(root / "app" / "ui" / "deep");
    const fb::process_result_t ran = fb_run({ "run" });
    // Outside every package, it's bad usage.
    std::filesystem::current_path(root / "alone");
    const fb::process_result_t alone = fb_run({ "check" });
    std::filesystem::current_path(before);

    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("from app\n"));
    INFO(alone.output);
    CHECK(alone.exit_code == 2);
    CHECK(alone.output.contains("there's no project.toml in this folder or above it"));
}

TEST_CASE("fb build's -o names a new file, not a folder or the program's own", "[cli]") {
    const std::filesystem::path source = write_source("output/hi.fb", "Print(\"hi\")\n");
    const std::filesystem::path folder = source.parent_path() / "made";
    std::filesystem::create_directories(folder);
    check_usage_error({ "build", fb::to_utf8(source), "-o", fb::to_utf8(folder) },
                      "'-o' names a folder");
    check_usage_error({ "build", fb::to_utf8(source), "-o", fb::to_utf8(source) },
                      "a file of the program; give another path");
    std::ifstream kept(source, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(kept), {}) == "Print(\"hi\")\n");

    // A path that isn't ASCII, without an extension, gets the one the target's executables have.
    const std::filesystem::path wide = source.parent_path() / fb::path_from_utf8("日本") / "out";
    std::filesystem::path written = wide;
    written += fb::executable_extension;
    std::filesystem::remove(written);
    const fb::process_result_t built =
        fb_run({ "build", fb::to_utf8(source), "-o", fb::to_utf8(wide) });
    INFO(built.output);
    CHECK(built.exit_code == 0);
    CHECK(std::filesystem::exists(written));
}

TEST_CASE("fb builds where the temporary folder's name isn't in the ANSI code page", "[cli]") {
#ifdef _WIN32
    // The C++ driver that links reads TMP and TEMP in the ANSI code page, which has no snowman.
    const std::filesystem::path temporary =
        std::filesystem::path(FB_TEST_OUTPUT) / fb::path_from_utf8("tmp \xE2\x98\x83 x");
    std::filesystem::create_directories(temporary);
    std::wstring old_tmp;
    std::wstring old_temp;
    if (const wchar_t* const value = _wgetenv(L"TMP")) {
        old_tmp = value;
    }

    if (const wchar_t* const value = _wgetenv(L"TEMP")) {
        old_temp = value;
    }

    _wputenv_s(L"TMP", temporary.wstring().c_str());
    _wputenv_s(L"TEMP", temporary.wstring().c_str());
    const fb::process_result_t ran = fb_run({ "run", program("hello.fb") });
    _wputenv_s(L"TMP", old_tmp.c_str());
    _wputenv_s(L"TEMP", old_temp.c_str());
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("hello\n"));
#else
    SKIP("only Windows reads TMP in a code page");
#endif
}

TEST_CASE("fb shows the first hundred diagnostics of a file with many", "[cli]") {
    std::string text;
    for (int i = 0; i < 150; ++i) {
        text += std::format("Local x{}: Int = \"a\"\n", i);
    }

    const std::filesystem::path source = write_source("many_errors.fb", text);
    const fb::process_result_t checked =
        fb_run({ "check", fb::to_utf8(source), "--color", "never" });
    CHECK(checked.exit_code == 1);
    CHECK(checked.output.contains("x99: Int"));
    CHECK_FALSE(checked.output.contains("x100: Int"));
    CHECK(checked.output.contains("note: 50 more diagnostics aren't shown\n"));
    CHECK(checked.output.contains("many_errors.fb has 150 errors\n"));
}

TEST_CASE("fb takes a .fb file's extension in any letter case", "[cli]") {
    const std::filesystem::path source = write_source("SHOUT.FB", "Print(\"loud\")\n");
    const fb::process_result_t checked = fb_run({ "check", fb::to_utf8(source) });
    INFO(checked.output);
    CHECK(checked.exit_code == 0);
}
