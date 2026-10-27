// std.compress's Zip, over minizip-ng with memory streams: a reader of a zip's entries, and a
// writer of new ones, stored or by Deflate, LZMA, XZ, or Zstd.

#include "compress_internal.hpp"
#include "fb/compress.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <lzma.h>
#include <mz.h>
#include <mz_crypt.h>
#include <mz_os.h>
#include <mz_strm.h>
#include <mz_strm_mem.h>
#include <mz_zip.h>
}

using fb::compress::chunk_size;
using fb::compress::KIND_FILE;
using fb::compress::KIND_FOLDER;
using fb::compress::KIND_LINK;
using fb::compress::moment_t;
using fb::compress::most_bytes;
using fb::compress::output_t;
using fb::compress::RESULT_DONE;
using fb::compress::RESULT_INVALID;
using fb::compress::RESULT_TOO_LARGE;
using fb::compress::RESULT_UNSUPPORTED;

namespace {

/// What code page 437, which a zip's names are in when they don't say they're UTF-8, has from
/// 0x80 on, as Unicode.
constexpr std::array<char16_t, 128> cp437 = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8, 0x00ef,
    0x00ee, 0x00ec, 0x00c4, 0x00c5, 0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
    0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192, 0x00e1, 0x00ed, 0x00f3, 0x00fa,
    0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255d, 0x255c, 0x255b, 0x2510, 0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256b, 0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4,
    0x221e, 0x03c6, 0x03b5, 0x2229, 0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
    0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

/// The methods as std.compress numbers them, from 0: stored, Deflate, LZMA, XZ, and Zstd.
constexpr std::array<std::uint16_t, 5> methods = {
    MZ_COMPRESS_METHOD_STORE, MZ_COMPRESS_METHOD_DEFLATE, MZ_COMPRESS_METHOD_LZMA,
    MZ_COMPRESS_METHOD_XZ,    MZ_COMPRESS_METHOD_ZSTD,
};

/// Made by Unix, by version 6.3 of the format, which has every method above.
constexpr std::uint16_t made_by = (static_cast<std::uint32_t>(MZ_HOST_SYSTEM_UNIX) << 8U) | 63U;

/// The Unix mode's bits that say a file and a folder are, which go in the top half of an
/// entry's external attributes, with its permissions, and MS-DOS's folder bit, in the bottom half.
constexpr std::uint32_t mode_file = 0100000U;
constexpr std::uint32_t mode_folder = 040000U;
constexpr std::uint32_t dos_folder = 0x10U;
/// A Unix mode's bits that say what kind of file it is, and a link's.
constexpr std::uint32_t mode_kind = 0170000U;
constexpr std::uint32_t mode_link = 0120000U;

/// A name as UTF-8: as it is, when it says it is or is well-formed UTF-8 anyway, as many tools
/// write without saying, or else read as code page 437.
std::string name_of(const std::string_view name, const std::uint16_t flag) {
    if ((flag & MZ_ZIP_FLAG_UTF8) != 0 || fb::compress::is_utf8(name)) {
        return fb::compress::utf8_or_replaced(name);
    }

    std::string made;
    for (const char byte : name) {
        const auto code = static_cast<std::uint8_t>(byte);
        fb::compress::append_utf8(made, code < 0x80 ? code : cp437[code - 0x80]);
    }

    return made;
}

/// The first and last moments a DOS date and time hold, 1980-01-01T00:00:00 and
/// 2107-12-31T23:59:58, which a zip's entries are dated in.
constexpr std::int64_t first_dos = 315532800;
constexpr std::int64_t last_dos = 4354819198;
/// The seconds from 1601, which NTFS counts its times from, to 1970.
constexpr std::int64_t ntfs_epoch = 11644473600;

/// The number in `bytes`, little-endian, as a zip's are.
std::uint64_t little_endian(const std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = bytes.size(); i-- > 0;) {
        value = (value << 8U) | bytes[i];
    }

    return value;
}

/// When an entry was last changed, by its extra fields, which say it in UTC: NTFS's, to 100
/// nanoseconds, then Info-ZIP's extended timestamp, then PKWARE's Unix field, to the second, each
/// read unsigned, as Go reads them, to 2106. Nothing when none of them has one.
std::optional<moment_t> extra_time(const std::span<const std::uint8_t> extra) {
    std::optional<moment_t> ntfs;
    std::optional<moment_t> extended;
    std::optional<moment_t> unix;
    for (std::size_t at = 0; at + 4 <= extra.size();) {
        const std::uint64_t id = little_endian(extra.subspan(at, 2));
        const std::uint64_t size = little_endian(extra.subspan(at + 2, 2));
        if (at + 4 + size > extra.size()) {
            break;
        }

        const std::span<const std::uint8_t> field = extra.subspan(at + 4, size);
        at += 4 + size;
        if (id == 0x000a) {
            // Four reserved bytes, then attributes, each a tag, a size, and its bytes: tag 1's are
            // three times in 100 nanoseconds since 1601, the last change's first.
            for (std::size_t in = 4; in + 4 <= field.size();) {
                const std::uint64_t tag = little_endian(field.subspan(in, 2));
                const std::uint64_t length = little_endian(field.subspan(in + 2, 2));
                if (in + 4 + length > field.size()) {
                    break;
                }

                if (tag == 1 && length >= 8) {
                    const std::uint64_t ticks = little_endian(field.subspan(in + 4, 8));
                    ntfs = moment_t{
                        .seconds = static_cast<std::int64_t>(ticks / 10000000) - ntfs_epoch,
                        .nanos = static_cast<std::int32_t>(ticks % 10000000) * 100,
                    };
                }

                in += 4 + length;
            }
        } else if (id == 0x5455 && field.size() >= 5 && (field[0] & 1U) != 0) {
            // Flags, whose first bit says the last change's time is next.
            extended = moment_t{
                .seconds = static_cast<std::int64_t>(little_endian(field.subspan(1, 4))),
                .nanos = 0,
            };
        } else if (id == 0x000d && field.size() >= 8) {
            // The time it was last read, then last changed.
            unix = moment_t{
                .seconds = static_cast<std::int64_t>(little_endian(field.subspan(4, 4))),
                .nanos = 0,
            };
        }
    }

    if (ntfs) {
        return ntfs;
    }

    return extended ? extended : unix;
}

/// The moment a DOS date and time say, as `(date << 16) | time`, read as UTC, as nothing says
/// their zone, or nothing when the date isn't one.
std::optional<moment_t> dos_time(const std::uint32_t dos) {
    const std::uint32_t day = (dos >> 16U) & 31U;
    const std::uint32_t month = (dos >> 21U) & 15U;
    if (day == 0 || month == 0 || month > 12) {
        return std::nullopt;
    }

    const std::chrono::sys_days date =
        std::chrono::year_month_day(std::chrono::year(static_cast<int>(1980 + (dos >> 25U))),
                                    std::chrono::month(month), std::chrono::day(day));
    const std::int64_t clock =
        (((dos >> 11U) & 31U) * 3600) + (((dos >> 5U) & 63U) * 60) + ((dos & 31U) * 2);
    const std::int64_t days = date.time_since_epoch().count();
    return moment_t{ .seconds = (days * 86400) + clock, .nanos = 0 };
}

/// The DOS date and time of `seconds`' UTC, as `(date << 16) | time`, kept from 1980 to 2107,
/// which they hold, and rounded down to an even second, as they are.
std::uint32_t dos_of(const std::int64_t seconds) {
    const std::chrono::sys_seconds kept{ std::chrono::seconds(
        std::clamp(seconds, first_dos, last_dos)) };
    const std::chrono::sys_days days = std::chrono::floor<std::chrono::days>(kept);
    const std::chrono::year_month_day date(days);
    const std::chrono::hh_mm_ss clock(kept - days);
    const auto year = static_cast<std::uint32_t>(static_cast<int>(date.year()) - 1980);
    const std::uint32_t day = (year << 9U) | (static_cast<unsigned>(date.month()) << 5U) |
                              static_cast<unsigned>(date.day());
    const std::uint32_t time = (static_cast<std::uint32_t>(clock.hours().count()) << 11U) |
                               (static_cast<std::uint32_t>(clock.minutes().count()) << 5U) |
                               (static_cast<std::uint32_t>(clock.seconds().count()) / 2);
    return (day << 16U) | time;
}

/// The DOS date and time of the central directory record at `at` in `bytes`, as
/// `(date << 16) | time`, or 0, which isn't a date, when there's no record there. minizip-ng
/// gives them as a time it made of them in local time, which doesn't hold one an hour of summer
/// time skips, so they're read from the record itself.
std::uint32_t dos_at(const std::span<const std::uint8_t> bytes, const std::int64_t at) {
    if (at < 0 || std::cmp_greater(at + 16, bytes.size())) {
        return 0;
    }

    const std::span<const std::uint8_t> record = bytes.subspan(static_cast<std::size_t>(at), 16);
    if (little_endian(record.first(4)) != 0x02014b50) {
        return 0;
    }

    return static_cast<std::uint32_t>((little_endian(record.subspan(14, 2)) << 16U) |
                                      little_endian(record.subspan(12, 2)));
}

/// Puts `value` in `bytes` at `at`, little-endian, in `size` bytes.
void put_little_endian(std::vector<std::uint8_t>& bytes, const std::size_t at, std::uint64_t value,
                       const std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}

struct entry_t {
    std::string name;
    std::int64_t size;
    std::int32_t kind;
    std::string target;
    /// When it was last changed, and its Unix permissions, or -1 when the zip doesn't say.
    std::optional<moment_t> modified;
    std::int32_t mode;
    /// Where its record is in the central directory.
    std::int64_t position;
};

struct reader_t {
    std::vector<std::uint8_t> bytes;
    void* stream = nullptr;
    void* zip = nullptr;
    std::vector<entry_t> entries;
    std::int32_t result = RESULT_DONE;
    std::string message;
};

/// A stream that writes a zip into a vector, which grows as vectors do: minizip-ng's memory
/// stream grows by the same size each time, so writing a zip through it takes time in the square
/// of the zip's size. minizip-ng calls it through its table, with the vector_t's address.
struct vector_t {
    mz_stream stream;
    std::vector<std::uint8_t>* bytes;
    std::int64_t position;
};

vector_t& vector_of(void* stream) {
    return *static_cast<vector_t*>(stream);
}

std::int32_t vector_open(void* /*stream*/, const char* /*path*/, const std::int32_t /*mode*/) {
    return MZ_OK;
}

std::int32_t vector_is_open(void* /*stream*/) {
    return MZ_OK;
}

std::int32_t vector_read(void* stream, void* buffer, const std::int32_t size) {
    vector_t& vector = vector_of(stream);
    const auto left = static_cast<std::int64_t>(vector.bytes->size()) - vector.position;
    const auto count = static_cast<std::int32_t>(std::clamp<std::int64_t>(left, 0, size));
    std::memcpy(buffer, vector.bytes->data() + vector.position, static_cast<std::size_t>(count));
    vector.position += count;
    return count;
}

std::int32_t vector_write(void* stream, const void* buffer, const std::int32_t size) {
    vector_t& vector = vector_of(stream);
    const auto end = static_cast<std::size_t>(vector.position + size);
    if (end > vector.bytes->size()) {
        vector.bytes->resize(end);
    }

    std::memcpy(vector.bytes->data() + vector.position, buffer, static_cast<std::size_t>(size));
    vector.position += size;
    return size;
}

std::int64_t vector_tell(void* stream) {
    return vector_of(stream).position;
}

std::int32_t vector_seek(void* stream, const std::int64_t offset, const std::int32_t origin) {
    vector_t& vector = vector_of(stream);
    std::int64_t position = offset;
    if (origin == MZ_SEEK_CUR) {
        position += vector.position;
    } else if (origin == MZ_SEEK_END) {
        position += static_cast<std::int64_t>(vector.bytes->size());
    }

    if (position < 0) {
        return MZ_SEEK_ERROR;
    }

    if (std::cmp_greater(position, vector.bytes->size())) {
        vector.bytes->resize(static_cast<std::size_t>(position));
    }
    vector.position = position;
    return MZ_OK;
}

std::int32_t vector_close(void* /*stream*/) {
    return MZ_OK;
}

std::int32_t vector_error(void* /*stream*/) {
    return MZ_OK;
}

mz_stream_vtbl vector_table{
    .open = vector_open,
    .is_open = vector_is_open,
    .read = vector_read,
    .write = vector_write,
    .tell = vector_tell,
    .seek = vector_seek,
    .close = vector_close,
    .error = vector_error,
    .create = nullptr,
    .destroy = nullptr,
    .get_prop_int64 = nullptr,
    .set_prop_int64 = nullptr,
};

struct writer_t {
    std::vector<std::uint8_t> bytes;
    vector_t stream{};
    void* zip = nullptr;
    /// Each entry's DOS date and time, in the order they're added.
    std::vector<std::uint32_t> dates;
};

/// Puts each entry's DOS date and time in the finished zip, in its local header and its central
/// directory record: minizip-ng writes a time's local date and time, which can't be one an hour of
/// summer time skips, so the same entries make the same bytes in every time zone. One of more
/// bytes than a UInt8[] holds is too large to give anyway, and is left as it is.
void put_dates(writer_t& writer) {
    std::vector<std::uint8_t>& bytes = writer.bytes;
    const std::span<const std::uint8_t> all(bytes);
    if (bytes.size() < 22 || std::cmp_greater(bytes.size(), most_bytes)) {
        return;
    }

    // The end of the central directory, which has no comment, says where it starts, unless it's
    // Zip64's, as many entries make it, whose locator is before it.
    const std::size_t end = bytes.size() - 22;
    if (little_endian(all.subspan(end, 4)) != 0x06054b50) {
        return;
    }

    std::uint64_t at = little_endian(all.subspan(end + 16, 4));
    if (at == UINT32_MAX) {
        if (end < 20 || little_endian(all.subspan(end - 20, 4)) != 0x07064b50) {
            return;
        }

        const std::uint64_t zip64 = little_endian(all.subspan(end - 12, 8));
        if (zip64 + 56 > end || little_endian(all.subspan(zip64, 4)) != 0x06064b50) {
            return;
        }

        at = little_endian(all.subspan(zip64 + 48, 8));
    }

    for (const std::uint32_t dos : writer.dates) {
        if (at + 46 > bytes.size() || little_endian(all.subspan(at, 4)) != 0x02014b50) {
            return;
        }

        put_little_endian(bytes, at + 12, dos, 4);
        const std::uint64_t local = little_endian(all.subspan(at + 42, 4));
        if (local + 30 <= bytes.size() && little_endian(all.subspan(local, 4)) == 0x04034b50) {
            put_little_endian(bytes, local + 10, dos, 4);
        }

        at += 46 + little_endian(all.subspan(at + 28, 2)) + little_endian(all.subspan(at + 30, 2)) +
              little_endian(all.subspan(at + 32, 2));
    }
}

std::int32_t fail(reader_t& reader, const std::int32_t result, const std::string_view message) {
    reader.message = message;
    return result;
}

/// Frees the options lzma_properties_decode gives once they go.
struct options_t {
    void* options = nullptr;

    options_t() = default;
    options_t(const options_t&) = delete;
    options_t& operator=(const options_t&) = delete;
    options_t(options_t&&) = delete;
    options_t& operator=(options_t&&) = delete;

    ~options_t() {
        std::free(options);
    }
};

/// Decodes an LZMA or XZ entry's raw bytes with liblzma, giving what comes out to `give`: a
/// result_t. Its dictionary takes no more memory than an array holds, which minizip-ng doesn't
/// ask, as XZ.Decompress does. A zip's LZMA has 2 bytes of the LZMA SDK's version and 2 of its
/// properties' size before its 5 properties, and may end with LZMA's end marker or at its size.
template <typename Give>
std::int32_t decode_lzma(reader_t& reader, const std::string& name, const std::uint16_t method,
                         const std::int64_t size, const std::span<const std::uint8_t> raw,
                         const Give& give) {
    lzma_stream stream = LZMA_STREAM_INIT;
    std::span<const std::uint8_t> data = raw;
    options_t options;
    lzma_ret started = LZMA_OK;
    if (method == MZ_COMPRESS_METHOD_LZMA) {
        lzma_filter lzma1{ .id = LZMA_FILTER_LZMA1, .options = nullptr };
        if (raw.size() < 9 || raw[2] != 5 || raw[3] != 0 ||
            lzma_properties_decode(&lzma1, nullptr, raw.data() + 4, 5) != LZMA_OK) {
            return fail(reader, RESULT_INVALID, "'" + name + "' is broken");
        }

        options.options = lzma1.options;
        auto* lzma = static_cast<lzma_options_lzma*>(lzma1.options);
        lzma->ext_flags = LZMA_LZMA1EXT_ALLOW_EOPM;
        lzma->ext_size_low = static_cast<std::uint32_t>(size);
        lzma->ext_size_high = static_cast<std::uint32_t>(static_cast<std::uint64_t>(size) >> 32U);
        const std::array<lzma_filter, 2> filters{
            lzma_filter{ .id = LZMA_FILTER_LZMA1EXT, .options = lzma },
            lzma_filter{ .id = LZMA_VLI_UNKNOWN, .options = nullptr },
        };
        if (lzma_raw_decoder_memusage(filters.data()) > static_cast<std::uint64_t>(most_bytes)) {
            return fail(reader, RESULT_UNSUPPORTED,
                        "'" + name + "' needs more memory than an array holds to decompress");
        }

        started = lzma_raw_decoder(&stream, filters.data());
        data = raw.subspan(9);
    } else {
        started = lzma_stream_decoder(&stream, static_cast<std::uint64_t>(most_bytes), 0);
    }

    if (started == LZMA_MEM_ERROR) {
        fb_panic("out of memory", nullptr);
    }

    if (started != LZMA_OK) {
        return fail(reader, RESULT_UNSUPPORTED, "'" + name + "' has options liblzma lacks");
    }

    std::vector<std::uint8_t> chunk(chunk_size);
    stream.next_in = data.data();
    stream.avail_in = data.size();
    lzma_ret status = LZMA_OK;
    std::int32_t result = RESULT_DONE;
    while (status == LZMA_OK && result == RESULT_DONE) {
        stream.next_out = chunk.data();
        stream.avail_out = chunk.size();
        status = lzma_code(&stream, LZMA_FINISH);
        const std::size_t made = chunk.size() - stream.avail_out;
        if (!give(std::span<const std::uint8_t>(chunk).first(made))) {
            result =
                fail(reader, RESULT_INVALID, "'" + name + "' has more bytes than its size says");
        }
    }

    lzma_end(&stream);
    if (result != RESULT_DONE) {
        return result;
    }

    if (status == LZMA_MEM_ERROR) {
        fb_panic("out of memory", nullptr);
    }

    if (status == LZMA_MEMLIMIT_ERROR) {
        return fail(reader, RESULT_UNSUPPORTED,
                    "'" + name + "' needs more memory than an array holds to decompress");
    }

    if (status != LZMA_STREAM_END) {
        return fail(reader, RESULT_INVALID, "'" + name + "' is broken");
    }

    return RESULT_DONE;
}

/// Reads the entry at `index`, giving its bytes to `add` a chunk at a time, which gives false when
/// they'd go past its size: a result_t. Every entry is checked against its CRC-32, which
/// minizip-ng checks only of a stream that took all its compressed bytes.
template <typename Add>
std::int32_t read_entry(reader_t& reader, const std::size_t index, const Add& add) {
    const entry_t& entry = reader.entries[index];
    mz_zip_file* info = nullptr;
    if (mz_zip_goto_entry(reader.zip, entry.position) != MZ_OK ||
        mz_zip_entry_get_info(reader.zip, &info) != MZ_OK) {
        return fail(reader, RESULT_INVALID, "its central directory is broken");
    }

    if ((info->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0) {
        return fail(reader, RESULT_UNSUPPORTED, "'" + entry.name + "' is encrypted");
    }

    if (entry.size > most_bytes) {
        return fail(reader, RESULT_TOO_LARGE, "");
    }

    const std::uint16_t method = info->compression_method;
    const std::uint32_t crc = info->crc;
    // An LZMA or XZ entry is read raw, and decoded here.
    const bool lzma = method == MZ_COMPRESS_METHOD_LZMA || method == MZ_COMPRESS_METHOD_XZ;
    const std::int32_t opened = mz_zip_entry_read_open(reader.zip, lzma ? 1 : 0, nullptr);
    if (opened == MZ_SUPPORT_ERROR) {
        return fail(reader, RESULT_UNSUPPORTED,
                    "'" + entry.name + "' is compressed by method " + std::to_string(method));
    }

    if (opened != MZ_OK) {
        return fail(reader, RESULT_INVALID, "'" + entry.name + "' is broken");
    }

    std::uint32_t made_crc = 0;
    std::int64_t total = 0;
    const auto give = [&](const std::span<const std::uint8_t> bytes) {
        made_crc =
            mz_crypt_crc32_update(made_crc, bytes.data(), static_cast<std::int32_t>(bytes.size()));
        total += static_cast<std::int64_t>(bytes.size());
        return add(bytes);
    };
    std::vector<std::uint8_t> chunk(chunk_size);
    std::vector<std::uint8_t> raw;
    std::int32_t result = RESULT_DONE;
    while (result == RESULT_DONE) {
        const std::int32_t read =
            mz_zip_entry_read(reader.zip, chunk.data(), static_cast<std::int32_t>(chunk.size()));
        if (read == 0) {
            break;
        }

        if (read < 0) {
            result = fail(reader, RESULT_INVALID, "'" + entry.name + "' is broken");
            break;
        }

        const auto bytes =
            std::span<const std::uint8_t>(chunk).first(static_cast<std::size_t>(read));
        if (lzma) {
            raw.insert(raw.end(), bytes.begin(), bytes.end());
        } else if (!give(bytes)) {
            result = fail(reader, RESULT_INVALID,
                          "'" + entry.name + "' has more bytes than its size says");
        }
    }

    const std::int32_t closed = mz_zip_entry_close(reader.zip);
    if (result == RESULT_DONE && lzma) {
        result = decode_lzma(reader, entry.name, method, entry.size, raw, give);
    }

    if (result != RESULT_DONE) {
        return result;
    }

    if (closed == MZ_CRC_ERROR || (closed == MZ_OK && total == entry.size && made_crc != crc)) {
        return fail(reader, RESULT_INVALID, "'" + entry.name + "' doesn't match its CRC-32");
    }

    if (closed != MZ_OK || total != entry.size) {
        return fail(reader, RESULT_INVALID,
                    "'" + entry.name + "' has fewer bytes than its size says");
    }

    return RESULT_DONE;
}

/// Opens the zip in the reader's bytes, and lists its entries: a result_t.
std::int32_t open(reader_t& reader) {
    reader.stream = mz_stream_mem_create();
    mz_stream_mem_set_buffer(reader.stream, reader.bytes.data(),
                             static_cast<std::int32_t>(reader.bytes.size()));
    reader.zip = mz_zip_create();
    if (mz_stream_open(reader.stream, nullptr, MZ_OPEN_MODE_READ) != MZ_OK ||
        mz_zip_open(reader.zip, reader.stream, MZ_OPEN_MODE_READ) != MZ_OK) {
        return fail(reader, RESULT_INVALID, "it has no central directory");
    }

    std::int32_t at = mz_zip_goto_first_entry(reader.zip);
    for (; at == MZ_OK; at = mz_zip_goto_next_entry(reader.zip)) {
        mz_zip_file* info = nullptr;
        if (mz_zip_entry_get_info(reader.zip, &info) != MZ_OK) {
            break;
        }

        // A link is Unix's, macOS's, or RISC OS's, by its mode, or one an extra field names the
        // target of: MS-DOS's and Windows' attributes have none, though minizip-ng takes Windows'
        // reparse points, as OneDrive's files are, for links.
        std::int32_t kind = KIND_FILE;
        const std::uint8_t host = MZ_HOST_SYSTEM(info->version_madeby);
        const bool unix_like = host == MZ_HOST_SYSTEM_UNIX || host == MZ_HOST_SYSTEM_OSX_DARWIN ||
                               host == MZ_HOST_SYSTEM_RISCOS;
        if (mz_zip_entry_is_dir(reader.zip) == MZ_OK) {
            kind = KIND_FOLDER;
        } else if ((unix_like && ((info->external_fa >> 16U) & mode_kind) == mode_link) ||
                   (info->linkname != nullptr && info->linkname[0] != 0)) {
            kind = KIND_LINK;
        }

        // A folder's name is given without the '/' after it, as every archive's are.
        std::string name = name_of({ info->filename, info->filename_size }, info->flag);
        while (kind == KIND_FOLDER && name.size() > 1 && mz_os_is_dir_separator(name.back()) != 0) {
            name.pop_back();
        }

        // Its time is UTC in an extra field, or else its DOS date's, read as UTC. A Unix-like
        // OS's entry has a Unix mode.
        const std::int64_t position = mz_zip_get_entry(reader.zip);
        std::optional<moment_t> modified =
            extra_time({ info->extrafield, static_cast<std::size_t>(info->extrafield_size) });
        if (!modified) {
            modified = dos_time(dos_at(reader.bytes, position));
        }

        const std::uint32_t mode = info->external_fa >> 16U;
        reader.entries.push_back({
            .name = std::move(name),
            .size = kind == KIND_FOLDER ? 0 : info->uncompressed_size,
            .kind = kind,
            .target = info->linkname != nullptr ? info->linkname : "",
            .modified = modified,
            .mode = unix_like && mode != 0 ? static_cast<std::int32_t>(mode & 07777U) : -1,
            .position = position,
        });
    }

    if (at != MZ_END_OF_LIST) {
        return fail(reader, RESULT_INVALID, "its central directory is broken");
    }

    // A link's target is its data, unless an extra field gave it.
    for (std::size_t i = 0; i < reader.entries.size(); ++i) {
        entry_t& entry = reader.entries[i];
        if (entry.kind != KIND_LINK || !entry.target.empty()) {
            continue;
        }

        std::string target;
        const auto add = [&](const std::span<const std::uint8_t> bytes) {
            if (std::cmp_greater(target.size() + bytes.size(), entry.size)) {
                return false;
            }

            target.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return true;
        };
        if (read_entry(reader, i, add) == RESULT_DONE) {
            entry.target = fb::compress::utf8_or_replaced(target);
        }
    }

    reader.message.clear();
    return RESULT_DONE;
}

void start(writer_t& writer) {
    writer.bytes.clear();
    writer.dates.clear();
    writer.stream = {
        .stream = { .vtbl = &vector_table, .base = nullptr },
        .bytes = &writer.bytes,
        .position = 0,
    };
    writer.zip = mz_zip_create();
    // Each local header has its entry's sizes and CRC, which minizip-ng goes back to write, as
    // the stream can seek, so it needs no data descriptor after the entry.
    mz_zip_set_data_descriptor(writer.zip, 0);
    if (mz_zip_open(writer.zip, &writer.stream, MZ_OPEN_MODE_WRITE) != MZ_OK) {
        fb_panic("std.compress's Zip couldn't start a zip", nullptr);
    }
}

void stop(writer_t& writer) {
    mz_zip_delete(&writer.zip);
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

// What minizip-ng's zip code asks of the OS layer it isn't built with, the same on every OS: zip's
// names end a folder's with '/', or '\' as Windows' tools may write it, and its encryption, which
// alone wants random bytes, isn't built.
int32_t mz_os_is_dir_separator(const char c) {
    return c == '/' || c == '\\' ? 1 : 0;
}

int32_t mz_os_rand(std::uint8_t* /*buf*/, const int32_t /*size*/) {
    return 0;
}

void* fb_zip_reader_new(const fb_array_t* data) {
    const std::span<const std::uint8_t> bytes = fb::compress::bytes_of(data);
    auto* reader = new reader_t{ .bytes = { bytes.begin(), bytes.end() } };
    reader->result = open(*reader);
    return reader;
}

std::int32_t fb_zip_reader_result(const void* reader) {
    return reader_of(reader).result;
}

std::int64_t fb_zip_reader_count(const void* reader) {
    return static_cast<std::int64_t>(reader_of(reader).entries.size());
}

fb_string_t* fb_zip_reader_name(const void* reader, const std::int64_t index) {
    return fb::compress::string_of(entry_of(reader, index).name);
}

std::int64_t fb_zip_reader_size(const void* reader, const std::int64_t index) {
    return entry_of(reader, index).size;
}

std::int32_t fb_zip_reader_kind(const void* reader, const std::int64_t index) {
    return entry_of(reader, index).kind;
}

fb_string_t* fb_zip_reader_target(const void* reader, const std::int64_t index) {
    return fb::compress::string_of(entry_of(reader, index).target);
}

std::int64_t fb_zip_reader_seconds(const void* reader, const std::int64_t index) {
    const std::optional<moment_t>& modified = entry_of(reader, index).modified;
    return modified ? modified->seconds : 0;
}

std::int32_t fb_zip_reader_nanos(const void* reader, const std::int64_t index) {
    const std::optional<moment_t>& modified = entry_of(reader, index).modified;
    return modified ? modified->nanos : -1;
}

std::int32_t fb_zip_reader_mode(const void* reader, const std::int64_t index) {
    return entry_of(reader, index).mode;
}

std::int32_t fb_zip_reader_read(void* reader, const std::int64_t index, fb_array_t* output) {
    reader_t& state = reader_of(reader);
    output_t out(output, entry_of(reader, index).size);
    return read_entry(state, static_cast<std::size_t>(index),
                      [&](const std::span<const std::uint8_t> bytes) { return out.add(bytes); });
}

fb_string_t* fb_zip_reader_message(const void* reader) {
    return fb::compress::string_of(reader_of(reader).message);
}

void fb_zip_reader_free(void* reader) {
    auto* state = static_cast<reader_t*>(reader);
    if (state->zip != nullptr) {
        mz_zip_close(state->zip);
        mz_zip_delete(&state->zip);
    }

    if (state->stream != nullptr) {
        mz_stream_mem_delete(&state->stream);
    }

    delete state;
}

void* fb_zip_writer_new() {
    auto* writer = new writer_t;
    start(*writer);
    return writer;
}

void fb_zip_writer_add(void* writer, const fb_string_t* name, const fb_array_t* data,
                       const std::int32_t method, const std::int32_t level,
                       const std::int32_t folder, const std::int32_t timed,
                       const std::int64_t seconds, const std::int32_t mode) {
    auto& state = *static_cast<writer_t*>(writer);
    const std::string filename(fb::compress::text_of(name));
    const bool ascii = std::ranges::all_of(
        filename, [](const char byte) { return static_cast<std::uint8_t>(byte) < 0x80; });
    mz_zip_file info{};
    info.version_madeby = made_by;
    info.flag = ascii ? 0 : MZ_ZIP_FLAG_UTF8;
    info.compression_method =
        folder != 0 ? MZ_COMPRESS_METHOD_STORE : methods[static_cast<std::size_t>(method)];
    // A time is its DOS date's, which put_dates() writes, and, where it's from 1970 to 2106,
    // exact to the second in Info-ZIP's extended timestamp, which tools read as UTC.
    state.dates.push_back(dos_of(timed != 0 ? seconds : first_dos));
    info.modified_date = first_dos;
    std::array<std::uint8_t, 9> extended{ 0x55, 0x54, 5, 0, 1 };
    if (timed != 0 && seconds >= 0 && std::cmp_less_equal(seconds, UINT32_MAX)) {
        for (std::size_t i = 0; i < 4; ++i) {
            extended.at(5 + i) =
                static_cast<std::uint8_t>(static_cast<std::uint64_t>(seconds) >> (8U * i));
        }

        info.extrafield = extended.data();
        info.extrafield_size = extended.size();
    }

    info.filename = filename.c_str();
    std::uint32_t permissions = folder != 0 ? 0755U : 0644U;
    if (mode >= 0) {
        permissions = static_cast<std::uint32_t>(mode);
    }

    info.external_fa = folder != 0 ? ((mode_folder | permissions) << 16U) | dos_folder
                                   : (mode_file | permissions) << 16U;
    // Its size, known first, lets minizip-ng leave Zip64 out, which no UInt8[]'s sizes need.
    info.uncompressed_size = data->count;
    info.zip64 = MZ_ZIP64_AUTO;
    if (mz_zip_entry_write_open(state.zip, &info, static_cast<std::int16_t>(level), 0, nullptr) !=
        MZ_OK) {
        fb_panic("std.compress's Zip couldn't add an entry", nullptr);
    }

    const std::span<const std::uint8_t> bytes = fb::compress::bytes_of(data);
    for (std::size_t at = 0; at < bytes.size(); at += chunk_size) {
        const std::size_t size = std::min(chunk_size, bytes.size() - at);
        if (mz_zip_entry_write(state.zip, bytes.data() + at, static_cast<std::int32_t>(size)) !=
            static_cast<std::int32_t>(size)) {
            fb_panic("std.compress's Zip couldn't write an entry", nullptr);
        }
    }

    if (mz_zip_entry_close(state.zip) != MZ_OK) {
        fb_panic("std.compress's Zip couldn't finish an entry", nullptr);
    }
}

std::int32_t fb_zip_writer_finish(void* writer, fb_array_t* output, const std::int64_t limit) {
    auto& state = *static_cast<writer_t*>(writer);
    if (mz_zip_close(state.zip) != MZ_OK) {
        fb_panic("std.compress's Zip couldn't finish a zip", nullptr);
    }

    put_dates(state);
    output_t out(output, limit);
    const bool fits = out.add(state.bytes);
    stop(state);
    start(state);
    return fits ? RESULT_DONE : RESULT_TOO_LARGE;
}

void fb_zip_writer_free(void* writer) {
    auto* state = static_cast<writer_t*>(writer);
    mz_zip_close(state->zip);
    stop(*state);
    delete state;
}
}
