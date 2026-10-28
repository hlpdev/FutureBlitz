#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// The least of a dynamic library's file each OS's loader reads its name from, for tests of what
// fb reads there on any OS.

namespace fb_test {

/// Puts `value` in `bytes` at `at`, in `size` bytes, little-endian unless `big`, growing them
/// to fit.
inline void put(std::string& bytes, const std::size_t at, const std::uint64_t value,
                const std::size_t size, const bool big = false) {
    if (bytes.size() < at + size) {
        bytes.resize(at + size);
    }

    for (std::size_t i = 0; i < size; ++i) {
        const std::size_t shift = 8 * (big ? size - 1 - i : i);
        bytes[at + i] = static_cast<char>((value >> shift) & 0xFFU);
    }
}

constexpr std::uint16_t elf_x86_64 = 62;
constexpr std::uint16_t elf_aarch64 = 183;
constexpr std::uint32_t mach_o_arm64 = 0x0100000C;
constexpr std::uint32_t mach_o_x86_64 = 0x01000007;
constexpr std::uint16_t pe_amd64 = 0x8664;
constexpr std::uint16_t pe_arm64 = 0xAA64;

/// A 64-bit ELF file of `type`, 3 for a shared object, with one loaded segment and a dynamic
/// one, whose DT_SONAME is `soname` unless it's empty, and whose DT_FLAGS_1 is `flags` unless
/// they're 0.
inline std::string elf_library(const std::uint16_t machine, const std::string_view soname,
                               const std::uint16_t type = 3, const std::uint64_t flags = 0) {
    constexpr std::size_t headers = 64;
    constexpr std::size_t header_size = 56;
    constexpr std::size_t dynamic = headers + (2 * header_size);
    constexpr std::size_t strings = dynamic + (std::size_t{ 4 } * 16);
    constexpr std::uint64_t address = 0x10000;
    std::string bytes = "\x7F"
                        "ELF";
    bytes += std::string{ 2, 1, 1 };
    put(bytes, 16, type, 2);
    put(bytes, 18, machine, 2);
    put(bytes, 32, headers, 8);
    put(bytes, 54, header_size, 2);
    put(bytes, 56, 2, 2);
    const std::string text = std::string(1, '\0') + std::string(soname) + std::string(1, '\0');
    const std::size_t size = strings + text.size();
    // PT_LOAD covers the file, and PT_DYNAMIC its entries.
    for (const auto& [kind, offset, length] :
         { std::tuple(1U, std::size_t{ 0 }, size), std::tuple(2U, dynamic, std::size_t{ 64 }) }) {
        const std::size_t header = headers + (kind == 1 ? 0 : header_size);
        put(bytes, header, kind, 4);
        put(bytes, header + 8, offset, 8);
        put(bytes, header + 16, address + offset, 8);
        put(bytes, header + 32, length, 8);
    }

    std::size_t entry = dynamic;
    put(bytes, entry, 5, 8);
    put(bytes, entry + 8, address + strings, 8);
    if (!soname.empty()) {
        entry += 16;
        put(bytes, entry, 14, 8);
        put(bytes, entry + 8, 1, 8);
    }

    if (flags != 0) {
        entry += 16;
        put(bytes, entry, 0x6FFFFFFB, 8);
        put(bytes, entry + 8, flags, 8);
    }

    // DT_NULL, all 0s, ends them.
    bytes.resize(strings);
    bytes += text;
    return bytes;
}

/// A 64-bit Mach-O file of `type`, 6 for a dylib, whose LC_ID_DYLIB is `install_name`.
inline std::string mach_o_library(const std::uint32_t cpu, const std::string_view install_name,
                                  const std::uint32_t type = 6) {
    std::string bytes;
    const std::size_t command_size = (24 + install_name.size() + 1 + 7) / 8 * 8;
    put(bytes, 0, 0xFEEDFACF, 4);
    put(bytes, 4, cpu, 4);
    put(bytes, 12, type, 4);
    put(bytes, 16, 1, 4);
    put(bytes, 20, command_size, 4);
    put(bytes, 32, 0xD, 4);
    put(bytes, 36, command_size, 4);
    put(bytes, 40, 24, 4);
    bytes.resize(56);
    bytes += install_name;
    bytes.resize(32 + command_size);
    return bytes;
}

/// One CPU's file in a universal Mach-O file.
struct slice_t {
    std::uint32_t cpu;
    std::string image;
    std::uint32_t subtype = 0;
};

/// A universal Mach-O file of `slices`.
inline std::string universal(const std::vector<slice_t>& slices) {
    std::string bytes;
    put(bytes, 0, 0xCAFEBABE, 4, true);
    put(bytes, 4, slices.size(), 4, true);
    std::size_t offset = 8 + (20 * slices.size());
    std::string images;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const std::size_t slice = 8 + (20 * i);
        put(bytes, slice, slices[i].cpu, 4, true);
        put(bytes, slice + 4, slices[i].subtype, 4, true);
        put(bytes, slice + 8, offset, 4, true);
        put(bytes, slice + 12, slices[i].image.size(), 4, true);
        offset += slices[i].image.size();
        images += slices[i].image;
    }

    bytes.resize(8 + (20 * slices.size()));
    return bytes + images;
}

/// A PE file for `machine`, a DLL unless `dll` is false.
inline std::string pe_library(const std::uint16_t machine, const bool dll = true) {
    std::string bytes = "MZ";
    put(bytes, 0x3C, 64, 4);
    bytes.resize(64);
    bytes += std::string("PE\0\0", 4);
    put(bytes, 68, machine, 2);
    put(bytes, 86, dll ? 0x2022U : 0x0022U, 2);
    bytes.resize(256);
    return bytes;
}

} // namespace fb_test
