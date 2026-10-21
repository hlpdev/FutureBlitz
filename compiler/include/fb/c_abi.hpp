#pragma once

#include "fb/types.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace fb {

/// What one scalar in C's memory is, for how the C ABI passes what holds it.
enum class c_scalar_t : std::uint8_t {
    /// An integer, a Bool, an Enum, a CString, or a Ptr.
    INTEGER,
    FLOAT,
    DOUBLE,
};

/// One scalar of a value, where C lays it out among the value's bytes.
struct c_field_t {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    c_scalar_t kind = c_scalar_t::INTEGER;
};

/// A value of a C type as C lays it out, on a 64-bit target: its size, its alignment, and its
/// scalars in order, a fixed array's and a Struct's included. It's LLVM's natural layout of the
/// type too, where a Bool is a byte.
struct c_layout_t {
    std::uint64_t size = 0;
    std::uint64_t align = 1;
    std::vector<c_field_t> fields;
};

/// The layout of `type`, which is C data: a number, Bool, Enum, CString, Ptr, or a fixed array
/// or Struct of these.
[[nodiscard]] c_layout_t c_layout(const type_table_t& types, type_id_t type);

/// The C ABI a target passes Structs by.
enum class c_abi_t : std::uint8_t {
    /// x86-64 Windows, MSVC's and MinGW's.
    WIN64,
    /// x86-64 everywhere else: Linux, macOS, and the BSDs.
    SYSV,
    /// One whose Structs aren't supported yet, like AArch64's.
    OTHER,
};

/// The C ABI of a target, by its triple, like "x86_64-w64-windows-gnu".
[[nodiscard]] c_abi_t c_abi_of(std::string_view triple);

/// What one part of a Struct goes in: an integer register, or an SSE register with a float, a
/// double, or two floats.
enum class c_part_t : std::uint8_t {
    INTEGER,
    FLOAT,
    DOUBLE,
    TWO_FLOATS,
};

/// One part, and for an INTEGER, how many of its bytes it has, 1 to 8.
struct c_piece_t {
    c_part_t part = c_part_t::INTEGER;
    std::uint64_t size = 8;

    bool operator==(const c_piece_t&) const = default;
};

/// How a Struct goes to or from C.
enum class c_pass_t : std::uint8_t {
    /// In its pieces, each as a register would have it.
    DIRECT,
    /// As a pointer to a copy the caller makes, as Win64 passes one that isn't 1, 2, 4, or 8
    /// bytes.
    INDIRECT,
    /// In memory: an argument copied onto the stack, as LLVM's byval does, or a result through a
    /// pointer the caller passes first, as its sret does.
    MEMORY,
};

struct c_passing_t {
    c_pass_t pass = c_pass_t::DIRECT;
    /// For DIRECT, in order.
    std::vector<c_piece_t> pieces;

    bool operator==(const c_passing_t&) const = default;
};

/// The registers left for a call's arguments: SysV's six integer and eight SSE registers.
struct c_registers_t {
    unsigned integers = 6;
    unsigned sse = 8;
};

/// How a Struct of `layout` goes, by the ABI `abi`: as an argument, taking the registers its
/// pieces need from `registers`, or as a call's result when `result`. Every Struct goes in
/// memory when the ABI is OTHER.
[[nodiscard]] c_passing_t classify(const c_layout_t& layout, c_abi_t abi, bool result,
                                   c_registers_t& registers);

} // namespace fb
