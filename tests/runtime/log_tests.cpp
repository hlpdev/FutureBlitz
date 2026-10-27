#include "fb/log.hpp"
#include "fb/runtime.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

// std.log's part of the runtime, with FB_LOG set and unset by the test.

namespace {

// Sets FB_LOG to UTF-8 text, or takes it out for nullptr.
void set_setting(const char* value) {
#ifdef _WIN32
    const std::string text = value == nullptr ? "" : value;
    std::wstring wide(text.size(), L'\0');
    wide.resize(static_cast<std::size_t>(
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                            static_cast<int>(wide.size()))));
    _wputenv_s(L"FB_LOG", wide.c_str());
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

    // Text past ASCII comes as UTF-8, whatever Windows's code page.
    set_setting("caf\xC3\xA9=debug");
    setting = fb_log_setting();
    REQUIRE(setting != nullptr);
    CHECK(std::string(reinterpret_cast<const char*>(setting + 1),
                      static_cast<std::size_t>(setting->length)) == "caf\xC3\xA9=debug");
    fb_string_release(setting);

    // Windows has no empty variable: setting one to "" takes it out.
    set_setting(nullptr);
    CHECK(fb_log_setting() == nullptr);
}
