#include "fb/package_archive.hpp"

#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <psa/crypto.h>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utf8proc.h>
#include <utility>
#include <vector>
#include <zlib.h>

namespace fb {
namespace {

constexpr std::size_t block = 512;

/// What a partial folder's name has after the folder it's for.
constexpr std::string_view partial_mark = ".~";

/// Where a ustar header's fields are, and how long each is.
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
constexpr field_t sum_field{ .at = 148, .size = 8 };
constexpr std::size_t type_at = 156;
constexpr field_t magic_field{ .at = 257, .size = 6 };
constexpr field_t version_field{ .at = 263, .size = 2 };
constexpr field_t prefix_field{ .at = 345, .size = 155 };

/// Writes `value` into a header's field as octal digits and a NUL, as ustar has numbers.
void put_octal(std::string& header, const field_t field, std::uint64_t value) {
    std::string digits(field.size - 1, '0');
    for (std::size_t i = digits.size(); i-- > 0 && value > 0;) {
        digits[i] = static_cast<char>('0' + (value & 7U));
        value >>= 3U;
    }

    header.replace(field.at, digits.size(), digits);
}

void put_text(std::string& header, const field_t field, const std::string_view text) {
    header.replace(field.at, text.size(), text);
}

/// A header's field as text, up to its first NUL.
std::string_view text_of(const std::string_view header, const field_t field) {
    const std::string_view text = header.substr(field.at, field.size);
    return text.substr(0, text.find('\0'));
}

/// A header's octal field, as the registry reads it: spaces, octal digits, and then only NULs or
/// spaces to its end. nullopt for anything else, like base-256, an empty field, or a NUL first,
/// which extractors read differently.
std::optional<std::uint64_t> octal_of(const std::string_view header, const field_t field) {
    const std::string_view text = header.substr(field.at, field.size);
    std::size_t at = 0;
    while (at < text.size() && text[at] == ' ') {
        ++at;
    }

    std::uint64_t value = 0;
    const std::size_t digits = at;
    for (; at < text.size() && text[at] >= '0' && text[at] <= '7'; ++at) {
        value = (value << 3U) | static_cast<std::uint64_t>(text[at] - '0');
    }

    if (at == digits ||
        !std::ranges::all_of(text.substr(at), [](const char c) { return c == '\0' || c == ' '; })) {
        return std::nullopt;
    }

    return value;
}

/// The header's checksum: the sum of its bytes, its own field's taken as spaces.
std::uint64_t checksum_of(const std::string_view header) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < block; ++i) {
        const bool own = i >= sum_field.at && i < sum_field.at + sum_field.size;
        sum += own ? static_cast<unsigned char>(' ') : static_cast<unsigned char>(header[i]);
    }

    return sum;
}

/// A ustar header of `type` for a file of `size` bytes, its path split into the prefix and name
/// fields.
std::string header_of(const std::string_view prefix, const std::string_view name,
                      const std::uint64_t size, const char type) {
    std::string header(block, '\0');
    put_text(header, name_field, name);
    put_octal(header, mode_field, 0644);
    put_octal(header, uid_field, 0);
    put_octal(header, gid_field, 0);
    put_octal(header, size_field, size);
    put_octal(header, time_field, 0);
    header[type_at] = type;
    put_text(header, magic_field, std::string_view("ustar\0", 6));
    put_text(header, version_field, "00");
    put_text(header, prefix_field, prefix);
    // Six digits, a NUL, and a space.
    const std::uint64_t sum = checksum_of(header);
    put_octal(header, { .at = sum_field.at, .size = 7 }, sum);
    header[sum_field.at + 7] = ' ';
    return header;
}

/// `bytes` and the zeros that fill its last block.
std::string padded(const std::string_view bytes) {
    std::string out(bytes);
    out.append((block - (bytes.size() % block)) % block, '\0');
    return out;
}

/// A file's headers: its ustar header, with its path split into the prefix and name fields at a
/// '/', or when it can't be, a pax header holding its path before it.
std::string headers_of(const archive_file_t& file) {
    const std::string_view path = file.path;
    if (path.size() <= name_field.size) {
        return header_of("", path, file.bytes.size(), '0');
    }

    // The first '/' that leaves a name that fits.
    for (std::size_t at = path.find('/'); at != std::string_view::npos;
         at = path.find('/', at + 1)) {
        if (path.size() - at - 1 <= name_field.size && at <= prefix_field.size) {
            return header_of(path.substr(0, at), path.substr(at + 1), file.bytes.size(), '0');
        }
    }

    // A pax record is "<length> path=<path>\n", its length counting its own digits.
    const std::string record = std::format(" path={}\n", path);
    std::size_t length = record.size() + 1;
    while (std::format("{}", length).size() + record.size() != length) {
        ++length;
    }

    const std::string records = std::format("{}{}", length, record);
    const std::string short_name(path.substr(path.size() - name_field.size));
    return header_of("", "PaxHeader", records.size(), 'x') + padded(records) +
           header_of("", short_name, file.bytes.size(), '0');
}

/// The pax keys an entry's header can have, as the registry takes them (docs/registry-api.md's
/// Archives): any other, like GNU.sparse.* or linkpath, changes what some extractor makes of the
/// entry.
constexpr std::array<std::string_view, 11> pax_keys = {
    // NOLINTNEXTLINE(readability-trailing-comma): clang-format packs the names on two lines
    "path", "size",  "mtime", "atime",      "ctime",   "uid",
    "gid",  "uname", "gname", "hdrcharset", "comment",
};

/// A pax header's records, `<length> <key>=<value>\n` each, by their keys, as the registry reads
/// them: each key one it takes, and once. Fails, saying why, for anything else.
std::expected<std::map<std::string, std::string, std::less<>>, std::string>
pax_records(std::string_view records) {
    std::map<std::string, std::string, std::less<>> out;
    while (!records.empty()) {
        const std::size_t space = records.find(' ');
        const std::string_view digits = records.substr(0, space);
        std::size_t length = 0;
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), length);
        if (space == std::string_view::npos || digits.empty() || digits.front() == '0' ||
            error != std::errc() || end != digits.data() + digits.size() || length <= space + 1 ||
            length > records.size() || records[length - 1] != '\n') {
            return std::unexpected("a pax header of its tar is malformed");
        }

        const std::string_view record = records.substr(space + 1, length - space - 2);
        const std::size_t equals = record.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected("a pax header of its tar is malformed");
        }

        const std::string_view key = record.substr(0, equals);
        if (!std::ranges::contains(pax_keys, key)) {
            return std::unexpected(
                std::format("a pax header of its tar has '{}', which the registry doesn't take",
                            escape_controls(key.substr(0, 40))));
        }

        if (!out.emplace(std::string(key), std::string(record.substr(equals + 1))).second) {
            return std::unexpected(std::format("a pax header of its tar has {} twice", key));
        }

        records.remove_prefix(length);
    }

    return out;
}

/// A pax size: decimal digits, without a leading zero but for 0 itself.
std::optional<std::uint64_t> pax_size(const std::string_view text) {
    std::uint64_t size = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), size);
    if (text.empty() || (text.size() > 1 && text.front() == '0') || error != std::errc() ||
        end != text.data() + text.size()) {
        return std::nullopt;
    }

    return size;
}

/// gzip of `bytes`, with no name, the time 0, and an unknown OS in its header.
std::expected<std::string, std::string> gzip(const std::string_view bytes) {
    z_stream stream{};
    constexpr int gzip_window = 15 + 16;
    constexpr int memory_level = 8;
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, gzip_window, memory_level,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return std::unexpected("zlib couldn't start compressing");
    }

    gz_header header{};
    header.os = 255;
    deflateSetHeader(&stream, &header);
    std::string out;
    std::array<unsigned char, 65536> chunk{};
    // zlib takes its input as unsigned and not const; it doesn't change it.
    stream.next_in =
        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(bytes.data()));
    stream.avail_in = static_cast<unsigned>(bytes.size());
    int result = Z_OK;
    while (result != Z_STREAM_END) {
        stream.next_out = chunk.data();
        stream.avail_out = static_cast<unsigned>(chunk.size());
        result = deflate(&stream, Z_FINISH);
        if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
            deflateEnd(&stream);
            return std::unexpected("zlib couldn't compress the archive");
        }

        out.append(reinterpret_cast<const char*>(chunk.data()), chunk.size() - stream.avail_out);
    }

    deflateEnd(&stream);
    return out;
}

/// Gives an archive's gzip'd bytes, a piece at a time: into `buffer`, at most `size` of them,
/// giving how many, 0 at their end.
using source_t = std::function<std::size_t(char* buffer, std::size_t size)>;

/// What gzip's bytes from a source hold, read as they're needed, so an archive is never all in
/// memory, up to `most` bytes. Reading past its end, or bytes that aren't gzip, is an error, which
/// stays.
class gunzip_t {
public:
    gunzip_t(source_t source, const std::uint64_t most)
        : m_source(std::move(source)), m_most(most) {
        constexpr int gzip_window = 15 + 16;
        if (inflateInit2(&m_stream, gzip_window) != Z_OK) {
            m_error = "zlib couldn't start decompressing";
        }
    }

    gunzip_t(const gunzip_t&) = delete;
    gunzip_t& operator=(const gunzip_t&) = delete;
    gunzip_t(gunzip_t&&) = delete;
    gunzip_t& operator=(gunzip_t&&) = delete;

    ~gunzip_t() {
        inflateEnd(&m_stream);
    }

    /// Reads exactly `size` bytes into `out`; false at an error or the stream's end before them.
    bool read(char* out, std::size_t size) {
        while (size > 0) {
            if (m_at == m_out.size() && !fill()) {
                return false;
            }

            const std::size_t taken = std::min(size, m_out.size() - m_at);
            std::copy_n(m_out.data() + m_at, taken, out);
            m_at += taken;
            out += taken;
            size -= taken;
        }

        return true;
    }

    /// Whether what's left is only zeros, and then the stream's end, with no bytes after it.
    bool rest_is_zeros() {
        while (true) {
            if (!std::all_of(m_out.begin() + static_cast<std::ptrdiff_t>(m_at), m_out.end(),
                             [](const char c) { return c == '\0'; })) {
                return false;
            }

            m_at = m_out.size();
            if (!fill()) {
                return m_error.empty() && m_ended;
            }
        }
    }

    /// Whether it stopped at its limit.
    [[nodiscard]] bool over() const {
        return m_over;
    }

    [[nodiscard]] const std::string& error() const {
        return m_error;
    }

private:
    source_t m_source;
    std::uint64_t m_most = 0;
    std::uint64_t m_total = 0;
    z_stream m_stream{};
    std::array<char, 65536> m_in{};
    std::string m_out;
    std::size_t m_at = 0;
    bool m_ended = false;
    bool m_over = false;
    std::string m_error;

    /// Inflates more, into m_out; false at the stream's end or an error.
    bool fill() {
        m_out.clear();
        m_at = 0;
        std::array<unsigned char, 65536> chunk{};
        while (m_out.empty() && m_error.empty() && !m_ended) {
            if (m_stream.avail_in == 0) {
                const std::size_t got = m_source(m_in.data(), m_in.size());
                if (got == 0) {
                    m_error = "it isn't gzip, or it's cut short";
                    return false;
                }

                // zlib takes its input as unsigned and not const; it doesn't change it.
                m_stream.next_in = reinterpret_cast<unsigned char*>(m_in.data());
                m_stream.avail_in = static_cast<unsigned>(got);
            }

            m_stream.next_out = chunk.data();
            m_stream.avail_out = static_cast<unsigned>(chunk.size());
            const int result = inflate(&m_stream, Z_NO_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
                m_error = "it isn't gzip, or it's cut short";
                return false;
            }

            const std::size_t made = chunk.size() - m_stream.avail_out;
            m_out.append(reinterpret_cast<const char*>(chunk.data()), made);
            m_total += made;
            if (m_total > m_most) {
                m_over = true;
                m_error = "it unpacks to more than its limit";
                return false;
            }

            if (result == Z_STREAM_END) {
                m_ended = true;
                // Nothing but the stream: no bytes after it, here or still to read.
                if (m_stream.avail_in > 0 || m_source(m_in.data(), m_in.size()) > 0) {
                    m_error = "it has bytes after its gzip stream";
                    return false;
                }
            }
        }

        return !m_out.empty();
    }
};

/// What an archive's walk does with each file: `begin` with its path and size, then `data`
/// with its bytes, piece by piece. Either fails, saying why, to stop the walk.
struct entries_t {
    std::function<std::expected<void, std::string>(const std::string& path, std::uint64_t size)>
        begin;
    std::function<std::expected<void, std::string>(std::string_view bytes)> data;
};

/// Walks a .fbpkg's entries from `source`, checking them as read_archive() says, up to `most`
/// bytes of files.
std::expected<void, std::string> walk_archive(source_t source, std::uint64_t most,
                                              const entries_t& entries);

/// A path as a disk that ignores letter case compares it, as the registry does: its
/// compatibility forms composed (NFKC) and its case folded, so "ſ" is "s" and "É" is "é". As it
/// is when it isn't UTF-8.
std::string folded(const std::string_view path) {
    constexpr auto options = static_cast<utf8proc_option_t>(
        static_cast<unsigned>(UTF8PROC_STABLE) | static_cast<unsigned>(UTF8PROC_COMPAT) |
        static_cast<unsigned>(UTF8PROC_COMPOSE) | static_cast<unsigned>(UTF8PROC_CASEFOLD));
    utf8proc_uint8_t* mapped = nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): utf8proc's bytes are chars
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(path.data());
    const utf8proc_ssize_t size =
        utf8proc_map(bytes, static_cast<utf8proc_ssize_t>(path.size()), &mapped, options);
    if (size < 0) {
        return std::string(path);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): utf8proc's bytes are chars
    std::string text(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(size));
    std::free(mapped); // NOLINT(cppcoreguidelines-no-malloc): utf8proc mallocs it
    return text;
}

/// Whether a part of a path is a name Windows keeps for a device, with any extension: CON, PRN,
/// AUX, NUL, CONIN$, CONOUT$, and COM and LPT with a digit or ¹, ², or ³, in any letter case.
bool is_device_name(const std::string_view name) {
    std::string stem(name.substr(0, name.find('.')));
    std::ranges::transform(stem, stem.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" ||
        stem == "conout$") {
        return true;
    }

    if (!stem.starts_with("com") && !stem.starts_with("lpt")) {
        return false;
    }

    const std::string_view number = std::string_view(stem).substr(3);
    return (number.size() == 1 && number[0] >= '0' && number[0] <= '9') || number == "\xc2\xb9" ||
           number == "\xc2\xb2" || number == "\xc2\xb3";
}

} // namespace

std::optional<std::string> archive_path_error(const std::string_view path) {
    if (path.empty() || path.starts_with('/')) {
        return std::format("'{}' isn't a relative path", path);
    }

    if (path.size() > longest_archive_path) {
        return std::format("'{}' is longer than {} bytes", path, longest_archive_path);
    }

    for (std::size_t at = 0; at < path.size();) {
        const decoded_t decoded = decode_utf8(path, at);
        if (!decoded.valid()) {
            return std::format("'{}' isn't UTF-8", escape_controls(path));
        }

        // C0's controls and DEL, and C1's, U+0080 to U+009F.
        const char32_t code = decoded.code_point;
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
            (code < 0x80 && std::string_view("\\:<>\"|?*").contains(static_cast<char>(code)))) {
            return std::format("'{}' has a control character or one of \\ : < > \" | ? *, which "
                               "Windows can't name",
                               escape_controls(path));
        }

        at += decoded.length;
    }

    for (const auto part : std::views::split(path, '/')) {
        const std::string_view name(part.begin(), part.end());
        if (name.empty() || name == "." || name == "..") {
            return std::format("'{}' has an empty, '.', or '..' part, which could leave the "
                               "package's folder",
                               path);
        }

        if (name.ends_with('.') || name.ends_with(' ')) {
            return std::format("'{}' has a part ending in '.' or ' ', which Windows can't name",
                               path);
        }

        if (is_device_name(name)) {
            return std::format("'{}' has a part Windows keeps for a device, {}", path, name);
        }
    }

    return std::nullopt;
}

namespace {

/// Writes `bytes` to `path`, making its folders. False when it can't.
bool write_file(const std::filesystem::path& path, const std::string_view bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return static_cast<bool>(out);
}

/// The seconds since 1970 now, which a partial folder's name has.
std::int64_t seconds_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// A name for a partial folder beside `path`, which no other process picks: the time it's
/// made, in hex seconds, then a random number, short, since Windows counts a path's characters.
std::filesystem::path partial_of(const std::filesystem::path& path) {
    std::random_device random;
    std::filesystem::path out = path;
    out += std::format("{}{:x}-{:08x}", partial_mark, seconds_now(), random());
    return out;
}

/// Adds the files of `folder`, `relative` from the package's root, to `files`.
void add_files(const std::filesystem::path& folder, const std::string& relative,
               package_files_t& files) {
    std::error_code error;
    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator each(folder, error), end; !error && each != end;
         each.increment(error)) {
        entries.push_back(each->path());
    }

    std::ranges::sort(entries, [](const auto& left, const auto& right) {
        return to_utf8(left.filename()) < to_utf8(right.filename());
    });
    for (const std::filesystem::path& entry : entries) {
        const std::string name = to_utf8(entry.filename());
        const std::string path = relative.empty() ? name : std::format("{}/{}", relative, name);
        if (name.starts_with('.')) {
            continue;
        }

        if (std::filesystem::is_directory(entry, error)) {
            // A folder with a project.toml of its own is another package.
            if ((relative.empty() && (name == "build" || name == "tests")) ||
                std::filesystem::exists(entry / "project.toml", error)) {
                continue;
            }

            add_files(entry, path, files);
            continue;
        }

        if (!std::filesystem::is_regular_file(entry, error) ||
            (relative.empty() && name == "fb.lock")) {
            continue;
        }

        // A file in lib/<target>/ or native/<target>/ goes in that target's archive.
        const auto target = std::ranges::find_if(package_targets, [&](const std::string_view each) {
            return path.starts_with(std::format("lib/{}/", each)) ||
                   path.starts_with(std::format("native/{}/", each));
        });
        if (target != package_targets.end()) {
            files.targets[std::string(*target)].push_back(path);
        } else {
            files.source.push_back(path);
        }
    }
}

} // namespace

std::string sha256_hex(const std::string_view bytes) {
    static const psa_status_t started = psa_crypto_init();
    std::array<std::uint8_t, 32> hash{};
    std::size_t length = 0;
    if (started != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const std::uint8_t*>(bytes.data()),
                         bytes.size(), hash.data(), hash.size(), &length) != PSA_SUCCESS) {
        return {};
    }

    std::string out;
    for (const std::uint8_t byte : hash) {
        out += std::format("{:02x}", byte);
    }

    return out;
}

std::expected<std::string, std::string> write_archive(std::vector<archive_file_t> files) {
    std::ranges::sort(files, {}, &archive_file_t::path);
    std::string tar;
    // No two paths a disk that ignores letter case takes as one, which unpacking refuses.
    std::set<std::string, std::less<>> seen;
    for (const archive_file_t& file : files) {
        if (const std::optional<std::string> wrong = archive_path_error(file.path)) {
            return std::unexpected(*wrong);
        }

        if (!seen.insert(folded(file.path)).second) {
            return std::unexpected(std::format(
                "'{}' and another path are one to a disk that ignores letter case", file.path));
        }

        tar += headers_of(file);
        tar += padded(file.bytes);
    }

    tar.append(2 * block, '\0');
    return gzip(tar);
}

namespace {

/// "80 MB", as the registry counts megabytes, or a smaller limit's bytes.
std::string megabytes(const std::uint64_t bytes) {
    if (bytes < 1'000'000) {
        return std::format("{} bytes", bytes);
    }

    return std::format("{} MB", bytes / 1'000'000);
}

std::expected<void, std::string> walk_archive(source_t source, const std::uint64_t most,
                                              const entries_t& entries) {
    // Its files' bytes, each with its headers and padding, and the two blocks at the end.
    gunzip_t tar(std::move(source), most + (most_archive_files * 4 * block) + (2 * block));
    const auto broken = [&](const std::string_view why) {
        // Past the limit, it's the limit that's said, not the tar's own.
        if (tar.over()) {
            return std::unexpected(std::format("it unpacks to more than {}", megabytes(most)));
        }

        return std::unexpected(std::format("it isn't a .fbpkg: {}",
                                           tar.error().empty() ? std::string(why) : tar.error()));
    };
    // Each path, folded, as a file's or a folder's, to find one two entries share, and a file
    // that's another's folder.
    std::set<std::string, std::less<>> files;
    std::set<std::string, std::less<>> folders;
    std::uint64_t total = 0;
    std::size_t count = 0;
    // A pax header's records, for the entry after it.
    std::optional<std::map<std::string, std::string, std::less<>>> pax;
    std::string header(block, '\0');
    std::string piece(65536, '\0');
    while (true) {
        if (!tar.read(header.data(), block)) {
            return broken("its tar ends without its end marker");
        }

        if (std::ranges::all_of(header, [](const char c) { return c == '\0'; })) {
            if (pax) {
                return broken("its tar ends after a pax header");
            }

            break;
        }

        const std::optional<std::uint64_t> sum = octal_of(header, sum_field);
        std::optional<std::uint64_t> size = octal_of(header, size_field);
        if (!sum || *sum != checksum_of(header)) {
            return broken("a header of its tar has a bad checksum");
        }

        // POSIX ustar's magic and version, exactly; GNU's and old tars' aren't taken.
        if (header.substr(magic_field.at, 8) != std::string_view("ustar\0"
                                                                 "00",
                                                                 8) ||
            !size) {
            return broken("a header of its tar isn't POSIX ustar's");
        }

        const char type = header[type_at];
        if (type == 'x') {
            if (pax) {
                return broken("an entry of its tar has two pax headers");
            }

            if (*size > std::uint64_t{ 1 } << 20U) {
                return broken("a pax header of its tar is too large");
            }

            std::string records(*size + ((block - (*size % block)) % block), '\0');
            if (!tar.read(records.data(), records.size())) {
                return broken("it ends in the middle of a pax header");
            }

            records.resize(*size);
            std::expected<std::map<std::string, std::string, std::less<>>, std::string> read =
                pax_records(records);
            if (!read) {
                return broken(read.error());
            }

            pax = std::move(*read);
            continue;
        }

        if (type == 'g' || type == 'L' || type == 'K') {
            return broken("its tar has a global or GNU extended header, which the registry "
                          "doesn't take");
        }

        // A pax header's path and size are the entry's, as every extractor reads them.
        const std::string_view prefix = text_of(header, prefix_field);
        const std::string_view name = text_of(header, name_field);
        std::string path = prefix.empty() ? std::string(name) : std::format("{}/{}", prefix, name);
        if (pax && pax->contains("path")) {
            path = pax->at("path");
        }

        if (pax && pax->contains("size")) {
            size = pax_size(pax->at("size"));
            if (!size) {
                return broken("a pax header of its tar has a size that isn't a number");
            }
        }

        pax.reset();
        if (++count > most_archive_files) {
            return std::unexpected(
                std::format("it has more than {} files and folders", most_archive_files));
        }

        const bool folder = type == '5';
        if (type != '0' && !folder) {
            return std::unexpected(std::format(
                "it has '{}', which isn't a file or a folder, like a link, which fb doesn't unpack",
                escape_controls(path)));
        }

        if (folder && *size != 0) {
            return broken(std::format("its folder '{}' has a size, which extractors read "
                                      "differently",
                                      escape_controls(path)));
        }

        if (folder && path.ends_with('/')) {
            path.pop_back();
        }

        if (const std::optional<std::string> wrong = archive_path_error(path)) {
            return std::unexpected(*wrong);
        }

        // Neither twice, nor a file where a folder is, comparing as a disk that ignores letter
        // case does.
        const std::string key = folded(path);
        for (std::size_t slash = key.find('/'); slash != std::string::npos;
             slash = key.find('/', slash + 1)) {
            if (files.contains(key.substr(0, slash))) {
                return std::unexpected(
                    std::format("it has '{}' as both a file and a folder", path));
            }

            folders.insert(key.substr(0, slash));
        }

        if (folder) {
            if (files.contains(key)) {
                return std::unexpected(
                    std::format("it has '{}' as both a file and a folder", path));
            }

            folders.insert(key);
            continue;
        }

        if (files.contains(key)) {
            return std::unexpected(std::format(
                "it has '{}' twice, or two paths a disk that ignores letter case takes as one",
                path));
        }

        if (folders.contains(key)) {
            return std::unexpected(std::format("it has '{}' as both a file and a folder", path));
        }

        files.insert(key);
        total += *size;
        if (total > most) {
            return std::unexpected(std::format("it unpacks to more than {}", megabytes(most)));
        }

        if (const std::expected<void, std::string> begun = entries.begin(path, *size); !begun) {
            return begun;
        }

        for (std::uint64_t left = *size; left > 0;) {
            const std::size_t taken = std::min<std::uint64_t>(left, piece.size());
            if (!tar.read(piece.data(), taken)) {
                return broken(std::format("it ends in the middle of '{}'", path));
            }

            if (const std::expected<void, std::string> written =
                    entries.data(std::string_view(piece).substr(0, taken));
                !written) {
                return written;
            }

            left -= taken;
        }

        const std::uint64_t padding = (block - (*size % block)) % block;
        if (padding > 0 && !tar.read(piece.data(), padding)) {
            return broken(std::format("it ends in the middle of '{}'", path));
        }
    }

    // After the end, only zeros, and no bytes after the gzip stream.
    if (!tar.rest_is_zeros()) {
        return broken("it has bytes after its tar's end");
    }

    return {};
}

/// A source of `bytes`, in memory.
source_t source_of(const std::string_view bytes) {
    return [bytes, at = std::size_t{ 0 }](char* buffer, const std::size_t size) mutable {
        const std::size_t taken = std::min(size, bytes.size() - at);
        std::copy_n(bytes.data() + at, taken, buffer);
        at += taken;
        return taken;
    };
}

/// Whether a path in an archive is one its kind has: a package's own has none of a target's
/// files, and a target's only its own; neither has a marker, which only fb writes. They're
/// compared as a disk that ignores letter case would, so "Native/Linux-x64/" is a target's too.
bool is_of_kind(const std::string_view path, const std::string_view marker) {
    const std::string key = folded(path);
    const bool targets = std::ranges::any_of(package_targets, [&](const std::string_view each) {
        return key.starts_with(std::format("lib/{}/", each)) ||
               key.starts_with(std::format("native/{}/", each));
    });
    const bool own = key.starts_with(std::format("lib/{}/", marker)) ||
                     key.starts_with(std::format("native/{}/", marker));
    return !key.starts_with(".fbpkg-") && (marker == "source" ? !targets : own);
}

/// A marker's checksum, or empty when it isn't there.
std::string read_marker(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

/// Removes what an fb stopped partway through unpacking `folder` left beside it: its partial
/// folders made over an hour ago, as their names say, which no fb still at work has.
void remove_stale_partials(const std::filesystem::path& folder) {
    const std::string prefix = to_utf8(folder.filename()) + std::string(partial_mark);
    const std::int64_t stale = seconds_now() - 3600;
    std::error_code error;
    for (std::filesystem::directory_iterator each(folder.parent_path(), error), end;
         !error && each != end; each.increment(error)) {
        const std::string name = to_utf8(each->path().filename());
        if (!name.starts_with(prefix)) {
            continue;
        }

        const std::string_view made = std::string_view(name).substr(prefix.size());
        std::int64_t seconds = 0;
        const auto [stop, wrong] =
            std::from_chars(made.data(), made.data() + made.size(), seconds, 16);
        if (wrong == std::errc() && stop != made.data() && stop != made.data() + made.size() &&
            *stop == '-' && seconds < stale) {
            std::error_code ignored;
            std::filesystem::remove_all(each->path(), ignored);
        }
    }
}

/// Why a file of the archive can't be written under `folder` on this OS, or nullopt when it can:
/// on Windows, a path past MAX_PATH's 259 characters, which Windows' own calls refuse.
std::optional<std::string> too_long([[maybe_unused]] const std::filesystem::path& folder,
                                    [[maybe_unused]] const std::filesystem::path& partial,
                                    [[maybe_unused]] const std::string_view path) {
#ifdef _WIN32
    constexpr std::size_t most_characters = 259;
    const std::size_t longest = std::max((partial / path_from_utf8(path)).native().size(),
                                         (folder / path_from_utf8(path)).native().size());
    if (longest > most_characters) {
        return std::format(
            "'{}' is too long a path for Windows in {}: {} characters, past its {}; a shorter "
            "FB_HOME holds it",
            path, to_utf8(folder), longest, most_characters);
    }
#endif
    return std::nullopt;
}

} // namespace

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }

    static const psa_status_t started = psa_crypto_init();
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (started != PSA_SUCCESS || psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return {};
    }

    std::array<char, 65536> chunk{};
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got > 0 &&
            psa_hash_update(&operation, reinterpret_cast<const std::uint8_t*>(chunk.data()), got) !=
                PSA_SUCCESS) {
            psa_hash_abort(&operation);
            return {};
        }
    }

    std::array<std::uint8_t, 32> hash{};
    std::size_t length = 0;
    if (in.bad() || psa_hash_finish(&operation, hash.data(), hash.size(), &length) != PSA_SUCCESS) {
        psa_hash_abort(&operation);
        return {};
    }

    std::string out;
    for (const std::uint8_t byte : hash) {
        out += std::format("{:02x}", byte);
    }

    return out;
}

std::expected<std::vector<archive_file_t>, std::string> read_archive(const std::string_view bytes,
                                                                     const std::uint64_t most) {
    std::vector<archive_file_t> files;
    const entries_t entries{
        .begin = [&](const std::string& path,
                     const std::uint64_t size) -> std::expected<void, std::string> {
            files.push_back({ .path = path });
            files.back().bytes.reserve(size);
            return {};
        },
        .data = [&](const std::string_view piece) -> std::expected<void, std::string> {
            files.back().bytes += piece;
            return {};
        },
    };
    if (const std::expected<void, std::string> walked =
            walk_archive(source_of(bytes), most, entries);
        !walked) {
        return std::unexpected(walked.error());
    }

    return files;
}

std::expected<void, std::string> unpack_archive(const std::filesystem::path& archive,
                                                const std::filesystem::path& folder,
                                                const std::string_view marker,
                                                const std::string_view checksum,
                                                const std::uint64_t most) {
    std::ifstream in(archive, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("couldn't read {}", to_utf8(archive)));
    }

    const std::filesystem::path marker_name = path_from_utf8(std::format(".fbpkg-{}", marker));
    std::error_code error;
    const bool there = std::filesystem::is_directory(folder, error);
    // A package's own archive makes its folder, never joins one: a folder that's there was
    // unpacked whole by another fb, with its marker, or is something else's.
    if (marker == "source" && there) {
        if (read_marker(folder / marker_name) == checksum) {
            return {};
        }

        return std::unexpected(std::format(
            "{} is there already, but isn't this archive's; deleting it lets fb unpack it again",
            to_utf8(folder)));
    }

    remove_stale_partials(folder);
    // Each file goes into a folder beside the package's, which is renamed, or whose files are,
    // once they're all there.
    const std::filesystem::path partial = partial_of(folder);
    std::vector<std::string> paths;
    std::ofstream out;
    std::filesystem::path writing;
    const auto failed = [&](const std::filesystem::path& file, const std::error_code& why) {
        return std::unexpected(
            std::format("couldn't write {}{}{}", to_utf8(file), why ? ": " : "", why.message()));
    };
    const entries_t entries{
        .begin = [&](const std::string& path, std::uint64_t) -> std::expected<void, std::string> {
            if (!is_of_kind(path, marker)) {
                return std::unexpected(
                    std::format("it has '{}', which an archive of its kind can't", path));
            }

            if (const std::optional<std::string> wrong = too_long(folder, partial, path)) {
                return std::unexpected(*wrong);
            }

            out.close();
            writing = partial / path_from_utf8(path);
            std::error_code made;
            std::filesystem::create_directories(writing.parent_path(), made);
            out.open(writing, std::ios::binary);
            if (!out) {
                return failed(writing, made);
            }

            paths.push_back(path);
            return {};
        },
        .data = [&](const std::string_view piece) -> std::expected<void, std::string> {
            out.write(piece.data(), static_cast<std::streamsize>(piece.size()));
            if (!out) {
                return failed(writing, std::error_code());
            }

            return {};
        },
    };
    const source_t source = [&](char* buffer, const std::size_t size) {
        in.read(buffer, static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(in.gcount());
    };
    std::expected<void, std::string> walked = walk_archive(source, most, entries);
    out.close();
    if (walked && !out) {
        walked = failed(writing, std::error_code());
    }

    if (walked && !write_file(partial / marker_name, checksum)) {
        walked = failed(partial / marker_name, std::error_code());
    }

    if (!walked) {
        std::filesystem::remove_all(partial, error);
        return walked;
    }

    if (there) {
        // A target's archive joins its package's: each file joins it, and the marker, last, says
        // they all did.
        for (const std::string& path : paths) {
            const std::filesystem::path to = folder / path_from_utf8(path);
            std::filesystem::create_directories(to.parent_path(), error);
            std::filesystem::rename(partial / path_from_utf8(path), to, error);
            if (error) {
                const std::error_code why = error;
                std::filesystem::remove_all(partial, error);
                return failed(to, why);
            }
        }

        std::filesystem::rename(partial / marker_name, folder / marker_name, error);
        const std::error_code why = error;
        std::filesystem::remove_all(partial, error);
        if (why) {
            return failed(folder / marker_name, why);
        }

        return {};
    }

    std::filesystem::create_directories(folder.parent_path(), error);
    std::filesystem::rename(partial, folder, error);
    if (error) {
        // Another fb unpacked it first; the one there is whole when its marker says so.
        std::error_code ignored;
        std::filesystem::remove_all(partial, ignored);
        if (read_marker(folder / marker_name) != checksum) {
            return std::unexpected(
                std::format("couldn't unpack into {}: {}", to_utf8(folder), error.message()));
        }
    }

    return {};
}

package_files_t package_files(const std::filesystem::path& root) {
    package_files_t files;
    add_files(root, "", files);
    return files;
}

} // namespace fb
