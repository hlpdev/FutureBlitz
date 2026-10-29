#include "fb/runtime.hpp"
#include "internal.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>

// The runtime's stack walks for traces: one walk, up to a limit it says it passed, and what a
// thrown error keeps of it.

namespace {

/// The stack `depth` calls further in, each of which a frame of its own.
[[gnu::noinline]] fb::runtime::captured_stack_t deeper(const int depth) {
    if (depth == 0) {
        return fb::runtime::capture_stack();
    }

    fb::runtime::captured_stack_t stack = deeper(depth - 1);
    // Something after the call, so it isn't a tail call that leaves no frame.
    stack.frames.shrink_to_fit();
    return stack;
}

} // namespace

TEST_CASE("a stack is walked up to the same limit everywhere, which it says it went past",
          "[runtime][trace]") {
    const fb::runtime::captured_stack_t shallow = deeper(3);
    CHECK_FALSE(shallow.cut);
    CHECK(shallow.frames.size() > 3);
    CHECK(shallow.frames.size() < 512);

    const fb::runtime::captured_stack_t deep = deeper(600);
    CHECK(deep.cut);
    CHECK(deep.frames.size() == 512);
}

TEST_CASE("a thrown error keeps the frames of the program's code alone", "[runtime][trace]") {
    // These tests have no binary's trace table, so none of the frames is the program's.
    const fb_object_t error{};
    fb::runtime::keep_error_trace(&error);
    CHECK(fb::runtime::error_trace(&error).frames.empty());
    fb_error_released(&error);
}
