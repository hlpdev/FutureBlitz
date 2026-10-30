#include "fb/package_archive.hpp"

#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <psa/crypto.h>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <zlib.h>

namespace fb {
namespace {

constexpr std::size_t block = 512;

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

/// A header's octal field, or nullopt when it isn't one.
std::optional<std::uint64_t> octal_of(const std::string_view header, const field_t field) {
    std::string_view text = text_of(header, field);
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }

    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }

    if (text.empty() || text.size() > 21) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '7') {
            return std::nullopt;
        }

        value = (value << 3U) | static_cast<std::uint64_t>(c - '0');
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

/// A pax header's path, from its records, or nullopt when it has none or isn't one.
std::optional<std::string> pax_path(std::string_view records) {
    std::optional<std::string> path;
    while (!records.empty()) {
        const std::size_t space = records.find(' ');
        if (space == std::string_view::npos) {
            return std::nullopt;
        }

        const std::string_view digits = records.substr(0, space);
        std::size_t length = 0;
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), length);
        if (error != std::errc() || end != digits.data() + digits.size() || length <= space + 1 ||
            length > records.size() || records[length - 1] != '\n') {
            return std::nullopt;
        }

        const std::string_view record = records.substr(space + 1, length - space - 2);
        if (record.starts_with("path=")) {
            path = std::string(record.substr(5));
        }

        records.remove_prefix(length);
    }

    return path;
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

/// What gzip's `bytes` hold, up to `most` bytes.
std::expected<std::string, std::string> gunzip(const std::string_view bytes,
                                               const std::uint64_t most) {
    z_stream stream{};
    constexpr int gzip_window = 15 + 16;
    if (inflateInit2(&stream, gzip_window) != Z_OK) {
        return std::unexpected("zlib couldn't start decompressing");
    }

    std::string out;
    std::array<unsigned char, 65536> chunk{};
    stream.next_in =
        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(bytes.data()));
    stream.avail_in = static_cast<unsigned>(bytes.size());
    int result = Z_OK;
    while (result != Z_STREAM_END) {
        stream.next_out = chunk.data();
        stream.avail_out = static_cast<unsigned>(chunk.size());
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            inflateEnd(&stream);
            return std::unexpected("it isn't gzip, or it's cut short");
        }

        out.append(reinterpret_cast<const char*>(chunk.data()), chunk.size() - stream.avail_out);
        if (out.size() > most) {
            inflateEnd(&stream);
            return std::unexpected(std::format("it unpacks to more than {} MB", most >> 20U));
        }
    }

    const bool rest = stream.avail_in > 0;
    inflateEnd(&stream);
    if (rest) {
        return std::unexpected("it has bytes after its gzip stream");
    }

    return out;
}

/// The letter case of ASCII folded, for comparing paths as a disk that ignores it does.
std::string folded(const std::string_view path) {
    std::string out(path);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return out;
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

        at += decoded.length;
    }

    if (std::ranges::any_of(path, [](const char c) {
            return std::string_view("\\:<>\"|?*").contains(c) ||
                   static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
        })) {
        return std::format("'{}' has a control character or one of \\ : < > \" | ? *, which "
                           "Windows can't name",
                           escape_controls(path));
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

        const std::string stem = folded(name.substr(0, name.find('.')));
        const bool numbered = stem.size() == 4 &&
                              (stem.starts_with("com") || stem.starts_with("lpt")) &&
                              stem[3] >= '1' && stem[3] <= '9';
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || numbered) {
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

/// A name for a temporary file or folder beside `path`, which no other process picks.
std::filesystem::path partial_of(const std::filesystem::path& path) {
    std::random_device random;
    std::filesystem::path out = path;
    out += std::format(".partial-{:08x}{:08x}", random(), random());
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
    for (const archive_file_t& file : files) {
        if (const std::optional<std::string> wrong = archive_path_error(file.path)) {
            return std::unexpected(*wrong);
        }

        tar += headers_of(file);
        tar += padded(file.bytes);
    }

    tar.append(2 * block, '\0');
    return gzip(tar);
}

std::expected<std::vector<archive_file_t>, std::string> read_archive(const std::string_view bytes,
                                                                     const std::uint64_t most) {
    // Its files' bytes, each with its headers and padding, and the two blocks at the end.
    const std::uint64_t most_tar = most + (most_archive_files * 4 * block) + (2 * block);
    const std::expected<std::string, std::string> tar = gunzip(bytes, most_tar);
    if (!tar) {
        return std::unexpected(std::format("it isn't a .fbpkg: {}", tar.error()));
    }

    std::vector<archive_file_t> files;
    std::set<std::string, std::less<>> seen;
    std::uint64_t total = 0;
    std::size_t at = 0;
    // A pax header's path, for the entry after it.
    std::optional<std::string> long_path;
    while (true) {
        if (at + block > tar->size()) {
            return std::unexpected("it isn't a .fbpkg: its tar ends without its two empty blocks");
        }

        const std::string_view header = std::string_view(*tar).substr(at, block);
        if (std::ranges::all_of(header, [](const char c) { return c == '\0'; })) {
            break;
        }

        const std::optional<std::uint64_t> sum = octal_of(header, sum_field);
        const std::optional<std::uint64_t> size = octal_of(header, size_field);
        if (!sum || *sum != checksum_of(header) || !size ||
            text_of(header, magic_field) != "ustar") {
            return std::unexpected("it isn't a .fbpkg: a header of its tar isn't ustar's");
        }

        const std::string_view prefix = text_of(header, prefix_field);
        const std::string_view name = text_of(header, name_field);
        std::string path = prefix.empty() ? std::string(name) : std::format("{}/{}", prefix, name);
        const char type = header[type_at];
        at += block;
        if (at + *size > tar->size()) {
            return std::unexpected(std::format("it ends in the middle of '{}'", path));
        }

        const std::size_t next = at + *size + ((block - (*size % block)) % block);
        if (type == 'x') {
            long_path = pax_path(std::string_view(*tar).substr(at, *size));
            if (!long_path) {
                return std::unexpected("it isn't a .fbpkg: a pax header of its tar has no path");
            }

            at = next;
            continue;
        }

        if (long_path) {
            path = std::move(*long_path);
            long_path.reset();
        }

        if (type == '5') {
            at = next;
            continue;
        }

        if (type != '0' && type != '\0') {
            return std::unexpected(std::format(
                "it has '{}', which isn't a file or a folder, like a link, which fb doesn't unpack",
                escape_controls(path)));
        }

        if (const std::optional<std::string> wrong = archive_path_error(path)) {
            return std::unexpected(*wrong);
        }

        if (!seen.insert(folded(path)).second) {
            return std::unexpected(std::format(
                "it has '{}' twice, or two paths that differ only in letter case", path));
        }

        total += *size;
        if (files.size() >= most_archive_files || total > most) {
            return std::unexpected(
                std::format("it has more than {} files, or unpacks to more than {} MB",
                            most_archive_files, most >> 20U));
        }

        files.push_back({ .path = std::move(path), .bytes = tar->substr(at, *size) });
        at = next;
    }

    // A file and a folder of one name can't both be on a disk.
    for (const archive_file_t& file : files) {
        const std::string inside = folded(file.path) + "/";
        const auto after = seen.lower_bound(inside);
        if (after != seen.end() && after->starts_with(inside)) {
            return std::unexpected(
                std::format("it has '{}' as both a file and a folder", file.path));
        }
    }

    return files;
}

std::expected<void, std::string> unpack_archive(const std::string_view bytes,
                                                const std::filesystem::path& folder,
                                                const std::string_view marker,
                                                const std::string_view checksum,
                                                const std::uint64_t most) {
    const std::expected<std::vector<archive_file_t>, std::string> files = read_archive(bytes, most);
    if (!files) {
        return std::unexpected(files.error());
    }

    // A package's own archive has none of a target's files, nor a marker, which only fb writes,
    // and a target's has only its own.
    for (const archive_file_t& file : *files) {
        const bool targets = std::ranges::any_of(package_targets, [&](const std::string_view each) {
            return file.path.starts_with(std::format("lib/{}/", each)) ||
                   file.path.starts_with(std::format("native/{}/", each));
        });
        const bool own = file.path.starts_with(std::format("lib/{}/", marker)) ||
                         file.path.starts_with(std::format("native/{}/", marker));
        if (file.path.starts_with(".fbpkg-") || (marker == "source" && targets) ||
            (marker != "source" && !own)) {
            return std::unexpected(
                std::format("it has '{}', which an archive of its kind can't", file.path));
        }
    }

    const std::filesystem::path marker_name = path_from_utf8(std::format(".fbpkg-{}", marker));
    std::error_code error;
    if (std::filesystem::is_directory(folder, error)) {
        // Another archive of the package's: each file is whole once it's renamed, and the
        // marker, last, says they all are.
        for (const archive_file_t& file : *files) {
            const std::filesystem::path path = folder / path_from_utf8(file.path);
            const std::filesystem::path partial = partial_of(path);
            if (!write_file(partial, file.bytes)) {
                return std::unexpected(std::format("couldn't write {}", to_utf8(path)));
            }

            std::filesystem::rename(partial, path, error);
            if (error) {
                std::filesystem::remove(partial, error);
                return std::unexpected(std::format("couldn't write {}", to_utf8(path)));
            }
        }

        if (!write_file(folder / marker_name, checksum)) {
            return std::unexpected(std::format("couldn't write {}", to_utf8(folder / marker_name)));
        }

        return {};
    }

    const std::filesystem::path partial = partial_of(folder);
    for (const archive_file_t& file : *files) {
        if (!write_file(partial / path_from_utf8(file.path), file.bytes)) {
            std::filesystem::remove_all(partial, error);
            return std::unexpected(std::format("couldn't write {}", to_utf8(folder)));
        }
    }

    if (!write_file(partial / marker_name, checksum)) {
        std::filesystem::remove_all(partial, error);
        return std::unexpected(std::format("couldn't write {}", to_utf8(folder)));
    }

    std::filesystem::create_directories(folder.parent_path(), error);
    std::filesystem::rename(partial, folder, error);
    if (error) {
        // Another fb unpacked it first; the one there is whole.
        std::error_code ignored;
        std::filesystem::remove_all(partial, ignored);
        if (!std::filesystem::is_directory(folder, ignored)) {
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
