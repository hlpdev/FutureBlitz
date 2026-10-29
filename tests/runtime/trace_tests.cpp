#include "fb/runtime.hpp"
#include "internal.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>

// The runtime's stack walks for traces: one walk, up to a limit it says it passed, what a thrown
// error keeps of it, and a table's markers, which name the frames.

namespace {

/// A table of one marker, as a binary's has before each call: where the code is, from the
/// entry, and its frame, which marked() fills in for its call of traced().
std::array<std::int32_t, 2> marker{};
constexpr std::array<fb_trace_frame_t, 1> marked_frames{
    {
        { .name = 0, .file = 7, .line = 12, .parent = 0xFFFFFFFF },
    },
};
/// The names, each ending in a NUL.
constexpr std::array<char, 17> marked_names{ "Marked\0marked.fb" };
const fb_trace_t marked_table{
    .begin = marker.data(),
    .end = marker.data() + marker.size(),
    .frames = marked_frames.data(),
    .names = marked_names.data(),
};

/// What the stack's trace says, and its frames, from the call in marked().
struct traced_t {
    std::string trace;
    std::string frames;
};

[[gnu::noinline]] traced_t traced() {
    // The marker is before the return address, in the call, as a binary's is.
    const auto call = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - 1;
    static const bool added = [call] {
        marker[0] = static_cast<std::int32_t>(
            static_cast<std::intptr_t>(call - reinterpret_cast<std::uintptr_t>(marker.data())));
        fb_trace_add(&marked_table);
        return true;
    }();
    (void)added;
    const fb::runtime::captured_stack_t stack = fb::runtime::capture_stack();
    traced_t made;
    for (const fb::runtime::stack_frame_t& frame : stack.frames) {
        made.frames += std::format("{:#x} from {:#x}\n", frame.address, frame.start);
    }

    made.frames += std::format("the marker {:#x}\n", call);
    std::FILE* file = std::tmpfile();
    REQUIRE(file != nullptr);
    fb::runtime::write_trace(stack, file);
    REQUIRE(std::fseek(file, 0, SEEK_SET) == 0);
    std::array<char, 256> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        made.trace += line.data();
    }

    std::fclose(file);
    return made;
}

[[gnu::noinline]] traced_t marked() {
    traced_t made = traced();
    // Something after the call, so it isn't a tail call that leaves no frame.
    made.trace.shrink_to_fit();
    return made;
}

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

TEST_CASE("a trace names the frame a table's marker is in", "[runtime][trace]") {
    // The stack walk gives each frame's function's start, which the marker is after.
    const traced_t made = marked();
    INFO(made.frames);
    CHECK(made.trace == "    at Marked (marked.fb:12)\n");
}

TEST_CASE("a thrown error keeps the frames of the program's code alone", "[runtime][trace]") {
    // None of these frames is in a table, so none of them is the program's.
    const fb_object_t error{};
    fb::runtime::keep_error_trace(&error);
    CHECK(fb::runtime::error_trace(&error).frames.empty());
    fb_error_released(&error);
}
