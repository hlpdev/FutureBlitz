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
