#include "fb/build_config.hpp"
#include "fb/link.hpp"
#include "fb/toolchain.hpp"
#include "fb/utf8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string_view>
#include <vector>

namespace {

/// An empty file at `path`, with its folders.
void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).flush();
}

} // namespace

TEST_CASE("fb finds a release's toolchain beside it, and else its build folder's", "[toolchain]") {
    const std::filesystem::path root = std::filesystem::path(FB_TEST_OUTPUT) / "toolchain";
    std::filesystem::remove_all(root);
#ifdef _WIN32
    const std::filesystem::path linker = root / "bin/lld.exe";
#else
    const std::filesystem::path linker = root / "bin/lld";
#endif
    const std::filesystem::path fb = root / "bin/fb";
    const std::filesystem::path libraries =
        root / "lib" / fb::path_from_utf8(fb::build_config::toolchain_target);

    // Each part missing, then all there.
    const fb::toolchain_t build = fb::find_toolchain(fb);
    CHECK(build.libraries == fb::path_from_utf8(fb::build_config::library_directory));
    CHECK(build.std == fb::path_from_utf8(fb::build_config::std_directory));
    touch(linker);
    CHECK(fb::find_toolchain(fb).libraries == build.libraries);
    std::filesystem::create_directories(libraries);
    CHECK(fb::find_toolchain(fb).libraries == build.libraries);
    std::filesystem::create_directories(root / "std");

    const fb::toolchain_t release = fb::find_toolchain(fb);
    CHECK(release.linker == linker);
    CHECK(release.libraries == libraries);
    CHECK(release.notices == root / "notices");
    CHECK(release.std == root / "std");
    CHECK(fb::find_toolchain({}).libraries == build.libraries);

    // fb-bindgen's headers: clang's in lib/clang/, and the target's C headers in its folder's
    // include/, where it has them, as a release for macOS hasn't.
    CHECK(release.clang_headers == root / "lib" / "clang");
    CHECK(release.c_headers.empty());
    std::filesystem::create_directories(libraries / "include");
    CHECK(fb::find_toolchain(fb).c_headers == std::vector{ libraries / "include" });

    // The build folder's are libclang's and the C compiler's own.
    CHECK(build.clang_headers == fb::path_from_utf8(fb::build_config::clang_resource_directory));
    CHECK(build.c_headers.size() == fb::build_config::c_header_directories.size());
}

TEST_CASE("the build folder's toolchain has every library the runtime and std link",
          "[toolchain]") {
    const fb::toolchain_t& toolchain = fb::toolchain();
    CHECK(std::filesystem::is_regular_file(toolchain.linker));
    CHECK(std::filesystem::is_regular_file(toolchain.libraries /
                                           fb::path_from_utf8(fb::build_config::runtime_library)));
    for (const fb::build_config::bundled_library_t& library : fb::build_config::bundled_libraries) {
        INFO(library.name);
        if (!library.runtime.empty()) {
            CHECK(std::filesystem::is_regular_file(toolchain.libraries /
                                                   fb::path_from_utf8(library.runtime)));
        }

        for (const auto part : std::views::split(library.archives, ';')) {
            CHECK(std::filesystem::is_regular_file(
                toolchain.libraries /
                fb::path_from_utf8(std::string_view(part.begin(), part.end()))));
        }

        if (!library.notice.empty()) {
            CHECK(std::filesystem::is_regular_file(toolchain.notices /
                                                   fb::path_from_utf8(library.notice)));
        }
    }
}

TEST_CASE("a Linux system library is found by its development link, or else the file it loads by",
          "[toolchain]") {
    const std::filesystem::path root = std::filesystem::path(FB_TEST_OUTPUT) / "system_libraries";
    std::filesystem::remove_all(root);
    const std::vector<std::filesystem::path> folders{ root / "own", root / "system" };
    touch(root / "own/libc.so");
    touch(root / "own/libpthread.a");
    touch(root / "system/libz.so");
    touch(root / "system/libGL.so.1.7.0");
    touch(root / "system/libGL.so.1");
    touch(root / "system/libvulkan.so.1");
    touch(root / "system/libvulkan.so.2");
    std::filesystem::create_directories(root / "system/libasound.so.2");

    CHECK(fb::system_library_argument("c", folders) == "-lc");
    CHECK(fb::system_library_argument("pthread", folders) == "-lpthread");
    CHECK(fb::system_library_argument("z", folders) == "-lz");
    CHECK(fb::system_library_argument("GL", folders) == "-l:libGL.so.1");
    CHECK(fb::system_library_argument("vulkan", folders) == "-l:libvulkan.so.2");
    CHECK(fb::system_library_argument("asound", folders) == "-lasound");
    // A name that's nowhere goes as -l, so lld says it can't find it.
    CHECK(fb::system_library_argument("missing", folders) == "-lmissing");

    // The earlier folder's library wins, a development link before any loaded file's.
    touch(root / "own/libGL.so.2");
    CHECK(fb::system_library_argument("GL", folders) == "-l:libGL.so.2");
    touch(root / "system/libGL.so");
    CHECK(fb::system_library_argument("GL", folders) == "-lGL");
}
