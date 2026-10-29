#include "fb/runtime.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unwind.h>
#endif

// Stack traces, from the tables that compiler/src/trace.cpp gives each binary: a marker before
// each call, which says where in the source the call is, and in which function, and so on out
// through the calls it's inlined into.

namespace {

/// A trace shows at most this many frames, the innermost.
constexpr std::size_t max_frames = 256;
/// A frame's parent when it isn't inlined, and a marker's frame when its call has no place.
constexpr std::uint32_t no_frame = 0xFFFFFFFF;

/// The binaries' tables, in the order they're added. A function's static, as a binary adds its
/// table before main, maybe before this file's statics are made.
std::vector<const fb_trace_t*>& tables() {
    static std::vector<const fb_trace_t*> instance;
    return instance;
}

std::mutex& tables_mutex() {
    static std::mutex instance;
    return instance;
}

/// A marker: where the code after it is, and its call's frame in its table.
struct marker_t {
    std::uintptr_t address;
    const fb_trace_t* table;
    std::uint32_t frame;
};

/// Every table's markers, by address, and how many tables they're from, made again once another
/// is added. Read under tables_mutex().
std::vector<marker_t>& markers() {
    static std::vector<marker_t> instance;
    return instance;
}

std::size_t& marked_tables() {
    static std::size_t instance = 0;
    return instance;
}

void mark_tables() {
    std::vector<const fb_trace_t*>& added = tables();
    if (marked_tables() == added.size()) {
        return;
    }

    std::vector<marker_t>& found = markers();
    found.clear();
    for (const fb_trace_t* table : added) {
        for (const std::int32_t* entry = table->begin; entry + 1 < table->end; entry += 2) {
            // The table's own start and end, and its padding, mark nothing.
            const auto frame = static_cast<std::uint32_t>(entry[1]);
            if (entry[0] == 0 || frame == no_frame) {
                continue;
            }

            found.push_back({
                .address = reinterpret_cast<std::uintptr_t>(entry) +
                           static_cast<std::uintptr_t>(static_cast<std::intptr_t>(entry[0])),
                .table = table,
                .frame = frame,
            });
        }
    }

    std::ranges::sort(found, {}, &marker_t::address);
    marked_tables() = added.size();
}

/// Where the function that has the code at `address` starts, as its unwind info says, or 0.
std::uintptr_t function_start(const std::uintptr_t address) {
#ifdef _WIN32
    DWORD64 image = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(address, &image, nullptr);
    return function != nullptr ? static_cast<std::uintptr_t>(image + function->BeginAddress) : 0;
#else
    (void)address;
    return 0;
#endif
}

/// The marker of the call a frame is in: the last before it, in its own function.
const marker_t* marker_of(const fb::runtime::stack_frame_t& frame) {
    // A return address is right after its call.
    const std::uintptr_t address = frame.address - 1;
    const std::uintptr_t start = frame.start != 0 ? frame.start : function_start(address);
    if (start == 0) {
        return nullptr;
    }

    const std::vector<marker_t>& found = markers();
    const auto after = std::ranges::upper_bound(found, address, {}, &marker_t::address);
    if (after == found.begin()) {
        return nullptr;
    }

    const marker_t* marker = &*std::prev(after);
    return marker->address >= start ? marker : nullptr;
}

/// Each thrown error's stack, by the error, until it goes, and how many there are, which a
/// release reads without the lock.
std::unordered_map<const fb_object_t*, std::vector<fb::runtime::stack_frame_t>>& error_traces() {
    static std::unordered_map<const fb_object_t*, std::vector<fb::runtime::stack_frame_t>> instance;
    return instance;
}

std::mutex& errors_mutex() {
    static std::mutex instance;
    return instance;
}

std::atomic<std::size_t>& traced_errors() {
    static std::atomic<std::size_t> instance = 0;
    return instance;
}

#ifndef _WIN32
_Unwind_Reason_Code add_frame(_Unwind_Context* context, void* data) {
    auto& frames = *static_cast<std::vector<fb::runtime::stack_frame_t>*>(data);
    if (frames.size() >= max_frames) {
        return _URC_END_OF_STACK;
    }

    int before = 0;
    frames.push_back({
        .address = static_cast<std::uintptr_t>(_Unwind_GetIPInfo(context, &before)),
        .start = static_cast<std::uintptr_t>(_Unwind_GetRegionStart(context)),
    });
    return _URC_NO_REASON;
}
#endif

} // namespace

namespace fb::runtime {

std::vector<stack_frame_t> capture_stack() {
    std::vector<stack_frame_t> frames;
    frames.reserve(32);
#ifdef _WIN32
    // The return addresses alone, which write_trace() finds the functions of: Windows walks
    // the stack faster than its parts would one frame at a time. It takes fewer than 63 at once.
    constexpr DWORD batch = 62;
    std::array<void*, batch> found{};
    for (DWORD skip = 0; frames.size() < max_frames; skip += batch) {
        const USHORT count = RtlCaptureStackBackTrace(skip, batch, found.data(), nullptr);
        for (USHORT i = 0; i < count; ++i) {
            frames.push_back({ .address = reinterpret_cast<std::uintptr_t>(found[i]), .start = 0 });
        }

        if (count < batch) {
            break;
        }
    }
#else
    _Unwind_Backtrace(add_frame, &frames);
#endif
    return frames;
}

void write_trace(const std::span<const stack_frame_t> frames, std::FILE* stream) {
    const std::scoped_lock held(tables_mutex());
    mark_tables();
    // The first frame is capture_stack()'s, which a marker can't be in.
    for (const stack_frame_t& frame : frames) {
        const marker_t* marker = frame.address != 0 ? marker_of(frame) : nullptr;
        if (marker == nullptr) {
            continue;
        }

        const fb_trace_t& table = *marker->table;
        for (std::uint32_t at = marker->frame; at != no_frame; at = table.frames[at].parent) {
            const fb_trace_frame_t& place = table.frames[at];
            if (place.line == 0) {
                continue;
            }

            const char* name = table.names + place.name;
            if (*name == '\0') {
                std::fprintf(stream, "    at %s:%u\n", table.names + place.file, place.line);
            } else {
                std::fprintf(stream, "    at %s (%s:%u)\n", name, table.names + place.file,
                             place.line);
            }
        }
    }
}

void keep_error_trace(const fb_object_t* error) {
    std::vector<stack_frame_t> frames = capture_stack();
    const std::scoped_lock held(errors_mutex());
    error_traces()[error] = std::move(frames);
    traced_errors().store(error_traces().size(), std::memory_order_relaxed);
}

std::vector<stack_frame_t> error_trace(const fb_object_t* error) {
    const std::scoped_lock held(errors_mutex());
    const auto found = error_traces().find(error);
    return found != error_traces().end() ? found->second : std::vector<stack_frame_t>();
}

} // namespace fb::runtime

void fb_trace_add(const fb_trace_t* table) {
    const std::scoped_lock held(tables_mutex());
    tables().push_back(table);
}

void fb_error_released(const fb_object_t* error) {
    // Most programs keep no error's trace at a time, so most releases end here.
    if (traced_errors().load(std::memory_order_relaxed) == 0) {
        return;
    }

    const std::scoped_lock held(errors_mutex());
    error_traces().erase(error);
    traced_errors().store(error_traces().size(), std::memory_order_relaxed);
}
