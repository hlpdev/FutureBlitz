// std.compress's 7z reading, over liblzma: the signature header, the header after it, which may be
// encoded as a folder of its own, its streams, folders, and files, and each folder's coders, a
// chain of what liblzma decodes. A folder is decoded whole, as a solid one has to be, and the
// last one decoded is kept, so reading its files in turn decodes it once. It's in liblzma's part
// of the runtime, as an object file of its own.

#include "compress_internal.hpp"
#include "fb/compress.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <lzma.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using fb::compress::chunk_size;
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

constexpr std::array<std::uint8_t, 6> signature = { '7', 'z', 0xbc, 0xaf, 0x27, 0x1c };
constexpr std::size_t signature_size = 32;

/// The header's property IDs.
enum property_t : std::uint8_t {
    ID_END = 0x00,
    ID_HEADER = 0x01,
    ID_ARCHIVE_PROPERTIES = 0x02,
    ID_ADDITIONAL_STREAMS = 0x03,
    ID_MAIN_STREAMS = 0x04,
    ID_FILES = 0x05,
    ID_PACK_INFO = 0x06,
    ID_UNPACK_INFO = 0x07,
    ID_SUBSTREAMS = 0x08,
    ID_SIZE = 0x09,
    ID_CRC = 0x0a,
    ID_FOLDER = 0x0b,
    ID_UNPACK_SIZE = 0x0c,
    ID_UNPACK_STREAMS = 0x0d,
    ID_EMPTY_STREAM = 0x0e,
    ID_EMPTY_FILE = 0x0f,
    ID_ANTI = 0x10,
    ID_NAME = 0x11,
    ID_ATTRIBUTES = 0x15,
    ID_ENCODED_HEADER = 0x17,
};

/// The bit of Windows' attributes that says a file's top 16 bits are a Unix mode, as 7-Zip
/// writes it, and the modes of a link and its kind's bits.
constexpr std::uint32_t unix_extension = 0x8000;
constexpr std::uint32_t mode_kind = 0170000;
constexpr std::uint32_t mode_link = 0120000;
constexpr std::uint32_t windows_folder = 0x10;

/// A reader of a header's bytes, which stops at their end: a read past it gives 0, and the
/// header is broken.
class cursor_t {
public:
    explicit cursor_t(const std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

    [[nodiscard]] bool broken() const {
        return m_broken;
    }

    [[nodiscard]] std::size_t left() const {
        return m_bytes.size() - m_at;
    }

    std::uint8_t byte() {
        if (m_at == m_bytes.size()) {
            m_broken = true;
            return 0;
        }

        return m_bytes[m_at++];
    }

    std::span<const std::uint8_t> take(const std::uint64_t count) {
        if (count > left()) {
            m_broken = true;
            m_at = m_bytes.size();
            return {};
        }

        const std::span<const std::uint8_t> taken = m_bytes.subspan(m_at, count);
        m_at += count;
        return taken;
    }

    /// 7z's number: the first byte's high bits, as many as are 1, say how many bytes follow,
    /// little-endian, and its bits after them are the top of the number.
    std::uint64_t number() {
        const std::uint8_t first = byte();
        std::uint64_t value = 0;
        std::uint32_t mask = 0x80U;
        for (std::uint32_t i = 0; i < 8; ++i) {
            if ((first & mask) == 0) {
                const std::uint64_t high = first & (mask - 1U);
                return value | (high << (8U * i));
            }

            value |= static_cast<std::uint64_t>(byte()) << (8U * i);
            mask >>= 1U;
        }

        return value;
    }

    /// A count of what follows, which can't be more than the bytes left times `per_byte`, as
    /// each takes at least a bit or a byte; past that, the header is broken.
    std::uint64_t count(const std::uint64_t per_byte = 1) {
        const std::uint64_t value = number();
        if (value > static_cast<std::uint64_t>(left()) * per_byte) {
            m_broken = true;
            return 0;
        }

        return value;
    }

    std::uint16_t uint16() {
        const std::uint32_t low = byte();
        const std::uint32_t high = byte();
        return static_cast<std::uint16_t>(low | (high << 8U));
    }

    std::uint32_t uint32() {
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(byte()) << (8U * i);
        }

        return value;
    }

    std::uint64_t uint64() {
        const std::uint64_t low = uint32();
        return low | (static_cast<std::uint64_t>(uint32()) << 32U);
    }

    /// `count` bits, the first in each byte's top bit.
    std::vector<bool> bits(const std::uint64_t count) {
        std::vector<bool> made;
        made.reserve(count);
        std::uint32_t current = 0;
        for (std::uint64_t i = 0; i < count && !m_broken; ++i) {
            if (i % 8 == 0) {
                current = byte();
            }

            made.push_back((current & (0x80U >> (i % 8))) != 0);
        }

        return made;
    }

    /// `count` bits after a byte that's 1 when they're all set.
    std::vector<bool> defined(const std::uint64_t count) {
        return byte() != 0 ? std::vector<bool>(count, true) : bits(count);
    }

    /// `count` CRCs, each there or not.
    std::vector<std::optional<std::uint32_t>> digests(const std::uint64_t count) {
        const std::vector<bool> there = defined(count);
        std::vector<std::optional<std::uint32_t>> made;
        made.reserve(there.size());
        for (const bool is : there) {
            made.push_back(is ? std::optional(uint32()) : std::nullopt);
        }

        return made;
    }

private:
    std::span<const std::uint8_t> m_bytes;
    std::size_t m_at = 0;
    bool m_broken = false;
};

struct coder_t {
    std::vector<std::uint8_t> id;
    std::vector<std::uint8_t> properties;
    std::uint64_t inputs = 1;
    std::uint64_t outputs = 1;
};

struct folder_t {
    std::vector<coder_t> coders;
    /// Each bind pair: a coder's input, and the output that goes into it.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> binds;
    std::vector<std::uint64_t> packed;
    std::vector<std::uint64_t> sizes;
    std::optional<std::uint32_t> crc;
    /// Its first packed stream, of the streams'.
    std::size_t first_stream = 0;
};

struct streams_t {
    std::uint64_t pack_position = 0;
    std::vector<std::uint64_t> pack_sizes;
    std::vector<folder_t> folders;
    /// Each folder's substreams: how many, and each one's size and CRC.
    std::vector<std::uint64_t> counts;
    std::vector<std::uint64_t> sizes;
    std::vector<std::optional<std::uint32_t>> crcs;
};

struct file_t {
    std::string name;
    std::uint64_t size = 0;
    std::int32_t kind = KIND_FILE;
    std::string target;
    bool has_stream = false;
    std::size_t folder = 0;
    std::uint64_t offset = 0;
    std::optional<std::uint32_t> crc;
};

/// What reading a header or a folder gives: a result_t, and why when it isn't 0.
struct status_t {
    std::int32_t result = RESULT_DONE;
    std::string message;

    [[nodiscard]] bool failed() const {
        return result != RESULT_DONE;
    }
};

status_t broken(const std::string_view what) {
    return { .result = RESULT_INVALID, .message = std::string(what) };
}

status_t unsupported(const std::string_view what) {
    return { .result = RESULT_UNSUPPORTED, .message = std::string(what) };
}

/// Skips a property's data, which says its size.
void skip(cursor_t& cursor) {
    cursor.take(cursor.number());
}

void read_pack_info(cursor_t& cursor, streams_t& streams) {
    streams.pack_position = cursor.number();
    const std::uint64_t count = cursor.count();
    for (std::uint8_t id = cursor.byte(); id != ID_END && !cursor.broken(); id = cursor.byte()) {
        if (id == ID_SIZE) {
            for (std::uint64_t i = 0; i < count; ++i) {
                streams.pack_sizes.push_back(cursor.number());
            }
        } else if (id == ID_CRC) {
            cursor.digests(count);
        } else {
            skip(cursor);
        }
    }

    streams.pack_sizes.resize(count);
}

status_t read_folder(cursor_t& cursor, folder_t& folder) {
    const std::uint64_t coders = cursor.count();
    if (coders == 0) {
        return broken("a folder has no coders");
    }

    std::uint64_t inputs = 0;
    std::uint64_t outputs = 0;
    for (std::uint64_t i = 0; i < coders && !cursor.broken(); ++i) {
        const std::uint8_t flags = cursor.byte();
        if ((flags & 0x80U) != 0) {
            return unsupported("a coder with alternative methods");
        }

        coder_t coder;
        const std::span<const std::uint8_t> id = cursor.take(flags & 0x0fU);
        coder.id.assign(id.begin(), id.end());
        if ((flags & 0x10U) != 0) {
            coder.inputs = cursor.count();
            coder.outputs = cursor.count();
        }

        if ((flags & 0x20U) != 0) {
            const std::span<const std::uint8_t> properties = cursor.take(cursor.number());
            coder.properties.assign(properties.begin(), properties.end());
        }

        inputs += coder.inputs;
        outputs += coder.outputs;
        folder.coders.push_back(std::move(coder));
    }

    if (outputs == 0 || inputs < outputs - 1) {
        return broken("a folder's coders don't fit together");
    }

    for (std::uint64_t i = 0; i + 1 < outputs && !cursor.broken(); ++i) {
        const std::uint64_t input = cursor.number();
        const std::uint64_t output = cursor.number();
        folder.binds.emplace_back(input, output);
    }

    const std::uint64_t packed = inputs - (outputs - 1);
    if (packed == 1) {
        for (std::uint64_t input = 0; input < inputs; ++input) {
            if (std::ranges::none_of(folder.binds,
                                     [&](const auto& bind) { return bind.first == input; })) {
                folder.packed.push_back(input);
                break;
            }
        }
    } else {
        for (std::uint64_t i = 0; i < packed && !cursor.broken(); ++i) {
            folder.packed.push_back(cursor.number());
        }
    }

    if (folder.packed.size() != packed) {
        return broken("a folder's packed streams don't fit its coders");
    }

    folder.sizes.resize(outputs);
    return {};
}

status_t read_unpack_info(cursor_t& cursor, streams_t& streams) {
    if (cursor.byte() != ID_FOLDER) {
        return broken("the folders aren't where they should be");
    }

    const std::uint64_t count = cursor.count();
    if (cursor.byte() != 0) {
        return unsupported("folders in another stream");
    }

    std::size_t stream = 0;
    for (std::uint64_t i = 0; i < count && !cursor.broken(); ++i) {
        folder_t folder;
        const status_t read = read_folder(cursor, folder);
        if (read.failed()) {
            return read;
        }

        folder.first_stream = stream;
        stream += folder.packed.size();
        streams.folders.push_back(std::move(folder));
    }

    if (cursor.byte() != ID_UNPACK_SIZE) {
        return broken("the folders' sizes aren't where they should be");
    }

    for (folder_t& folder : streams.folders) {
        for (std::uint64_t& size : folder.sizes) {
            size = cursor.number();
        }
    }

    for (std::uint8_t id = cursor.byte(); id != ID_END && !cursor.broken(); id = cursor.byte()) {
        if (id == ID_CRC) {
            const std::vector<std::optional<std::uint32_t>> crcs = cursor.digests(count);
            for (std::size_t i = 0; i < crcs.size() && i < streams.folders.size(); ++i) {
                streams.folders[i].crc = crcs[i];
            }
        } else {
            skip(cursor);
        }
    }

    return {};
}

/// The size of what a folder gives: its output that no bind pair takes.
std::optional<std::uint64_t> final_output(const folder_t& folder) {
    for (std::uint64_t output = 0; output < folder.sizes.size(); ++output) {
        if (std::ranges::none_of(folder.binds,
                                 [&](const auto& bind) { return bind.second == output; })) {
            return output;
        }
    }

    return std::nullopt;
}

std::uint64_t folder_size(const folder_t& folder) {
    const std::optional<std::uint64_t> output = final_output(folder);
    return output ? folder.sizes[*output] : 0;
}

status_t read_substreams(cursor_t& cursor, streams_t& streams) {
    streams.counts.assign(streams.folders.size(), 1);
    std::uint8_t id = cursor.byte();
    if (id == ID_UNPACK_STREAMS) {
        for (std::uint64_t& count : streams.counts) {
            count = cursor.count(8);
        }

        id = cursor.byte();
    }

    for (std::size_t i = 0; i < streams.folders.size(); ++i) {
        const std::uint64_t count = streams.counts[i];
        if (count == 0) {
            continue;
        }

        std::uint64_t sum = 0;
        if (id == ID_SIZE) {
            const std::uint64_t whole = folder_size(streams.folders[i]);
            for (std::uint64_t j = 1; j < count && !cursor.broken(); ++j) {
                const std::uint64_t size = cursor.number();
                if (size > whole - sum) {
                    return broken("a folder's substreams are larger than it");
                }

                sum += size;
                streams.sizes.push_back(size);
            }
        } else if (count > 1) {
            return broken("a folder's substreams have no sizes");
        }

        const std::uint64_t total = folder_size(streams.folders[i]);
        if (sum > total) {
            return broken("a folder's substreams are larger than it");
        }

        streams.sizes.push_back(total - sum);
    }

    if (id == ID_SIZE) {
        id = cursor.byte();
    }

    // Each substream's CRC: a folder's own for its one substream, or else what comes next.
    std::uint64_t unknown = 0;
    for (std::size_t i = 0; i < streams.folders.size(); ++i) {
        if (streams.counts[i] != 1 || !streams.folders[i].crc) {
            unknown += streams.counts[i];
        }
    }

    std::vector<std::optional<std::uint32_t>> digests;
    for (; id != ID_END && !cursor.broken(); id = cursor.byte()) {
        if (id == ID_CRC) {
            digests = cursor.digests(unknown);
        } else {
            skip(cursor);
        }
    }

    std::size_t next = 0;
    for (std::size_t i = 0; i < streams.folders.size(); ++i) {
        if (streams.counts[i] == 1 && streams.folders[i].crc) {
            streams.crcs.push_back(streams.folders[i].crc);
            continue;
        }

        for (std::uint64_t j = 0; j < streams.counts[i]; ++j) {
            streams.crcs.push_back(next < digests.size() ? digests[next] : std::nullopt);
            ++next;
        }
    }

    return {};
}

status_t read_streams(cursor_t& cursor, streams_t& streams) {
    bool substreams = false;
    for (std::uint8_t id = cursor.byte(); id != ID_END && !cursor.broken(); id = cursor.byte()) {
        status_t read;
        if (id == ID_PACK_INFO) {
            read_pack_info(cursor, streams);
        } else if (id == ID_UNPACK_INFO) {
            read = read_unpack_info(cursor, streams);
        } else if (id == ID_SUBSTREAMS) {
            read = read_substreams(cursor, streams);
            substreams = true;
        } else {
            return broken("its streams' information is broken");
        }

        if (read.failed()) {
            return read;
        }
    }

    if (!substreams) {
        // Each folder is one stream.
        streams.counts.assign(streams.folders.size(), 1);
        for (const folder_t& folder : streams.folders) {
            streams.sizes.push_back(folder_size(folder));
            streams.crcs.push_back(folder.crc);
        }
    }

    return cursor.broken() ? broken("its streams' information is broken") : status_t{};
}

/// liblzma's filter for a coder, or nothing for one it doesn't have. Copy is LZMA_VLI_UNKNOWN.
std::optional<lzma_vli> filter_of(const std::span<const std::uint8_t> id) {
    const auto is = [&](const std::initializer_list<std::uint8_t> bytes) {
        return std::ranges::equal(id, bytes);
    };
    if (is({ 0x00 })) {
        return LZMA_VLI_UNKNOWN;
    }

    if (is({ 0x03, 0x01, 0x01 })) {
        return LZMA_FILTER_LZMA1;
    }

    if (is({ 0x21 })) {
        return LZMA_FILTER_LZMA2;
    }

    if (is({ 0x03 })) {
        return LZMA_FILTER_DELTA;
    }

    if (is({ 0x03, 0x03, 0x01, 0x03 }) || is({ 0x04 })) {
        return LZMA_FILTER_X86;
    }

    if (is({ 0x03, 0x03, 0x02, 0x05 }) || is({ 0x05 })) {
        return LZMA_FILTER_POWERPC;
    }

    if (is({ 0x03, 0x03, 0x04, 0x01 }) || is({ 0x06 })) {
        return LZMA_FILTER_IA64;
    }

    if (is({ 0x03, 0x03, 0x05, 0x01 }) || is({ 0x07 })) {
        return LZMA_FILTER_ARM;
    }

    if (is({ 0x03, 0x03, 0x07, 0x01 }) || is({ 0x08 })) {
        return LZMA_FILTER_ARMTHUMB;
    }

    if (is({ 0x03, 0x03, 0x08, 0x05 }) || is({ 0x09 })) {
        return LZMA_FILTER_SPARC;
    }

    if (is({ 0x0a })) {
        return LZMA_FILTER_ARM64;
    }

    if (is({ 0x0b })) {
        return LZMA_FILTER_RISCV;
    }

    return std::nullopt;
}

/// A coder's name, for saying it isn't supported.
std::string coder_name(const std::span<const std::uint8_t> id) {
    const auto is = [&](const std::initializer_list<std::uint8_t> bytes) {
        return std::ranges::equal(id, bytes);
    };
    if (is({ 0x03, 0x03, 0x01, 0x1b })) {
        return "BCJ2";
    }

    if (is({ 0x03, 0x04, 0x01 })) {
        return "PPMd";
    }

    if (is({ 0x04, 0x01, 0x08 })) {
        return "Deflate";
    }

    if (is({ 0x04, 0x02, 0x02 })) {
        return "BZip2";
    }

    if (is({ 0x06, 0xf1, 0x07, 0x01 })) {
        return "AES, for encryption";
    }

    std::string name = "0x";
    for (const std::uint8_t byte : id) {
        constexpr std::string_view digits = "0123456789abcdef";
        name += digits[byte >> 4U];
        name += digits[byte & 0x0fU];
    }

    return name;
}

/// liblzma's filters for a chain of coders, always ended as liblzma ends one, which frees the
/// options lzma_properties_decode gave them once it goes.
class filters_t {
public:
    filters_t() = default;
    filters_t(const filters_t&) = delete;
    filters_t& operator=(const filters_t&) = delete;
    filters_t(filters_t&&) = delete;
    filters_t& operator=(filters_t&&) = delete;

    ~filters_t() {
        lzma_filters_free(m_list.data(), nullptr);
    }

    void add(const lzma_filter filter) {
        m_list.insert(m_list.end() - 1, filter);
    }

    [[nodiscard]] std::size_t count() const {
        return m_list.size() - 1;
    }

    [[nodiscard]] const lzma_filter* chain() const {
        return m_list.data();
    }

private:
    std::vector<lzma_filter> m_list{ { .id = LZMA_VLI_UNKNOWN, .options = nullptr } };
};

/// Decodes a folder from the archive's bytes into `output`.
status_t decode(const std::span<const std::uint8_t> archive, const streams_t& streams,
                const folder_t& folder, std::vector<std::uint8_t>& output) {
    output.clear();
    // A chain of coders that each take one stream and give one, from the one that gives the
    // folder's bytes to the one that takes the packed stream, which is liblzma's order.
    for (const coder_t& coder : folder.coders) {
        if (coder.inputs != 1 || coder.outputs != 1) {
            return unsupported("its coder " + coder_name(coder.id) + " isn't supported");
        }
    }

    const std::optional<std::uint64_t> last = final_output(folder);
    if (!last || folder.packed.size() != 1) {
        return broken("a folder's coders don't fit together");
    }

    std::vector<std::uint64_t> chain{ *last };
    while (chain.size() <= folder.coders.size()) {
        const auto bound = std::ranges::find_if(
            folder.binds, [&](const auto& bind) { return bind.first == chain.back(); });
        if (bound == folder.binds.end()) {
            break;
        }

        chain.push_back(bound->second);
    }

    if (chain.size() > folder.coders.size() || chain.back() != folder.packed[0]) {
        return broken("a folder's coders don't fit together");
    }

    filters_t filters;
    for (const std::uint64_t index : chain) {
        const coder_t& coder = folder.coders[index];
        const std::optional<lzma_vli> filter = filter_of(coder.id);
        if (!filter) {
            return unsupported("its coder " + coder_name(coder.id) + " isn't supported");
        }

        if (*filter == LZMA_VLI_UNKNOWN) {
            continue;
        }

        lzma_filter made{ .id = *filter, .options = nullptr };
        if (filters.count() == LZMA_FILTERS_MAX ||
            lzma_properties_decode(&made, nullptr, coder.properties.data(),
                                   coder.properties.size()) != LZMA_OK) {
            return broken("a coder's properties are broken");
        }

        filters.add(made);
    }

    // The packed stream: the folder's first, after those of the folders before it.
    const std::size_t stream = folder.first_stream;
    if (stream >= streams.pack_sizes.size()) {
        return broken("a folder has no packed stream");
    }

    // Where it starts: none of the numbers is more than the archive's size where they're added,
    // so the sum can't wrap around.
    const std::uint64_t whole = archive.size();
    std::uint64_t start =
        streams.pack_position > whole ? whole + 1 : signature_size + streams.pack_position;
    for (std::size_t i = 0; i < stream && start <= whole; ++i) {
        start = streams.pack_sizes[i] > whole ? whole + 1 : start + streams.pack_sizes[i];
    }

    const std::uint64_t packed_size = streams.pack_sizes[stream];
    const std::uint64_t size = folder.sizes[*last];
    if (start > whole || packed_size > whole - start) {
        return { .result = RESULT_ENDS_TOO_SOON, .message = "" };
    }

    if (size > static_cast<std::uint64_t>(most_bytes)) {
        return { .result = RESULT_TOO_LARGE, .message = "" };
    }

    const std::span<const std::uint8_t> packed = archive.subspan(start, packed_size);
    if (filters.count() == 0) {
        if (packed.size() != size) {
            return broken("a stored folder's size doesn't match");
        }

        output.assign(packed.begin(), packed.end());
        return {};
    }

    // A dictionary larger than an array's bytes is one no folder here could need, and asking for
    // it would only run out of memory.
    if (lzma_raw_decoder_memusage(filters.chain()) > static_cast<std::uint64_t>(most_bytes)) {
        return unsupported("its coders need more memory than an array holds");
    }

    lzma_stream decoder = LZMA_STREAM_INIT;
    const lzma_ret started = lzma_raw_decoder(&decoder, filters.chain());
    if (started == LZMA_MEM_ERROR) {
        fb_panic("out of memory", nullptr);
    }

    if (started != LZMA_OK) {
        return unsupported("its coders' options aren't supported");
    }

    // The bytes come a chunk at a time, so a size the header claims takes no memory the coders
    // don't give.
    std::vector<std::uint8_t> chunk(chunk_size);
    decoder.next_in = packed.data();
    decoder.avail_in = packed.size();
    lzma_ret result = LZMA_OK;
    while (output.size() < size && result == LZMA_OK) {
        decoder.next_out = chunk.data();
        decoder.avail_out = std::min<std::uint64_t>(chunk.size(), size - output.size());
        const std::size_t room = decoder.avail_out;
        result = lzma_code(&decoder, decoder.avail_in != 0 ? LZMA_RUN : LZMA_FINISH);
        output.insert(output.end(), chunk.begin(),
                      chunk.begin() + static_cast<std::ptrdiff_t>(room - decoder.avail_out));
    }

    const std::uint64_t made = output.size();
    lzma_end(&decoder);
    if (result == LZMA_MEM_ERROR) {
        fb_panic("out of memory", nullptr);
    }

    if (made != size) {
        if (result == LZMA_BUF_ERROR) {
            return { .result = RESULT_ENDS_TOO_SOON, .message = "" };
        }

        return broken(result == LZMA_STREAM_END ? "a folder is smaller than its size says"
                                                : "a folder's packed stream is broken");
    }

    if (folder.crc && lzma_crc32(output.data(), output.size(), 0) != *folder.crc) {
        return broken("a folder doesn't match its CRC-32");
    }

    return {};
}

/// A file's name: UTF-16LE up to a NUL, as UTF-8, with U+FFFD for a surrogate that isn't in a
/// pair.
std::string name_from(cursor_t& cursor) {
    std::string name;
    while (!cursor.broken()) {
        const char16_t unit = cursor.uint16();
        if (unit == 0) {
            break;
        }

        char32_t code = unit;
        if (unit >= 0xd800 && unit <= 0xdbff) {
            const char16_t low = cursor.uint16();
            if (low >= 0xdc00 && low <= 0xdfff) {
                code = 0x10000 + ((static_cast<char32_t>(unit) - 0xd800U) << 10U) +
                       (static_cast<char32_t>(low) - 0xdc00U);
            } else {
                fb::compress::append_utf8(name, 0xfffd);
                if (low == 0) {
                    break;
                }

                code = low >= 0xd800 && low <= 0xdfff ? 0xfffd : low;
            }
        } else if (unit >= 0xdc00 && unit <= 0xdfff) {
            code = 0xfffd;
        }

        fb::compress::append_utf8(name, code);
    }

    return name;
}

struct reader_t {
    std::vector<std::uint8_t> bytes;
    streams_t streams;
    std::vector<file_t> files;
    /// The folder decoded last, and its bytes.
    std::optional<std::size_t> decoded;
    std::vector<std::uint8_t> folder_bytes;
    std::int32_t result = RESULT_DONE;
    std::string message;
};

std::int32_t fail(reader_t& reader, const status_t& status) {
    reader.message = status.message;
    return status.result;
}

status_t read_files(cursor_t& cursor, reader_t& reader) {
    const std::uint64_t count = cursor.count(8);
    std::vector<file_t> files(count);
    std::vector<bool> empty_stream(count, false);
    std::vector<bool> empty_file;
    std::vector<bool> anti;
    std::vector<std::optional<std::uint32_t>> attributes(count);
    for (std::uint8_t id = cursor.byte(); id != ID_END && !cursor.broken(); id = cursor.byte()) {
        const std::uint64_t size = cursor.number();
        cursor_t property(cursor.take(size));
        const auto empties = static_cast<std::uint64_t>(std::ranges::count(empty_stream, true));
        if (id == ID_EMPTY_STREAM) {
            empty_stream = property.bits(count);
        } else if (id == ID_EMPTY_FILE) {
            empty_file = property.bits(empties);
        } else if (id == ID_ANTI) {
            anti = property.bits(empties);
        } else if (id == ID_NAME) {
            if (property.byte() != 0) {
                return unsupported("names in another stream");
            }

            for (file_t& file : files) {
                file.name = name_from(property);
            }
        } else if (id == ID_ATTRIBUTES) {
            const std::vector<bool> there = property.defined(count);
            if (property.byte() != 0) {
                return unsupported("attributes in another stream");
            }

            for (std::size_t i = 0; i < there.size(); ++i) {
                if (there[i]) {
                    attributes[i] = property.uint32();
                }
            }
        }

        if (property.broken()) {
            return broken("its files' information is broken");
        }
    }

    // Files with streams take the substreams in turn, folder by folder.
    std::size_t folder = 0;
    std::uint64_t in_folder = 0;
    std::uint64_t offset = 0;
    std::size_t stream = 0;
    std::size_t empty = 0;
    std::vector<file_t> listed;
    for (std::size_t i = 0; i < files.size(); ++i) {
        file_t& file = files[i];
        bool is_anti = false;
        if (empty_stream[i]) {
            const bool is_file = empty < empty_file.size() && empty_file[empty];
            is_anti = empty < anti.size() && anti[empty];
            ++empty;
            file.kind = is_file ? KIND_FILE : KIND_FOLDER;
        } else {
            while (folder < reader.streams.counts.size() &&
                   in_folder == reader.streams.counts[folder]) {
                ++folder;
                in_folder = 0;
                offset = 0;
            }

            if (folder == reader.streams.counts.size() || stream == reader.streams.sizes.size()) {
                return broken("it has more files than streams");
            }

            file.has_stream = true;
            file.folder = folder;
            file.offset = offset;
            file.size = reader.streams.sizes[stream];
            file.crc =
                stream < reader.streams.crcs.size() ? reader.streams.crcs[stream] : std::nullopt;
            offset += file.size;
            ++in_folder;
            ++stream;
        }

        if (const std::optional<std::uint32_t> attributes_of = attributes[i]) {
            const std::uint32_t attribute = *attributes_of;
            if ((attribute & windows_folder) != 0 && !file.has_stream) {
                file.kind = KIND_FOLDER;
            } else if ((attribute & unix_extension) != 0 &&
                       ((attribute >> 16U) & mode_kind) == mode_link) {
                file.kind = KIND_LINK;
            }
        }

        // Anti-items say to remove a file an update had, so they aren't entries.
        if (!is_anti) {
            while (file.name.size() > 1 && file.name.back() == '/') {
                file.name.pop_back();
            }

            listed.push_back(std::move(file));
        }
    }

    reader.files = std::move(listed);
    return {};
}

/// Reads a header, whose first byte is ID_HEADER, after the archive's streams.
status_t read_header(cursor_t& cursor, reader_t& reader) {
    for (std::uint8_t id = cursor.byte(); id != ID_END && !cursor.broken(); id = cursor.byte()) {
        status_t read;
        if (id == ID_ARCHIVE_PROPERTIES) {
            for (std::uint8_t type = cursor.byte(); type != 0 && !cursor.broken();
                 type = cursor.byte()) {
                skip(cursor);
            }
        } else if (id == ID_ADDITIONAL_STREAMS) {
            streams_t additional;
            read = read_streams(cursor, additional);
        } else if (id == ID_MAIN_STREAMS) {
            read = read_streams(cursor, reader.streams);
        } else if (id == ID_FILES) {
            read = read_files(cursor, reader);
        } else {
            return broken("its header is broken");
        }

        if (read.failed()) {
            return read;
        }
    }

    return cursor.broken() ? broken("its header is broken") : status_t{};
}

std::int32_t open(reader_t& reader) {
    const std::span<const std::uint8_t> bytes(reader.bytes);
    if (bytes.size() < signature_size || !std::ranges::equal(bytes.first(6), signature)) {
        return fail(reader, broken("it doesn't start with 7z's signature"));
    }

    if (bytes[6] != 0) {
        return fail(reader, unsupported("its version, " + std::to_string(bytes[6]) + "." +
                                        std::to_string(bytes[7]) + ", isn't supported"));
    }

    cursor_t start(bytes.subspan(8, signature_size - 8));
    const std::uint32_t start_crc = start.uint32();
    if (lzma_crc32(bytes.data() + 12, 20, 0) != start_crc) {
        return fail(reader, broken("its start header doesn't match its CRC-32"));
    }

    const std::uint64_t offset = start.uint64();
    const std::uint64_t size = start.uint64();
    const std::uint32_t crc = start.uint32();
    if (size == 0) {
        return RESULT_DONE;
    }

    const std::uint64_t at = signature_size + offset;
    if (offset > bytes.size() || at > bytes.size() || size > bytes.size() - at) {
        return fail(reader, { .result = RESULT_ENDS_TOO_SOON, .message = "" });
    }

    std::vector<std::uint8_t> header(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
    if (lzma_crc32(header.data(), header.size(), 0) != crc) {
        return fail(reader, broken("its header doesn't match its CRC-32"));
    }

    // An encoded header is a folder that decodes to the header, which may be encoded again.
    for (int depth = 0; depth < 4 && !header.empty() && header[0] == ID_ENCODED_HEADER; ++depth) {
        cursor_t cursor{ std::span(header).subspan(1) };
        streams_t streams;
        status_t read = read_streams(cursor, streams);
        if (!read.failed() && streams.folders.empty()) {
            read = broken("its encoded header has no folder");
        }

        std::vector<std::uint8_t> decoded;
        if (!read.failed()) {
            read = decode(bytes, streams, streams.folders[0], decoded);
        }

        if (read.failed()) {
            return fail(reader, read);
        }

        header = std::move(decoded);
    }

    if (header.empty() || header[0] != ID_HEADER) {
        return fail(reader, broken("its header is broken"));
    }

    cursor_t cursor{ std::span(header).subspan(1) };
    const status_t read = read_header(cursor, reader);
    if (read.failed()) {
        reader.files.clear();
        return fail(reader, read);
    }

    for (std::size_t i = 0; i < reader.files.size(); ++i) {
        file_t& file = reader.files[i];
        if (file.kind != KIND_LINK || !file.has_stream) {
            continue;
        }

        std::vector<std::uint8_t> folder_bytes;
        if (decode(bytes, reader.streams, reader.streams.folders[file.folder], folder_bytes)
                .failed() ||
            file.offset + file.size > folder_bytes.size()) {
            continue;
        }

        file.target = fb::compress::utf8_or_replaced(
            { reinterpret_cast<const char*>(folder_bytes.data()) + file.offset,
              static_cast<std::size_t>(file.size) });
    }

    return RESULT_DONE;
}

reader_t& reader_of(void* reader) {
    return *static_cast<reader_t*>(reader);
}

const reader_t& reader_of(const void* reader) {
    return *static_cast<const reader_t*>(reader);
}

const file_t& file_of(const void* reader, const std::int64_t index) {
    return reader_of(reader).files[static_cast<std::size_t>(index)];
}

} // namespace

extern "C" {

void* fb_xz_7z_reader_new(const fb_array_t* data) {
    const std::span<const std::uint8_t> bytes = fb::compress::bytes_of(data);
    auto* reader = new reader_t{ .bytes = { bytes.begin(), bytes.end() } };
    reader->result = open(*reader);
    return reader;
}

std::int32_t fb_xz_7z_reader_result(const void* reader) {
    return reader_of(reader).result;
}

std::int64_t fb_xz_7z_reader_count(const void* reader) {
    return static_cast<std::int64_t>(reader_of(reader).files.size());
}

fb_string_t* fb_xz_7z_reader_name(const void* reader, const std::int64_t index) {
    return fb::compress::string_of(file_of(reader, index).name);
}

std::int64_t fb_xz_7z_reader_size(const void* reader, const std::int64_t index) {
    return static_cast<std::int64_t>(file_of(reader, index).size);
}

std::int32_t fb_xz_7z_reader_kind(const void* reader, const std::int64_t index) {
    return file_of(reader, index).kind;
}

fb_string_t* fb_xz_7z_reader_target(const void* reader, const std::int64_t index) {
    return fb::compress::string_of(file_of(reader, index).target);
}

std::int32_t fb_xz_7z_reader_read(void* reader, const std::int64_t index, fb_array_t* output) {
    reader_t& state = reader_of(reader);
    const file_t& file = file_of(reader, index);
    output->count = 0;
    if (!file.has_stream) {
        return RESULT_DONE;
    }

    if (file.size > static_cast<std::uint64_t>(most_bytes)) {
        return fail(state, { .result = RESULT_TOO_LARGE, .message = "" });
    }

    if (state.decoded != file.folder) {
        state.decoded.reset();
        const status_t decoded = decode(state.bytes, state.streams,
                                        state.streams.folders[file.folder], state.folder_bytes);
        if (decoded.failed()) {
            return fail(state, decoded);
        }

        state.decoded = file.folder;
    }

    if (file.offset + file.size > state.folder_bytes.size()) {
        return fail(state, broken("a file is past its folder's end"));
    }

    const std::span<const std::uint8_t> bytes =
        std::span(state.folder_bytes).subspan(file.offset, file.size);
    if (file.crc && lzma_crc32(bytes.data(), bytes.size(), 0) != *file.crc) {
        return fail(state, broken("'" + file.name + "' doesn't match its CRC-32"));
    }

    output_t out(output, most_bytes);
    out.add_all(bytes);
    return RESULT_DONE;
}

fb_string_t* fb_xz_7z_reader_message(const void* reader) {
    return fb::compress::string_of(reader_of(reader).message);
}

void fb_xz_7z_reader_free(void* reader) {
    delete static_cast<reader_t*>(reader);
}
}
