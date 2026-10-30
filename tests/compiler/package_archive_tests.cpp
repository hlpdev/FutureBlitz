#include "fb/package_archive.hpp"
#include "package_helpers.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace {

using files_t = std::vector<fb::archive_file_t>;

std::string archive(files_t files) {
    const std::expected<std::string, std::string> bytes = fb::write_archive(std::move(files));
    INFO((bytes ? std::string() : bytes.error()));
    REQUIRE(bytes);
    return *bytes;
}

std::string read_error(const std::string_view bytes) {
    const std::expected<files_t, std::string> files =
        fb::read_archive(bytes, fb::most_source_unpacked);
    return files ? std::string() : files.error();
}

/// What gzip's `bytes` hold.
std::string gunzip(const std::string_view bytes) {
    z_stream stream{};
    REQUIRE(inflateInit2(&stream, 31) == Z_OK);
    stream.next_in =
        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(bytes.data()));
    stream.avail_in = static_cast<unsigned>(bytes.size());
    std::string out;
    std::string chunk(65536, '\0');
    int result = Z_OK;
    while (result != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<unsigned char*>(chunk.data());
        stream.avail_out = static_cast<unsigned>(chunk.size());
        result = inflate(&stream, Z_NO_FLUSH);
        REQUIRE((result == Z_OK || result == Z_STREAM_END));
        out.append(chunk.data(), chunk.size() - stream.avail_out);
    }

    inflateEnd(&stream);
    return out;
}

/// Whether a header's octal field is as the registry takes it: octal digits after any spaces,
/// then only NULs or spaces, and not empty or starting with a NUL.
bool strict_octal(const std::string_view field) {
    std::size_t at = 0;
    while (at < field.size() && field[at] == ' ') {
        ++at;
    }

    const std::size_t digits = at;
    while (at < field.size() && field[at] >= '0' && field[at] <= '7') {
        ++at;
    }

    if (at == digits || field.front() == '\0') {
        return false;
    }

    return std::ranges::all_of(field.substr(at),
                               [](const char c) { return c == '\0' || c == ' '; });
}

/// Why `tar` isn't inside the registry's subset of tar (docs/registry-api.md's Archives), or
/// empty when it is.
std::string subset_error(const std::string_view tar) {
    constexpr std::size_t block = 512;
    const std::vector<std::string_view> keys{
        "path", "size",  "mtime", "atime",      "ctime",   "uid",
        "gid",  "uname", "gname", "hdrcharset", "comment",
    };
    bool after_pax = false;
    std::size_t at = 0;
    while (at + block <= tar.size()) {
        const std::string_view header = tar.substr(at, block);
        if (std::ranges::all_of(header, [](const char c) { return c == '\0'; })) {
            // The end marker, and after it, only zeros.
            const bool zeros =
                std::ranges::all_of(tar.substr(at), [](const char c) { return c == '\0'; });
            if (after_pax) {
                return "a pax header ends the tar";
            }

            return zeros ? "" : "bytes after the end";
        }

        if (header.substr(257, 6) != std::string_view("ustar\0", 6) ||
            header.substr(263, 2) != "00") {
            return "a header isn't POSIX ustar's";
        }

        if (!strict_octal(header.substr(124, 12)) || !strict_octal(header.substr(148, 8))) {
            return "a size or checksum isn't strict octal";
        }

        const char type = header[156];
        const std::uint64_t size = std::stoull(std::string(header.substr(124, 12)), nullptr, 8);
        at += block;
        if (type == 'x') {
            if (after_pax) {
                return "two pax headers before one entry";
            }

            // Each record is "<length> <key>=<value>\n".
            std::string_view records = tar.substr(at, size);
            std::vector<std::string_view> seen;
            while (!records.empty()) {
                const std::size_t space = records.find(' ');
                const std::size_t length = std::stoull(std::string(records.substr(0, space)));
                const std::string_view record = records.substr(space + 1, length - space - 2);
                const std::string_view key = record.substr(0, record.find('='));
                if (!std::ranges::contains(keys, key) || std::ranges::contains(seen, key)) {
                    return std::format("a pax header has '{}'", key);
                }

                seen.push_back(key);
                records.remove_prefix(length);
            }

            after_pax = true;
        } else if (type == '0') {
            after_pax = false;
        } else if (type != '5' || size != 0) {
            return std::format("an entry of type '{}' with {} bytes", type, size);
        }

        at += (size + block - 1) / block * block;
    }

    return "no end marker";
}

/// gzip of `bytes`, as another tool would write it.
std::string gzip(const std::string_view bytes) {
    z_stream stream{};
    REQUIRE(deflateInit2(&stream, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string out(deflateBound(&stream, static_cast<uLong>(bytes.size())), '\0');
    stream.next_in =
        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(bytes.data()));
    stream.avail_in = static_cast<unsigned>(bytes.size());
    stream.next_out = reinterpret_cast<unsigned char*>(out.data());
    stream.avail_out = static_cast<unsigned>(out.size());
    REQUIRE(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

/// A tar of one entry, its header as ustar's, of `type` and `name`, with `body`.
std::string tar_of(const std::string_view name, const char type, const std::string_view body) {
    std::string header(512, '\0');
    header.replace(0, name.size(), name);
    header.replace(100, 7, "0000644");
    header.replace(108, 7, "0000000");
    header.replace(116, 7, "0000000");
    const std::string size = std::format("{:011o}", body.size());
    header.replace(124, 11, size);
    header.replace(136, 11, "00000000000");
    header[156] = type;
    header.replace(257, 6, std::string_view("ustar\0", 6));
    header.replace(263, 2, "00");
    std::uint64_t sum = 0;
    header.replace(148, 8, "        ");
    for (const char c : header) {
        sum += static_cast<unsigned char>(c);
    }

    header.replace(148, 7, std::format("{:06o}", sum) + std::string(1, '\0'));
    std::string out = header + std::string(body);
    out.append((512 - (body.size() % 512)) % 512, '\0');
    out.append(1024, '\0');
    return out;
}

} // namespace

TEST_CASE("an archive holds its files, and reads back as they were", "[archive]") {
    const std::string long_path = std::string(60, 'a') + "/" + std::string(90, 'b') + ".fb";
    const std::string longer_name = "x/" + std::string(130, 'c') + ".fb";
    const std::string bytes = archive({
        { .path = "project.toml", .bytes = "[project]\n" },
        { .path = "shapes/circle.fb", .bytes = std::string(1300, 'z') },
        { .path = "empty.txt", .bytes = "" },
        { .path = long_path, .bytes = "split" },
        { .path = longer_name, .bytes = "pax" },
    });
    const std::expected<files_t, std::string> files =
        fb::read_archive(bytes, fb::most_source_unpacked);
    REQUIRE(files);
    REQUIRE(files->size() == 5);
    // In their paths' byte order.
    CHECK(files->at(0).path == long_path);
    CHECK(files->at(0).bytes == "split");
    CHECK(files->at(1).path == "empty.txt");
    CHECK(files->at(1).bytes.empty());
    CHECK(files->at(2).path == "project.toml");
    CHECK(files->at(3).path == "shapes/circle.fb");
    CHECK(files->at(3).bytes == std::string(1300, 'z'));
    CHECK(files->at(4).path == longer_name);
    CHECK(files->at(4).bytes == "pax");

    // The same files make the same archive, in any order.
    CHECK(archive({ { .path = "b", .bytes = "2" }, { .path = "a", .bytes = "1" } }) ==
          archive({ { .path = "a", .bytes = "1" }, { .path = "b", .bytes = "2" } }));
    CHECK(fb::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("an archive's paths are ones every disk can hold, inside its folder", "[archive]") {
    const std::vector<std::string_view> paths{
        "/etc/passwd", "../up.fb", "a/../../b", "a//b",   "./a", "C:/x", "a\\b",
        "con.txt",     "x/LPT3",   "dot.",      "space ", "a?b", "a\"b", "nul",
    };
    for (const std::string_view wrong : paths) {
        INFO(wrong);
        CHECK(fb::archive_path_error(wrong));
        CHECK_FALSE(fb::write_archive({ { .path = std::string(wrong), .bytes = "" } }));
    }

    CHECK(fb::archive_path_error(std::string(256, 'a')));
    CHECK(fb::archive_path_error("bad\xff.fb"));
    CHECK_FALSE(fb::archive_path_error("console.fb"));
    CHECK_FALSE(fb::archive_path_error("com10/x.fb"));
    CHECK_FALSE(fb::archive_path_error("\xe6\xb8\xb8\xe6\x88\x8f/main.fb"));
}

TEST_CASE("an archive that isn't one fb takes is refused, saying why", "[archive]") {
    CHECK(read_error("not gzip").contains("isn't a .fbpkg"));
    CHECK(read_error(gzip(tar_of("../evil.fb", '0', "x"))).contains("'..'"));
    CHECK(read_error(gzip(tar_of("link", '2', ""))).contains("isn't a file or a folder"));
    CHECK(read_error(gzip(tar_of("hard", '1', ""))).contains("isn't a file or a folder"));
    CHECK(read_error(gzip(std::string(1024, '\0'))).empty());
    CHECK(read_error(gzip("short")).contains("two empty blocks"));
    CHECK(read_error(gzip(tar_of("a.fb", '0', "x")) + "trailing").contains("after its gzip"));

    std::string bad = tar_of("a.fb", '0', "x");
    bad[148] = '7';
    CHECK(read_error(gzip(bad)).contains("isn't ustar's"));

    // Two paths that are one on a disk that ignores letter case, and a file that's a folder too.
    const std::string twice =
        archive({ { .path = "A.fb", .bytes = "" }, { .path = "a.fb", .bytes = "" } });
    CHECK(read_error(twice).contains("twice"));
    const std::string both =
        archive({ { .path = "x", .bytes = "" }, { .path = "x/y.fb", .bytes = "" } });
    CHECK(read_error(both).contains("both a file and a folder"));

    // More than it can unpack to.
    const std::string large = archive({ { .path = "big", .bytes = std::string(4096, 'q') } });
    const std::expected<files_t, std::string> limited = fb::read_archive(large, 1024);
    REQUIRE_FALSE(limited);
    CHECK(limited.error().contains("unpacks to more than"));
}

namespace {

/// Unpacks `bytes`, written to a file in the sandbox's downloads/ first, as an archive fb
/// downloaded would be.
std::expected<void, std::string> unpack(const fb::testing::sandbox_t& sandbox,
                                        const std::string_view bytes,
                                        const std::filesystem::path& folder,
                                        const std::string_view marker,
                                        const std::string_view checksum, const std::uint64_t most) {
    sandbox.write("downloads/archive.fbpkg", bytes);
    return fb::unpack_archive(sandbox / "downloads/archive.fbpkg", folder, marker, checksum, most);
}

} // namespace

TEST_CASE("an archive unpacks whole into its folder, and a target's joins it", "[archive]") {
    const fb::testing::sandbox_t sandbox;
    const std::filesystem::path folder = sandbox / "cache/physics-1.0.0";
    const std::string source = archive({
        { .path = "project.toml", .bytes = "[project]\n" },
        { .path = "shapes/circle.fb", .bytes = "' circle\n" },
    });
    REQUIRE(unpack(sandbox, source, folder, "source", "abc", fb::most_source_unpacked));
    CHECK(sandbox.read("cache/physics-1.0.0/shapes/circle.fb") == "' circle\n");
    CHECK(sandbox.read("cache/physics-1.0.0/.fbpkg-source") == "abc");

    const std::string target =
        archive({ { .path = "lib/linux-x64/debug/physics.fbi", .bytes = "fbi" } });
    REQUIRE(unpack(sandbox, target, folder, "linux-x64", "def", fb::most_target_unpacked));
    CHECK(sandbox.read("cache/physics-1.0.0/lib/linux-x64/debug/physics.fbi") == "fbi");
    CHECK(sandbox.read("cache/physics-1.0.0/.fbpkg-linux-x64") == "def");

    // Nothing but the folder itself is left beside it.
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator(folder.parent_path())) {
        ++entries;
    }

    CHECK(entries == 1);

    // A package's own archive has no target's files or markers, and a target's only its own.
    const auto refused = [&](const std::string& bytes, const std::string_view marker) {
        const std::expected<void, std::string> unpacked =
            unpack(sandbox, bytes, sandbox / "cache/other", marker, "x", fb::most_target_unpacked);
        return unpacked ? std::string() : unpacked.error();
    };
    CHECK(refused(target, "source").contains("lib/linux-x64/debug/physics.fbi"));
    CHECK(refused(target, "windows-x64").contains("can't"));
    CHECK(refused(archive({ { .path = ".fbpkg-linux-x64", .bytes = "def" } }), "source")
              .contains(".fbpkg-linux-x64"));
    CHECK(refused(source, "linux-x64").contains("project.toml"));
    CHECK_FALSE(std::filesystem::exists(sandbox / "cache/other"));
}

TEST_CASE("a package's files go in its own archive, but for its targets' folders", "[archive]") {
    const fb::testing::sandbox_t sandbox;
    const std::vector<std::string_view> written{
        "pkg/project.toml",
        "pkg/README.md",
        "pkg/shapes.fb",
        "pkg/data/level.txt",
        "pkg/lib/windows-x64/debug/pkg.fbi",
        "pkg/native/linux-x64/libx.so",
        "pkg/lib/util.fb",
        "pkg/libs/linux-x64/libjolt.a",
        "pkg/build/pkg.exe",
        "pkg/tests/basic.fb",
        "pkg/fb.lock",
        "pkg/.git/HEAD",
        "pkg/.hidden",
        "pkg/inner/project.toml",
        "pkg/inner/x.fb",
        "pkg/sub/tests/kept.fb",
        "pkg/sub/build/kept.fb",
    };
    for (const std::string_view file : written) {
        sandbox.write(file, "x");
    }

    const fb::package_files_t files = fb::package_files(sandbox / "pkg");
    CHECK(files.source == std::vector<std::string>{
                              "README.md",
                              "data/level.txt",
                              "lib/util.fb",
                              "libs/linux-x64/libjolt.a",
                              "project.toml",
                              "shapes.fb",
                              "sub/build/kept.fb",
                              "sub/tests/kept.fb",
                          });
    REQUIRE(files.targets.size() == 2);
    CHECK(files.targets.at("windows-x64") ==
          std::vector<std::string>{ "lib/windows-x64/debug/pkg.fbi" });
    CHECK(files.targets.at("linux-x64") == std::vector<std::string>{ "native/linux-x64/libx.so" });
}

TEST_CASE("a package's archives stay inside the registry's subset of tar", "[archive]") {
    const fb::testing::sandbox_t sandbox;
    const std::string index = sandbox.registry();
    // A long path takes a pax header; one that splits fits ustar's prefix and name.
    const std::string long_name = "assets/" + std::string(140, 'l') + ".txt";
    const std::string split = std::string(80, 's') + "/" + std::string(60, 't') + ".fb";
    sandbox.library("shapes", "shapes", "0.3.0", "", { { "area.fb", "// shapes\n" } });
    sandbox.write("shapes/" + long_name, "long");
    sandbox.write("shapes/" + split, "// split\n");
    sandbox.write("shapes/native/linux-x64/libx.so", std::string(3000, 'x'));
    const std::expected<fb::prepared_package_t, std::string> prepared =
        fb::prepare_package(sandbox / "shapes", index, false);
    INFO((prepared ? std::string() : prepared.error()));
    REQUIRE(prepared);
    const std::string source = gunzip(prepared->source.bytes);
    CHECK(subset_error(source).empty());
    CHECK(source.contains(std::format(" path={}\n", long_name)));
    REQUIRE(prepared->targets.size() == 1);
    CHECK(subset_error(gunzip(prepared->targets.front().bytes)).empty());

    // The check itself finds what's outside the subset.
    CHECK(subset_error(tar_of("link", '2', "")) == "an entry of type '2' with 0 bytes");
    std::string gnu = tar_of("a.fb", '0', "x");
    gnu.replace(257, 8, std::string_view("ustar  \0", 8));
    CHECK(subset_error(gnu) == "a header isn't POSIX ustar's");
    CHECK(subset_error(tar_of("a.fb", '0', "x") + "x") == "bytes after the end");
}

TEST_CASE("a project.toml over 1 MB isn't published", "[archive]") {
    const fb::testing::sandbox_t sandbox;
    const std::string index = sandbox.registry();
    sandbox.library("shapes", "shapes", "0.3.0", "", { { "area.fb", "// shapes\n" } });
    const std::string toml = sandbox.read("shapes/project.toml");
    sandbox.write("shapes/project.toml",
                  std::format("{}# {}\n", toml, std::string(std::size_t{ 1 } << 20U, 'c')));
    const std::expected<fb::prepared_package_t, std::string> prepared =
        fb::prepare_package(sandbox / "shapes", index, false);
    REQUIRE_FALSE(prepared);
    CHECK(prepared.error() == "project.toml is more than the registry's 1 MB");
}

TEST_CASE("an archive unpacks to at most 8 times the registry's limit for its kind", "[archive]") {
    // docs/registry-api.md's limits: 10 MB a package's own, 200 MB a target's.
    STATIC_CHECK(fb::most_source_archive == std::uint64_t{ 10 } << 20U);
    STATIC_CHECK(fb::most_target_archive == std::uint64_t{ 200 } << 20U);
    STATIC_CHECK(fb::most_source_unpacked == std::uint64_t{ 80 } << 20U);
    STATIC_CHECK(fb::most_target_unpacked == std::uint64_t{ 1600 } << 20U);

    // A file a little past a package's own limit, which a target's takes, read a piece at a
    // time from the archive's file.
    const fb::testing::sandbox_t sandbox;
    const std::size_t past = (std::size_t{ 80 } << 20U) + 1;
    const std::string zeros =
        archive({ { .path = "native/linux-x64/big.bin", .bytes = std::string(past, '\0') } });
    const std::string source = archive({ { .path = "big.bin", .bytes = std::string(past, '\0') } });
    const std::expected<void, std::string> refused =
        unpack(sandbox, source, sandbox / "cache/own", "source", "x", fb::most_source_unpacked);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().contains("unpacks to more than 80 MB"));
    // Its header's size says so before anything's written.
    CHECK_FALSE(std::filesystem::exists(sandbox / "cache"));
    REQUIRE(unpack(sandbox, zeros, sandbox / "cache/target", "linux-x64", "y",
                   fb::most_target_unpacked));
    CHECK(std::filesystem::file_size(sandbox / "cache/target/native/linux-x64/big.bin") == past);

    // A file's hash, read a piece at a time, is its bytes'.
    CHECK(fb::sha256_file(sandbox / "downloads/archive.fbpkg") == fb::sha256_hex(zeros));
    CHECK(fb::sha256_file(sandbox / "nothing.fbpkg").empty());

    // An archive cut short, or with more after its gzip, unpacks to nothing.
    const std::string small = archive({ { .path = "a.fb", .bytes = "// a\n" } });
    const std::expected<void, std::string> cut =
        unpack(sandbox, small.substr(0, small.size() / 2), sandbox / "cache/cut", "source", "z",
               fb::most_source_unpacked);
    REQUIRE_FALSE(cut);
    CHECK(cut.error().contains("cut short"));
    const std::expected<void, std::string> more = unpack(
        sandbox, small + "more", sandbox / "cache/more", "source", "z", fb::most_source_unpacked);
    REQUIRE_FALSE(more);
    CHECK(more.error().contains("after its gzip stream"));
    CHECK_FALSE(std::filesystem::exists(sandbox / "cache/cut"));
    CHECK_FALSE(std::filesystem::exists(sandbox / "cache/more"));
}
