#include "fb/log.hpp"
#include "fb/runtime.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdlib>
#include <string>

// std.log's part of the runtime, with FB_LOG set and unset by the test.

namespace {

void set_setting(const char* value) {
#ifdef _WIN32
    _putenv_s("FB_LOG", value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv("FB_LOG");
    } else {
        setenv("FB_LOG", value, 1);
    }
#endif
}

} // namespace

TEST_CASE("FB_LOG's value is what the default logger reads", "[runtime][log]") {
    set_setting("debug,net=trace");
    fb_string_t* setting = fb_log_setting();
    REQUIRE(setting != nullptr);
    CHECK(std::string(reinterpret_cast<const char*>(setting + 1),
                      static_cast<std::size_t>(setting->length)) == "debug,net=trace");
    fb_string_release(setting);

    // Windows has no empty variable: setting one to "" takes it out.
    set_setting(nullptr);
    CHECK(fb_log_setting() == nullptr);
}
