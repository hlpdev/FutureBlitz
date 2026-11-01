#include "fb/files.hpp"
#include "fb/runtime.hpp"
#include "files_internal.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>

namespace {

/// The text of a String the runtime made, which is released.
std::string take(fb_string_t* string) {
    // The bytes follow the header.
    std::string text(reinterpret_cast<const char*>(string + 1),
                     static_cast<std::size_t>(string->length));
    fb_string_release(string);
    return text;
}

} // namespace

TEST_CASE("a recursive FileWatcher past inotify's watches says which setting says how many",
          "[runtime]") {
    CHECK(fb_io_error_kind(fb::runtime::too_many_watches) == 0);
    CHECK(take(fb_io_error_message(fb::runtime::too_many_watches)) ==
          "it holds more folders than inotify watches, as fs.inotify.max_user_watches says");
    // One that isn't recursive says so of inotify alone.
    CHECK(fb_io_error_kind(fb::runtime::no_watches_left) == 0);
    CHECK(take(fb_io_error_message(fb::runtime::no_watches_left)) ==
          "inotify watches as many as fs.inotify.max_user_watches lets it");
}

#ifdef _WIN32
TEST_CASE("a path Windows' calls take only with their prefix goes to them so, however it's spelled",
          "[runtime]") {
    using fb::runtime::os_path;
    const std::string name(300, 'a');
    // A short one as it is, and a long one after "\\?\", with '\'s.
    CHECK(os_path(R"(C:\saves\one.dat)") == R"(C:\saves\one.dat)");
    CHECK(os_path("C:/" + name + "/x") == R"(\\?\C:\)" + name + R"(\x)");
    CHECK(os_path("save.dat") == "save.dat");
    // One the program writes after "\\?\" itself keeps it, a drive's, a share's, or a volume's with
    // '\'s, while "//?/" goes by Windows' rules, a short one as it is and a long one as its full
    // path, with its ".", "..", and trailing dot gone.
    CHECK(os_path(R"(\\?\C:/x/y)") == R"(\\?\C:\x\y)");
    CHECK(os_path(R"(\\?\UNC\server/share/x)") == R"(\\?\UNC\server\share\x)");
    CHECK(os_path(R"(\\?\Unc\server/share/x)") == R"(\\?\Unc\server\share\x)");
    CHECK(os_path(R"(\\?\Volume{1234}/a/b)") == R"(\\?\Volume{1234}\a\b)");
    CHECK(os_path("//?/C:/a/../a/x.txt") == "//?/C:/a/../a/x.txt");
    CHECK(os_path("//?/C:/" + name + "/../y/./z.") == R"(\\?\C:\y\z)");
    CHECK(os_path("//?/C:/x/" + name + "/y") == R"(\\?\C:\x\)" + name + R"(\y)");
    // An NT path, and a device's, as they are, but a long one of a drive's through a device as
    // the drive's.
    CHECK(os_path(R"(\??\C:\x)") == R"(\??\C:\x)");
    CHECK(os_path(R"(\\.\pipe\name)") == R"(\\.\pipe\name)");
    CHECK(os_path("//./C:/" + name) == R"(\\?\C:\)" + name);
    // A share's after "\\?\UNC\".
    CHECK(os_path(R"(\\server\share\)" + name) == R"(\\?\UNC\server\share\)" + name);
    // One long as it's written, but short once its ".."s go, as that.
    std::string dots = "C:";
    for (int i = 0; i < 70; ++i) {
        dots += R"(\aaaa\..)";
    }

    CHECK(os_path(dots + R"(\x)") == R"(C:\x)");
    // What the OS gives back for one, without what os_path() put before it.
    CHECK(fb::runtime::plain_path(R"(\\?\C:\x)") == R"(C:\x)");
    CHECK(fb::runtime::plain_path(R"(\\?\UNC\server\share\x)") == R"(\\server\share\x)");
}
#endif
