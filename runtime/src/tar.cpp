// std.compress's Tar: a reader of a tar's entries, POSIX's ustar with pax's extended headers and
// GNU's long names and big numbers, and old tars without a magic, and a writer of ustar, with a
// pax header for a name ustar can't hold. It needs no library, so it's in the runtime's own
// archive, which links it only for a program that uses it.

#include "compress_internal.hpp"
#include "fb/compress.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using fb::compress::KIND_FILE;
using fb::compress::KIND_FOLDER;
using fb::compress::KIND_LINK;
using fb::compress::most_bytes;
using fb::compress::output_t;
using fb::compress::RESULT_DONE;
using fb::compress::RESULT_ENDS_TOO_SOON;
using fb::compress::RESULT_INVALID;
using fb::compress::RESULT_TOO_LARGE;
using fb::compress::RESULT_UNSUPPORTED;

namespace {

constexpr std::size_t block = 512;

/// Where a header's fields are, and how long each is.
struct field_t {
    std::size_t at;
    std::size_t size;
};

constexpr field_t name_field{ .at = 0, .size = 100 };
constexpr field_t mode_field{ .at = 100, .size = 8 };
constexpr field_t uid_field{ .at = 108, .size = 8 };
constexpr field_t gid_field{ .at = 116, .size = 8 };
constexpr field_t size_field{ .at = 124, .size = 12 };
constexpr field_t time_field{ .at = 136, .size = 12 };
constexpr field_t checksum_field{ .at = 148, .size = 8 };
constexpr std::size_t type_at = 156;
constexpr field_t link_field{ .at = 157, .size = 100 };
constexpr field_t magic_field{ .at = 257, .size = 6 };
constexpr field_t version_field{ .at = 263, .size = 2 };
constexpr field_t owner_field{ .at = 265, .size = 32 };
constexpr field_t group_field{ .at = 297, .size = 32 };
constexpr field_t prefix_field{ .at = 345, .size = 155 };

using header_t = std::array<std::uint8_t, block>;

/// A field's text, up to its first NUL.
std::string_view text_of(const std::span<const std::uint8_t> header, const field_t field) {
    const std::string_view text(reinterpret_cast<const char*>(header.data()) + field.at,
                                field.size);
    return text.substr(0, text.find('\0'));
}

/// A field's number: octal digits, with spaces and NULs around them, or GNU's base-256, a
/// big-endian number after a first byte with its top bit set. Nothing for anything else, or one
/// that's negative or too large.
std::optional<std::int64_t> number_of(const std::span<const std::uint8_t> header,
                                      const field_t field) {
    const std::span<const std::uint8_t> bytes = header.subspan(field.at, field.size);
    if ((bytes[0] & 0x80U) != 0) {
        if (bytes[0] != 0x80) {
            return std::nullopt;
        }

        std::uint64_t value = 0;
        for (std::size_t i = 1; i < bytes.size(); ++i) {
            if (value > (UINT64_MAX >> 8U)) {
                return std::nullopt;
            }

            value = (value << 8U) | bytes[i];
        }

        return value > INT64_MAX ? std::nullopt : std::optional(static_cast<std::int64_t>(value));
    }

    std::size_t at = 0;
    while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == 0)) {
        ++at;
    }

    std::int64_t value = 0;
    bool digits = false;
    for (; at < bytes.size() && bytes[at] >= '0' && bytes[at] <= '7'; ++at) {
        if (value > (INT64_MAX >> 3)) {
            return std::nullopt;
        }

        value = (value * 8) + (bytes[at] - '0');
        digits = true;
    }

    for (; at < bytes.size(); ++at) {
        if (bytes[at] != ' ' && bytes[at] != 0) {
            return std::nullopt;
        }
    }

    // An empty field is 0, as some tars write a time or an owner.
    return digits || at == bytes.size() ? std::optional(value) : std::nullopt;
}

/// Whether the header's checksum is its bytes' sum, with the checksum's own read as spaces, by
/// bytes read unsigned, or signed, as some old tars summed them.
bool checksum_matches(const std::span<const std::uint8_t> header) {
    const std::optional<std::int64_t> stored = number_of(header, checksum_field);
    if (!stored) {
        return false;
    }

    std::int64_t unsigned_sum = 0;
    std::int64_t signed_sum = 0;
    for (std::size_t i = 0; i < block; ++i) {
        const bool in_checksum = i >= checksum_field.at && i < checksum_field.at + 8;
        const std::uint8_t byte = in_checksum ? ' ' : header[i];
        unsigned_sum += byte;
        signed_sum += static_cast<std::int8_t>(byte);
    }

    return *stored == unsigned_sum || *stored == signed_sum;
}

/// The values of a pax extended header's records, "<length> <key>=<value>\n", that the reader
/// uses: a name, a link's target, and a size.
struct pax_t {
    std::optional<std::string> path;
    std::optional<std::string> link;
    std::optional<std::int64_t> size;
};

/// Reads pax records into `pax`, or gives false when they aren't well-formed.
bool read_pax(std::string_view records, pax_t& pax) {
    while (!records.empty()) {
        const std::size_t space = records.find(' ');
        if (space == std::string_view::npos || space == 0) {
            return false;
        }

        std::size_t length = 0;
        for (const char digit : records.substr(0, space)) {
            if (digit < '0' || digit > '9' || length > records.size()) {
                return false;
            }

            length = (length * 10) + static_cast<std::size_t>(digit - '0');
        }

        if (length <= space + 1 || length > records.size() || records[length - 1] != '\n') {
            return false;
        }

        const std::string_view record = records.substr(space + 1, length - space - 2);
        records.remove_prefix(length);
        const std::size_t equals = record.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }

        const std::string_view key = record.substr(0, equals);
        const std::string_view value = record.substr(equals + 1);
        if (key == "path") {
            pax.path = std::string(value);
        } else if (key == "linkpath") {
            pax.link = std::string(value);
        } else if (key == "size") {
            std::int64_t size = 0;
            for (const char digit : value) {
                if (digit < '0' || digit > '9' || size > (INT64_MAX - 9) / 10) {
                    return false;
                }

                size = (size * 10) + (digit - '0');
            }

            pax.size = size;
        }
    }

    return true;
}

/// A name without the '/' that ends a folder's, as every archive's names are given.
std::string without_slash(std::string name) {
    while (name.size() > 1 && name.back() == '/') {
        name.pop_back();
    }

    return name;
}

struct entry_t {
    std::string name;
    std::int64_t size;
    std::int32_t kind;
    std::string target;
    /// Where its bytes start, in the reader's bytes, and whether it's GNU's sparse file, which
    /// the reader doesn't put together.
    std::size_t data;
    bool sparse;
};

struct reader_t {
    std::vector<std::uint8_t> bytes;
    std::vector<entry_t> entries;
    std::int32_t result = RESULT_DONE;
    std::string message;
};

std::int32_t fail(reader_t& reader, const std::int32_t result, const std::string_view message) {
    reader.message = message;
    return result;
}

/// Lists the entries of the tar in the reader's bytes: a result_t.
std::int32_t open(reader_t& reader) {
    const std::span<const std::uint8_t> bytes(reader.bytes);
    pax_t pax;
    std::optional<std::string> long_name;
    std::optional<std::string> long_link;
    std::size_t at = 0;
    while (true) {
        if (at == bytes.size()) {
            // A tar that ends without its two zero blocks ends where its entries do, as GNU's
            // tar takes it.
            break;
        }

        if (bytes.size() - at < block) {
            return fail(reader, RESULT_ENDS_TOO_SOON, "");
        }

        const std::span<const std::uint8_t> header = bytes.subspan(at, block);
        if (std::ranges::all_of(header, [](const std::uint8_t byte) { return byte == 0; })) {
            break;
        }

        if (!checksum_matches(header)) {
            return fail(reader, RESULT_INVALID,
                        "the header at byte " + std::to_string(at) + " doesn't match its checksum");
        }

        const std::optional<std::int64_t> stored_size = number_of(header, size_field);
        if (!stored_size) {
            return fail(reader, RESULT_INVALID,
                        "the header at byte " + std::to_string(at) + " has no size");
        }

        const char type = static_cast<char>(header[type_at]);
        // Links, folders, devices, and FIFOs have no bytes, whatever their size says, but pax's.
        const bool has_bytes = type == '0' || type == '\0' || type == '7' || type == 'x' ||
                               type == 'g' || type == 'L' || type == 'K' || type == 'S';
        std::int64_t size = has_bytes ? *stored_size : 0;
        if ((type == '0' || type == '\0' || type == '7' || type == 'S') && pax.size) {
            size = *pax.size;
        }

        const std::size_t data = at + block;
        const std::uint64_t blocks = (static_cast<std::uint64_t>(size) + block - 1) / block;
        if (static_cast<std::uint64_t>(bytes.size() - data) / block < blocks) {
            return fail(reader, RESULT_ENDS_TOO_SOON, "");
        }

        at = data + (blocks * block);
        const std::string_view content(reinterpret_cast<const char*>(bytes.data()) + data,
                                       static_cast<std::size_t>(size));
        if (type == 'x') {
            if (!read_pax(content, pax)) {
                return fail(reader, RESULT_INVALID, "a pax header's records are broken");
            }

            continue;
        }

        if (type == 'g') {
            continue;
        }

        if (type == 'L' || type == 'K') {
            std::string text(content.substr(0, content.find('\0')));
            (type == 'L' ? long_name : long_link) = std::move(text);
            continue;
        }

        std::string name;
        if (pax.path) {
            name = *pax.path;
        } else if (long_name) {
            name = *long_name;
        } else {
            const std::string_view prefix = text_of(header, prefix_field);
            const bool ustar = text_of(header, magic_field) == "ustar";
            name = ustar && !prefix.empty() ? std::string(prefix) + "/" : "";
            name += text_of(header, name_field);
        }

        std::string target(text_of(header, link_field));
        if (pax.link) {
            target = *pax.link;
        } else if (long_link) {
            target = *long_link;
        }

        pax = {};
        long_name.reset();
        long_link.reset();
        std::int32_t kind = KIND_FILE;
        if (type == '5') {
            kind = KIND_FOLDER;
        } else if (type == '1' || type == '2') {
            kind = KIND_LINK;
        } else if (type != '0' && type != '\0' && type != '7' && type != 'S') {
            // Devices, FIFOs, and the rest aren't entries a program reads.
            continue;
        }

        reader.entries.push_back({
            .name = without_slash(fb::compress::utf8_or_replaced(name)),
            .size = size,
            .kind = kind,
            .target = kind == KIND_LINK ? fb::compress::utf8_or_replaced(target) : "",
            .data = data,
            .sparse = type == 'S',
        });
    }

    return RESULT_DONE;
}

/// Puts `text` in a field, which stays NUL after it.
void put_text(header_t& header, const field_t field, const std::string_view text) {
    std::ranges::copy(text.substr(0, field.size), header.data() + field.at);
}

/// Puts `value` in a field as octal digits, padded with zeros, before a NUL.
void put_number(header_t& header, const field_t field, std::uint64_t value) {
    for (std::size_t i = field.size - 1; i-- > 0;) {
        header[field.at + i] = static_cast<std::uint8_t>('0' + (value & 7U));
        value >>= 3U;
    }
}

/// A ustar header of an entry, with its checksum.
header_t header_of(const std::string_view name, const std::string_view prefix,
                   const std::uint64_t size, const char type, const std::uint64_t mode) {
    header_t header{};
    put_text(header, name_field, name);
    put_number(header, mode_field, mode);
    put_number(header, uid_field, 0);
    put_number(header, gid_field, 0);
    put_number(header, size_field, size);
    put_number(header, time_field, 0);
    header[type_at] = static_cast<std::uint8_t>(type);
    put_text(header, magic_field, "ustar");
    put_text(header, version_field, "00");
    put_text(header, owner_field, "root");
    put_text(header, group_field, "root");
    put_text(header, prefix_field, prefix);
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < block; ++i) {
        const bool in_checksum = i >= checksum_field.at && i < checksum_field.at + 8;
        sum += in_checksum ? static_cast<std::uint8_t>(' ') : header[i];
    }

    // Six digits, a NUL, and a space, as tars write it.
    put_number(header, { .at = checksum_field.at, .size = 7 }, sum);
    header[checksum_field.at + 7] = ' ';
    return header;
}

/// Where to split a name into ustar's prefix and name, at a '/', or nothing when it can't be.
std::optional<std::size_t> ustar_split(const std::string_view name) {
    if (name.size() <= name_field.size) {
        return 0;
    }

    for (std::size_t slash = name.find('/'); slash != std::string_view::npos;
         slash = name.find('/', slash + 1)) {
        if (slash > 0 && slash <= prefix_field.size && name.size() - slash - 1 <= name_field.size &&
            slash + 1 < name.size()) {
            return slash;
        }
    }

    return std::nullopt;
}

/// A pax record, "<length> <key>=<value>\n", whose length counts itself.
std::string pax_record(const std::string_view key, const std::string_view value) {
    const std::size_t rest = 1 + key.size() + 1 + value.size() + 1;
    std::size_t length = rest + 1;
    while (std::to_string(length).size() + rest > length) {
        ++length;
    }

    return std::to_string(length) + " " + std::string(key) + "=" + std::string(value) + "\n";
}

struct writer_t {
    std::vector<std::uint8_t> bytes;
};

void append(writer_t& writer, const std::span<const std::uint8_t> bytes) {
    writer.bytes.insert(writer.bytes.end(), bytes.begin(), bytes.end());
    writer.bytes.resize(writer.bytes.size() + ((block - (bytes.size() % block)) % block), 0);
}

reader_t& reader_of(void* reader) {
    return *static_cast<reader_t*>(reader);
}

const reader_t& reader_of(const void* reader) {
    return *static_cast<const reader_t*>(reader);
}

const entry_t& entry_of(const void* reader, const std::int64_t index) {
    return reader_of(reader).entries[static_cast<std::size_t>(index)];
}

} // namespace

extern "C" {

void* fb_tar_reader_new(const fb_array_t* data) {
    const std::span<const std::uint8_t> bytes = fb::compress::bytes_of(data);
    auto* reader = new reader_t{ .bytes = { bytes.begin(), bytes.end() } };
    reader->result = open(*reader);
    return reader;
}

std::int32_t fb_tar_reader_result(const void* reader) {
    return reader_of(reader).result;
}

std::int64_t fb_tar_reader_count(const void* reader) {
    return static_cast<std::int64_t>(reader_of(reader).entries.size());
}

fb_string_t* fb_tar_reader_name(const void* reader, const std::int64_t index) {
    return fb::compress::string_of(entry_of(reader, index).name);
}

std::int64_t fb_tar_reader_size(const void* reader, const std::int64_t index) {
    return entry_of(reader, index).size;
}

std::int32_t fb_tar_reader_kind(const void* reader, const std::int64_t index) {
    return entry_of(reader, index).kind;
}

fb_string_t* fb_tar_reader_target(const void* reader, const std::int64_t index) {
    return fb::compress::string_of(entry_of(reader, index).target);
}

std::int32_t fb_tar_reader_read(void* reader, const std::int64_t index, fb_array_t* output) {
    reader_t& state = reader_of(reader);
    const entry_t& entry = entry_of(reader, index);
    output->count = 0;
    if (entry.sparse) {
        return fail(state, RESULT_UNSUPPORTED, "'" + entry.name + "' is a GNU sparse file");
    }

    if (entry.size > most_bytes) {
        return fail(state, RESULT_TOO_LARGE, "");
    }

    output_t out(output, entry.size);
    out.add_all(std::span(state.bytes).subspan(entry.data, static_cast<std::size_t>(entry.size)));
    return RESULT_DONE;
}

fb_string_t* fb_tar_reader_message(const void* reader) {
    return fb::compress::string_of(reader_of(reader).message);
}

void fb_tar_reader_free(void* reader) {
    delete static_cast<reader_t*>(reader);
}

void* fb_tar_writer_new() {
    return new writer_t;
}

void fb_tar_writer_add(void* writer, const fb_string_t* name, const fb_array_t* data,
                       const std::int32_t folder) {
    auto& state = *static_cast<writer_t*>(writer);
    std::string path(fb::compress::text_of(name));
    if (folder != 0 && !path.ends_with('/')) {
        path += '/';
    }

    const std::span<const std::uint8_t> bytes = fb::compress::bytes_of(data);
    const bool ascii = std::ranges::all_of(
        path, [](const char byte) { return static_cast<std::uint8_t>(byte) < 0x80; });
    const std::optional<std::size_t> split = ascii ? ustar_split(path) : std::nullopt;
    std::string_view prefix;
    std::string_view rest = path;
    if (split && *split != 0) {
        prefix = rest.substr(0, *split);
        rest.remove_prefix(*split + 1);
    }

    // A name ustar can't hold, or that isn't ASCII, goes in a pax header before it, which says
    // it's UTF-8, and the ustar header has as much of it as fits.
    if (!split) {
        const std::string records = pax_record("path", path);
        const header_t pax = header_of("././@PaxHeader", "", records.size(), 'x', 0644);
        append(state, pax);
        append(state, { reinterpret_cast<const std::uint8_t*>(records.data()), records.size() });
        rest = std::string_view(path).substr(0, name_field.size);
    }

    const header_t header = header_of(rest, prefix, folder != 0 ? 0 : bytes.size(),
                                      folder != 0 ? '5' : '0', folder != 0 ? 0755 : 0644);
    append(state, header);
    if (folder == 0) {
        append(state, bytes);
    }
}

void fb_tar_writer_finish(void* writer, fb_array_t* output) {
    auto& state = *static_cast<writer_t*>(writer);
    state.bytes.resize(state.bytes.size() + (2 * block), 0);
    output_t out(output, most_bytes);
    out.add_all(state.bytes);
    state.bytes.clear();
}

void fb_tar_writer_free(void* writer) {
    delete static_cast<writer_t*>(writer);
}
}
