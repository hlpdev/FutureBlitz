#include "fb/dynamic_library.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// The unsigned number of `size` bytes at `at` in `bytes`, little-endian unless `big`, or
/// nullopt past their end.
std::optional<std::uint64_t> number_at(const std::string_view bytes, const std::uint64_t at,
                                       const std::size_t size, const bool big = false) {
    if (at > bytes.size() || bytes.size() - at < size) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const std::size_t byte = big ? i : size - 1 - i;
        value = (value << 8U) | static_cast<unsigned char>(bytes[at + byte]);
    }

    return value;
}

/// The NUL-ended text at `at` in `bytes`, or nullopt when it doesn't end before they do.
std::optional<std::string> text_at(const std::string_view bytes, const std::uint64_t at) {
    if (at >= bytes.size()) {
        return std::nullopt;
    }

    const std::size_t end = bytes.find('\0', at);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }

    return std::string(bytes.substr(at, end - at));
}

/// A 64-bit, little-endian ELF shared object's SONAME, or "" for one without: DT_SONAME (14) is
/// the name's place in the string table that DT_STRTAB (5) gives the address of, found through
/// the loaded segments. An executable, position-independent or not, isn't one.
std::expected<std::string, library_problem_t> elf_name(const std::string_view bytes,
                                                       const std::uint64_t machine) {
    if (!bytes.starts_with("\x7F"
                           "ELF") ||
        bytes.size() < 64) {
        return std::unexpected(library_problem_t::NOT_A_LIBRARY);
    }

    // A 32-bit or big-endian one is another CPU's.
    if (bytes[4] != 2 || bytes[5] != 1 || number_at(bytes, 18, 2) != machine) {
        return std::unexpected(library_problem_t::OTHER_TARGET);
    }

    constexpr std::uint64_t executable = 2;
    constexpr std::uint64_t shared_object = 3;
    const std::optional<std::uint64_t> type = number_at(bytes, 16, 2);
    if (type == executable) {
        return std::unexpected(library_problem_t::EXECUTABLE);
    }

    if (type != shared_object) {
        return std::unexpected(library_problem_t::NOT_A_LIBRARY);
    }

    struct segment_t {
        std::uint64_t address;
        std::uint64_t offset;
        std::uint64_t size;
    };
    std::vector<segment_t> loaded;
    std::optional<segment_t> dynamic;
    const std::uint64_t headers = number_at(bytes, 32, 8).value_or(0);
    const std::uint64_t header_size = number_at(bytes, 54, 2).value_or(0);
    const std::uint64_t count = number_at(bytes, 56, 2).value_or(0);
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t header = headers + (i * header_size);
        const std::optional<std::uint64_t> segment_type = number_at(bytes, header, 4);
        const std::optional<std::uint64_t> offset = number_at(bytes, header + 8, 8);
        const std::optional<std::uint64_t> address = number_at(bytes, header + 16, 8);
        const std::optional<std::uint64_t> size = number_at(bytes, header + 32, 8);
        if (!segment_type || !offset || !address || !size) {
            return std::unexpected(library_problem_t::NOT_A_LIBRARY);
        }

        const segment_t segment{ .address = *address, .offset = *offset, .size = *size };
        if (*segment_type == 1) {
            loaded.push_back(segment);
        } else if (*segment_type == 2) {
            dynamic = segment;
        }
    }

    if (!dynamic) {
        return std::unexpected(library_problem_t::NOT_A_LIBRARY);
    }

    // A position-independent executable is a shared object too, with DF_1_PIE in DT_FLAGS_1.
    constexpr std::uint64_t flags = 0x6FFFFFFB;
    constexpr std::uint64_t position_independent_executable = 0x08000000;
    std::optional<std::uint64_t> name;
    std::optional<std::uint64_t> strings;
    for (std::uint64_t entry = dynamic->offset; entry - dynamic->offset + 16 <= dynamic->size;
         entry += 16) {
        const std::optional<std::uint64_t> tag = number_at(bytes, entry, 8);
        const std::optional<std::uint64_t> value = number_at(bytes, entry + 8, 8);
        if (!tag || !value) {
            return std::unexpected(library_problem_t::NOT_A_LIBRARY);
        }

        if (*tag == 0) {
            break;
        }

        if (*tag == 14) {
            name = value;
        } else if (*tag == 5) {
            strings = value;
        } else if (*tag == flags && (*value & position_independent_executable) != 0) {
            return std::unexpected(library_problem_t::EXECUTABLE);
        }
    }

    if (!name) {
        return "";
    }

    for (const segment_t& segment : loaded) {
        if (strings && *strings >= segment.address && *strings - segment.address < segment.size) {
            if (std::optional<std::string> text =
                    text_at(bytes, *strings - segment.address + segment.offset + *name)) {
                return std::move(*text);
            }
        }
    }

    return std::unexpected(library_problem_t::NOT_A_LIBRARY);
}

/// A 64-bit Mach-O dylib's install name, from its LC_ID_DYLIB (0xD), in a universal file the
/// one for `cpu`, which for arm64 isn't an arm64e one, as the linker takes it.
std::expected<std::string, library_problem_t> mach_o_name(const std::string_view bytes,
                                                          const std::uint64_t cpu) {
    constexpr std::uint64_t thin = 0xFEEDFACF;
    constexpr std::uint64_t arm64 = 0x0100000C;
    constexpr std::uint64_t arm64e = 2;
    constexpr std::uint64_t subtype_bits = 0x00FFFFFF;
    constexpr std::uint64_t universal = 0xCAFEBABE;
    constexpr std::uint64_t universal_64 = 0xCAFEBABF;
    std::string_view image = bytes;
    const std::optional<std::uint64_t> magic = number_at(bytes, 0, 4, true);
    if (magic == universal || magic == universal_64) {
        // Big-endian, as its slices' headers aren't: a count, then each slice's CPU, subtype,
        // offset, and size, 32-bit or 64-bit.
        const bool wide = magic == universal_64;
        const std::uint64_t size = wide ? 32 : 20;
        const std::uint64_t field = wide ? 8 : 4;
        const std::uint64_t slices = number_at(bytes, 4, 4, true).value_or(0);
        bool found = false;
        for (std::uint64_t i = 0; i < slices && !found; ++i) {
            const std::uint64_t slice = 8 + (i * size);
            const std::optional<std::uint64_t> kind = number_at(bytes, slice, 4, true);
            const std::optional<std::uint64_t> subtype = number_at(bytes, slice + 4, 4, true);
            const std::optional<std::uint64_t> offset = number_at(bytes, slice + 8, field, true);
            const std::optional<std::uint64_t> length =
                number_at(bytes, slice + 8 + field, field, true);
            if (!kind || !subtype || !offset || !length || *offset > bytes.size() ||
                bytes.size() - *offset < *length) {
                return std::unexpected(library_problem_t::NOT_A_LIBRARY);
            }

            if (*kind == cpu && (cpu != arm64 || (*subtype & subtype_bits) != arm64e)) {
                image = bytes.substr(*offset, *length);
                found = true;
            }
        }

        if (!found) {
            return std::unexpected(slices == 0 ? library_problem_t::NOT_A_LIBRARY
                                               : library_problem_t::OTHER_TARGET);
        }
    }

    // A 32-bit one, or one in the other byte order, is another CPU's.
    const std::optional<std::uint64_t> start = number_at(image, 0, 4);
    if (start == 0xFEEDFACE || start == 0xCEFAEDFE || start == 0xCFFAEDFE) {
        return std::unexpected(library_problem_t::OTHER_TARGET);
    }

    if (start != thin) {
        return std::unexpected(library_problem_t::NOT_A_LIBRARY);
    }

    // A thin arm64e image is another CPU's too, as its slice of a universal file is.
    if (number_at(image, 4, 4) != cpu ||
        (cpu == arm64 && (number_at(image, 8, 4).value_or(0) & subtype_bits) == arm64e)) {
        return std::unexpected(library_problem_t::OTHER_TARGET);
    }

    constexpr std::uint64_t executable = 2;
    constexpr std::uint64_t dylib = 6;
    const std::optional<std::uint64_t> type = number_at(image, 12, 4);
    if (type == executable) {
        return std::unexpected(library_problem_t::EXECUTABLE);
    }

    if (type != dylib) {
        return std::unexpected(library_problem_t::NOT_A_LIBRARY);
    }

    constexpr std::uint64_t id_dylib = 0xD;
    std::uint64_t command = 32;
    const std::uint64_t commands = number_at(image, 16, 4).value_or(0);
    for (std::uint64_t i = 0; i < commands; ++i) {
        const std::optional<std::uint64_t> kind = number_at(image, command, 4);
        const std::optional<std::uint64_t> size = number_at(image, command + 4, 4);
        if (!kind || !size || *size < 8) {
            break;
        }

        if (*kind == id_dylib) {
            if (const std::optional<std::uint64_t> name = number_at(image, command + 8, 4)) {
                if (std::optional<std::string> text = text_at(image, command + *name)) {
                    return std::move(*text);
                }
            }

            break;
        }

        command += *size;
    }

    return std::unexpected(library_problem_t::NOT_A_LIBRARY);
}

/// Whether a PE file is a DLL for `machine`: IMAGE_FILE_DLL (0x2000) in its characteristics.
std::expected<std::string, library_problem_t> pe_name(const std::string_view bytes,
                                                      const std::uint64_t machine) {
    const std::uint64_t pe = number_at(bytes, 0x3C, 4).value_or(bytes.size());
    constexpr std::uint64_t is_dll = 0x2000;
    const std::optional<std::uint64_t> characteristics = number_at(bytes, pe + 22, 2);
    if (!bytes.starts_with("MZ") ||
        bytes.substr(std::min<std::uint64_t>(pe, bytes.size()), 4) !=
            std::string_view("PE\0\0", 4) ||
        !characteristics) {
        return std::unexpected(library_problem_t::NOT_A_LIBRARY);
    }

    if (number_at(bytes, pe + 4, 2) != machine) {
        return std::unexpected(library_problem_t::OTHER_TARGET);
    }

    if ((*characteristics & is_dll) == 0) {
        return std::unexpected(library_problem_t::EXECUTABLE);
    }

    return "";
}

} // namespace

std::expected<std::string, library_problem_t> dynamic_library_name(const std::string_view bytes,
                                                                   const std::string_view target) {
    constexpr std::uint64_t amd64 = 0x8664;
    constexpr std::uint64_t x86_64 = 62;
    constexpr std::uint64_t aarch64 = 183;
    constexpr std::uint64_t apple_arm64 = 0x0100000C;
    constexpr std::uint64_t apple_x86_64 = 0x01000007;
    if (target == "windows-x64") {
        return pe_name(bytes, amd64);
    }

    if (target == "macos-arm64" || target == "macos-x64") {
        return mach_o_name(bytes, target == "macos-arm64" ? apple_arm64 : apple_x86_64);
    }

    return elf_name(bytes, target == "linux-arm64" ? aarch64 : x86_64);
}

} // namespace fb
