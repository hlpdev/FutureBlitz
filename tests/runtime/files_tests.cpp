#include "fb/files.hpp"
#include "fb/runtime.hpp"
#include "files_internal.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

/// The text of a String the runtime made, which is released.
std::string take(fb_string_t* string) {
    // The bytes follow the header.
    std::string text(reinterpret_cast<const char*>(string + 1),
                     static_cast<std::size_t>(string->length));
    fb_string_release(string);
    return text;
}

/// Sets a file's access time to now and leaves its write time and what's in it, as a read of it
/// does where the OS keeps access times, whether or not this machine does.
void set_access_time(const std::filesystem::path& file) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    CHECK(SetFileTime(handle, nullptr, &now, nullptr) != 0);
    CloseHandle(handle);
#else
    // With its write time given too, as it is, since Linux tells of a file whose times are both
    // set, and of one whose access time alone is as of a read, which libuv doesn't ask for.
    struct stat status{};
    REQUIRE(stat(file.c_str(), &status) == 0);
#ifdef __APPLE__
    const timespec written = status.st_mtimespec;
#else
    const timespec written = status.st_mtim;
#endif
    const std::array<timespec, 2> times{ { { .tv_sec = 0, .tv_nsec = UTIME_NOW }, written } };
    REQUIRE(utimensat(AT_FDCWD, file.c_str(), times.data(), 0) == 0);
#endif
}

/// A watcher of `path`, which has started.
void* watcher_of(const std::filesystem::path& path, const bool recursive) {
    const std::string text = path.generic_string();
    fb_string_t* name = fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
    void* watcher = fb_io_watcher_new(name, recursive ? 1 : 0);
    fb_string_release(name);
    REQUIRE(fb_io_watcher_error(watcher) == 0);
    return watcher;
}

/// Takes a watcher's events, waiting for each, until the one of `kind` for the file named
/// `last`, and gives how many of them were for the file named `counted`.
int taken_until(void* watcher, const std::int32_t kind, const std::string_view last,
                const std::string_view counted) {
    int count = 0;
    while (fb_io_watcher_next(watcher, 1) != 0) {
        const std::string name =
            std::filesystem::path(take(fb_io_watcher_path(watcher))).filename().generic_string();
        if (name == counted) {
            ++count;
        }

        if (name == last && fb_io_watcher_kind(watcher) == kind) {
            break;
        }
    }

    return count;
}

} // namespace

TEST_CASE("a FileWatcher tells of a write, and not of a read or a new access time", "[runtime]") {
    // Windows tells of a file whose access time changed, which a read changes where it keeps
    // them, as it tells of a write, and Linux of its times set. Nothing here waits on a clock:
    // the OS gives a folder's events in order, so a file made after the reads is a fence, which
    // nothing about the file that was read comes before; and then a write of it does come.
    // The folder is this process's own, as copies of this test run at once.
#ifdef _WIN32
    const auto own = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto own = static_cast<unsigned long>(getpid());
#endif
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / std::format("fb-watch-access-{}", own);
    for (const bool recursive : { false, true }) {
        CAPTURE(recursive);
        std::filesystem::remove_all(root);
        // A recursive one's file is in a folder of its folder, which Linux watches apart.
        const std::filesystem::path folder = recursive ? root / "inner" : root;
        std::filesystem::create_directories(folder);
        const std::filesystem::path file = folder / "read.txt";
        std::ofstream(file, std::ios::binary) << "as it was";
        // Written long ago, so it isn't a file whose write time is too new to tell writes by.
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() -
                                                   std::chrono::hours(2));
        void* watcher = watcher_of(root, recursive);

        for (int i = 0; i < 50; ++i) {
            std::ifstream in(file, std::ios::binary);
            const std::string text{ std::istreambuf_iterator<char>(in), {} };
            CHECK(text == "as it was");
            in.close();
            set_access_time(file);
        }

        std::ofstream(folder / "fence.txt", std::ios::binary) << "made";
        CHECK(taken_until(watcher, 0, "fence.txt", "read.txt") == 0);

        // A write is told of, once at least, as a change.
        std::ofstream(file, std::ios::app) << ", and more";
        CHECK(taken_until(watcher, 1, "read.txt", "read.txt") >= 1);
        fb_io_watcher_close(watcher);
        fb_io_watcher_free(watcher);
    }

    // A file's own watcher has no other file for a fence, so the file's going is one: it's the
    // only event.
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path file = root / "alone.txt";
    std::ofstream(file, std::ios::binary) << "as it was";
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() -
                                               std::chrono::hours(2));
    void* watcher = watcher_of(file, false);
    for (int i = 0; i < 50; ++i) {
        set_access_time(file);
    }

    std::filesystem::remove(file);
    CHECK(taken_until(watcher, 2, "alone.txt", "alone.txt") == 1);
    fb_io_watcher_close(watcher);
    fb_io_watcher_free(watcher);
    std::filesystem::remove_all(root);
}

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
