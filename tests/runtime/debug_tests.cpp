#include "fb/debug.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("no debugger is attached to the tests, so a breakpoint goes on", "[runtime][debug]") {
    CHECK(fb_debug_attached() == 0);
    // Without a debugger, the trap would end the process: it's left out.
    fb_debug_break();
    fb_debug_break();
    CHECK(fb_debug_attached() == 0);
}
