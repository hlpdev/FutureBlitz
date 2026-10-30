#include "fb/lockfile.hpp"
#include "fb/registry.hpp"
#include "fb/utf8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view hash_a =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view hash_b =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view hash_c =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

/// A package's folder that needn't be there, in the temporary folder, as fb.lock's are.
std::filesystem::path somewhere(const std::string_view name) {
    return std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
           "fb-lockfile-tests" / name;
}

std::string lock_error(const std::string_view text) {
    const std::expected<fb::lockfile_t, std::string> read =
        fb::read_lockfile(text, somewhere("app"));
    return read ? std::string() : read.error();
}

} // namespace

TEST_CASE("fb.lock writes each package, sorted, and reads back the same", "[lockfile]") {
    const std::filesystem::path root = somewhere("app");
    const std::string local = fb::canonical_index("../registry/index", root);
    fb::lockfile_t lockfile;
    lockfile.packages.push_back({
        .name = "shapes",
        .version = "0.3.1",
        .source = local,
        .archive = { .checksum = std::string(hash_b) },
    });
    lockfile.packages.push_back({
        .name = "physics",
        .version = "1.2.3-beta.1",
        .source = "https://index.futureblitz.dev/",
        .archive = { .checksum = std::string(hash_a), .size = 1234 },
        .dependencies = { "shapes", "math" },
    });
    lockfile.packages.back().targets.emplace(
        "windows-x64", fb::locked_archive_t{ .checksum = std::string(hash_c), .size = 99 });
    lockfile.packages.back().targets.emplace(
        "linux-x64", fb::locked_archive_t{ .checksum = std::string(hash_b) });
    const std::string text = fb::write_lockfile(lockfile, root);
    CHECK(text == std::format("# fb.lock: the exact versions this package builds with, which fb "
                              "writes. Don't edit it.\n"
                              "version = 1\n"
                              "\n"
                              "[[package]]\n"
                              "name = \"physics\"\n"
                              "version = \"1.2.3-beta.1\"\n"
                              "source = \"https://index.futureblitz.dev/\"\n"
                              "checksum = \"sha256:{}\"\n"
                              "size = 1234\n"
                              "dependencies = [\"math\", \"shapes\"]\n"
                              "targets = {{ \"linux-x64\" = {{ checksum = \"sha256:{}\" }}, "
                              "\"windows-x64\" = {{ checksum = \"sha256:{}\", size = 99 }} }}\n"
                              "\n"
                              "[[package]]\n"
                              "name = \"shapes\"\n"
                              "version = \"0.3.1\"\n"
                              "source = \"path+../registry/index/\"\n"
                              "checksum = \"sha256:{}\"\n"
                              "dependencies = []\n",
                              hash_a, hash_b, hash_c, hash_b));

    const std::expected<fb::lockfile_t, std::string> read = fb::read_lockfile(text, root);
    REQUIRE(read);
    REQUIRE(read->packages.size() == 2);
    CHECK(read->packages[0].name == "physics");
    CHECK(read->packages[0].archive.size == 1234);
    CHECK(read->packages[0].dependencies == std::vector<std::string>{ "math", "shapes" });
    CHECK(read->packages[0].targets.at("windows-x64") ==
          fb::locked_archive_t{ .checksum = std::string(hash_c), .size = 99 });
    REQUIRE(read->find("shapes") != nullptr);
    CHECK(read->find("shapes")->version == "0.3.1");
    CHECK(read->find("shapes")->source == local);
    CHECK(read->find("nope") == nullptr);
    CHECK(fb::write_lockfile(*read, root) == text);
    CHECK(fb::read_lockfile("version = 1\n", root).value_or(fb::lockfile_t{}).packages.empty());
}

TEST_CASE("fb.lock pins a registry on disk by its folder from the package, not where it is",
          "[lockfile]") {
    // resolve-2 and archive-11: a lock written in one place reads in another, with the
    // registry beside the package there.
    const std::filesystem::path here = somewhere("here/app");
    const std::filesystem::path there = somewhere("there/app");
    fb::lockfile_t lockfile;
    lockfile.packages.push_back({
        .name = "shapes",
        .version = "1.0.0",
        .source = fb::canonical_index("../registry/index", here),
        .archive = { .checksum = std::string(hash_a) },
    });
    const std::string text = fb::write_lockfile(lockfile, here);
    CHECK(text.contains("source = \"path+../registry/index/\""));
    CHECK_FALSE(text.contains("file://"));
    CHECK_FALSE(text.contains(fb::to_utf8(somewhere(""))));

    const std::expected<fb::lockfile_t, std::string> moved = fb::read_lockfile(text, there);
    REQUIRE(moved);
    CHECK(moved->packages[0].source == fb::canonical_index("../registry/index", there));
    CHECK(fb::write_lockfile(*moved, there) == text);
}

TEST_CASE("fb.lock quotes a target's name, so any name a TOML key can't be bare reads back",
          "[lockfile]") {
    // resolve-7: a target's key goes through a TOML string.
    const std::filesystem::path root = somewhere("app");
    fb::lockfile_t lockfile;
    lockfile.packages.push_back({
        .name = "physics",
        .version = "1.0.0",
        .source = "https://index.futureblitz.dev/",
        .archive = { .checksum = std::string(hash_a) },
    });
    lockfile.packages.back().targets.emplace(
        "odd \"target\".x", fb::locked_archive_t{ .checksum = std::string(hash_b), .size = 5 });
    const std::string text = fb::write_lockfile(lockfile, root);
    CHECK(text.contains(R"("odd \"target\".x" = { checksum)"));
    const std::expected<fb::lockfile_t, std::string> read = fb::read_lockfile(text, root);
    INFO((read ? std::string() : read.error()));
    REQUIRE(read);
    CHECK(*read == lockfile);
}

TEST_CASE("an fb.lock fb can't read says why", "[lockfile]") {
    CHECK(lock_error("[[package]\n").contains("isn't valid TOML"));
    CHECK(lock_error("version = 2\n").contains("another format"));
    CHECK(lock_error("").contains("another format"));
    const std::string package = "version = 1\n[[package]]\nname = \"physics\"\nversion = "
                                "\"1.0.0\"\nsource = \"x\"\n";
    CHECK(lock_error(package).contains("without a name, a version, a source, and a checksum"));
    CHECK(lock_error(package + "checksum = \"sha256:abc\"\n").contains("without"));
    CHECK(
        lock_error(std::format("{}checksum = \"md5:{}\"\n", package, hash_a)).contains("without"));
    const std::string whole = std::format("{}checksum = \"sha256:{}\"\n", package, hash_a);
    CHECK(lock_error(whole).empty());
    CHECK(lock_error(whole + "size = -1\n").contains("without"));
    CHECK(lock_error(whole + "size = \"5\"\n").contains("without"));
    CHECK(lock_error(whole + whole.substr(std::string_view("version = 1\n").size()))
              .contains("physics twice"));
    CHECK(lock_error(whole + "targets = { linux-x64 = { checksum = \"nope\" } }\n")
              .contains("a target, linux-x64, without its archive's checksum"));
    CHECK(lock_error(whole + "targets = { linux-x64 = \"sha256:" + std::string(hash_a) + "\" }\n")
              .contains("without its archive's checksum"));
    CHECK(lock_error(whole + "targets = 5\n").contains("targets that aren't a table"));
    CHECK(lock_error(whole + "dependencies = [1]\n").contains("a dependency that isn't a name"));
    CHECK(lock_error(whole + "dependencies = 1\n").contains("dependencies that aren't a list"));
}
