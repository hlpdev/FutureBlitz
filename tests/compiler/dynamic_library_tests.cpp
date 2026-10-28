#include "binary_images.hpp"
#include "fb/dynamic_library.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace {

using fb::library_problem_t;
using name_t = std::expected<std::string, library_problem_t>;

constexpr auto not_one = std::unexpected(library_problem_t::NOT_A_LIBRARY);
constexpr auto other_cpu = std::unexpected(library_problem_t::OTHER_TARGET);
constexpr auto executable = std::unexpected(library_problem_t::EXECUTABLE);

} // namespace

TEST_CASE("a shared object's name is its SONAME", "[dynamic_library]") {
    using namespace fb_test;
    const std::string library = elf_library(elf_x86_64, "libengine.so.1");
    CHECK(fb::dynamic_library_name(library, "linux-x64") == name_t("libengine.so.1"));
    CHECK(fb::dynamic_library_name(elf_library(elf_aarch64, "libengine.so"), "linux-arm64") ==
          name_t("libengine.so"));
    // One without a SONAME is recorded by its file's name.
    CHECK(fb::dynamic_library_name(elf_library(elf_x86_64, ""), "linux-x64") == name_t(""));
    CHECK(fb::dynamic_library_name(elf_library(elf_x86_64, "x", 3, 1), "linux-x64") == name_t("x"));

    // One for another CPU, of another width, or in the other byte order is another target's.
    CHECK(fb::dynamic_library_name(library, "linux-arm64") == name_t(other_cpu));
    std::string narrow = library;
    narrow[4] = 1;
    CHECK(fb::dynamic_library_name(narrow, "linux-x64") == name_t(other_cpu));
    std::string big = library;
    big[5] = 2;
    CHECK(fb::dynamic_library_name(big, "linux-x64") == name_t(other_cpu));

    // An executable, position-independent or not, is one.
    CHECK(fb::dynamic_library_name(elf_library(elf_x86_64, "x", 2), "linux-x64") ==
          name_t(executable));
    constexpr std::uint64_t pie = 0x08000000;
    CHECK(fb::dynamic_library_name(elf_library(elf_x86_64, "", 3, pie), "linux-x64") ==
          name_t(executable));

    // A relocatable object, a cut-off file, and another OS's library aren't libraries.
    CHECK(fb::dynamic_library_name(elf_library(elf_x86_64, "x", 1), "linux-x64") ==
          name_t(not_one));
    CHECK(fb::dynamic_library_name(library.substr(0, library.size() - 4), "linux-x64") ==
          name_t(not_one));
    CHECK(fb::dynamic_library_name(library.substr(0, 100), "linux-x64") == name_t(not_one));
    CHECK(fb::dynamic_library_name(library.substr(0, 3), "linux-x64") == name_t(not_one));
    CHECK(fb::dynamic_library_name(library, "macos-arm64") == name_t(not_one));
    CHECK(fb::dynamic_library_name(library, "windows-x64") == name_t(not_one));
}

TEST_CASE("a dylib's name is its install name, in a universal file its CPU's",
          "[dynamic_library]") {
    using namespace fb_test;
    const std::string arm = mach_o_library(mach_o_arm64, "@rpath/libengine.dylib");
    const std::string intel = mach_o_library(mach_o_x86_64, "@loader_path/libengine.dylib");
    CHECK(fb::dynamic_library_name(arm, "macos-arm64") == name_t("@rpath/libengine.dylib"));
    CHECK(fb::dynamic_library_name(intel, "macos-x64") == name_t("@loader_path/libengine.dylib"));
    CHECK(fb::dynamic_library_name(arm, "macos-x64") == name_t(other_cpu));

    const std::string both = universal(
        { { .cpu = mach_o_x86_64, .image = intel }, { .cpu = mach_o_arm64, .image = arm } });
    CHECK(fb::dynamic_library_name(both, "macos-arm64") == name_t("@rpath/libengine.dylib"));
    CHECK(fb::dynamic_library_name(both, "macos-x64") == name_t("@loader_path/libengine.dylib"));
    CHECK(fb::dynamic_library_name(universal({ { .cpu = mach_o_x86_64, .image = intel } }),
                                   "macos-arm64") == name_t(other_cpu));

    // An arm64e slice isn't the one an arm64 program links, wherever it is.
    const std::string signed_arm = mach_o_library(mach_o_arm64, "@rpath/libsigned.dylib");
    constexpr std::uint32_t arm64e = 0x80000002;
    CHECK(fb::dynamic_library_name(
              universal({ { .cpu = mach_o_arm64, .image = signed_arm, .subtype = arm64e },
                          { .cpu = mach_o_arm64, .image = arm } }),
              "macos-arm64") == name_t("@rpath/libengine.dylib"));
    CHECK(fb::dynamic_library_name(
              universal({ { .cpu = mach_o_arm64, .image = signed_arm, .subtype = arm64e } }),
              "macos-arm64") == name_t(other_cpu));

    // A 32-bit one, and one in the other byte order, are another target's.
    for (const std::uint32_t magic : { 0xFEEDFACEU, 0xCEFAEDFEU, 0xCFFAEDFEU }) {
        std::string other = arm;
        put(other, 0, magic, 4);
        CHECK(fb::dynamic_library_name(other, "macos-arm64") == name_t(other_cpu));
    }

    // An executable is one; a universal file with nothing in it or past its end, a cut-off one,
    // and another OS's library aren't libraries.
    CHECK(fb::dynamic_library_name(mach_o_library(mach_o_arm64, "x", 2), "macos-arm64") ==
          name_t(executable));
    CHECK(fb::dynamic_library_name(mach_o_library(mach_o_arm64, "x", 1), "macos-arm64") ==
          name_t(not_one));
    CHECK(fb::dynamic_library_name(universal({}), "macos-arm64") == name_t(not_one));
    CHECK(fb::dynamic_library_name(both.substr(0, both.size() - 1), "macos-arm64") ==
          name_t(not_one));
    CHECK(fb::dynamic_library_name(arm.substr(0, 60), "macos-arm64") == name_t(not_one));
    CHECK(fb::dynamic_library_name(arm, "linux-x64") == name_t(not_one));
}

TEST_CASE("a DLL is recorded by its file's name", "[dynamic_library]") {
    using namespace fb_test;
    CHECK(fb::dynamic_library_name(pe_library(pe_amd64), "windows-x64") == name_t(""));
    // One for Arm or for 32-bit x86 is another target's.
    CHECK(fb::dynamic_library_name(pe_library(pe_arm64), "windows-x64") == name_t(other_cpu));
    CHECK(fb::dynamic_library_name(pe_library(0x14C), "windows-x64") == name_t(other_cpu));
    // An executable is one, and a file that only starts as one does isn't a library.
    CHECK(fb::dynamic_library_name(pe_library(pe_amd64, false), "windows-x64") ==
          name_t(executable));
    CHECK(fb::dynamic_library_name("MZ", "windows-x64") == name_t(not_one));
    CHECK(fb::dynamic_library_name(pe_library(pe_amd64).substr(0, 80), "windows-x64") ==
          name_t(not_one));
    CHECK(fb::dynamic_library_name("", "windows-x64") == name_t(not_one));
}
