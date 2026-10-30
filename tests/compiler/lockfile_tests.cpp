#include "fb/lockfile.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
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

std::string lock_error(const std::string_view text) {
    const std::expected<fb::lockfile_t, std::string> read = fb::read_lockfile(text);
    return read ? std::string() : read.error();
}

} // namespace

TEST_CASE("fb.lock writes each package, sorted, and reads back the same", "[lockfile]") {
    fb::lockfile_t lockfile;
    lockfile.packages.push_back({
        .name = "shapes",
        .version = "0.3.1",
        .source = "file:///C:/registry/index/",
        .checksum = std::string(hash_b),
    });
    lockfile.packages.push_back({
        .name = "physics",
        .version = "1.2.3-beta.1",
        .source = "https://index.futureblitz.dev/",
        .checksum = std::string(hash_a),
        .dependencies = { "shapes", "math" },
    });
    lockfile.packages.back().targets.emplace("windows-x64", hash_c);
    lockfile.packages.back().targets.emplace("linux-x64", hash_b);
    const std::string text = fb::write_lockfile(lockfile);
    CHECK(text == std::format("# fb.lock: the exact versions this package builds with, which fb "
                              "writes. Don't edit it.\n"
                              "version = 1\n"
                              "\n"
                              "[[package]]\n"
                              "name = \"physics\"\n"
                              "version = \"1.2.3-beta.1\"\n"
                              "source = \"https://index.futureblitz.dev/\"\n"
                              "checksum = \"sha256:{}\"\n"
                              "dependencies = [\"math\", \"shapes\"]\n"
                              "targets = {{ linux-x64 = \"sha256:{}\", windows-x64 = "
                              "\"sha256:{}\" }}\n"
                              "\n"
                              "[[package]]\n"
                              "name = \"shapes\"\n"
                              "version = \"0.3.1\"\n"
                              "source = \"file:///C:/registry/index/\"\n"
                              "checksum = \"sha256:{}\"\n"
                              "dependencies = []\n",
                              hash_a, hash_b, hash_c, hash_b));

    const std::expected<fb::lockfile_t, std::string> read = fb::read_lockfile(text);
    REQUIRE(read);
    REQUIRE(read->packages.size() == 2);
    CHECK(read->packages[0].name == "physics");
    CHECK(read->packages[0].dependencies == std::vector<std::string>{ "math", "shapes" });
    CHECK(read->packages[0].targets.at("windows-x64") == hash_c);
    REQUIRE(read->find("shapes") != nullptr);
    CHECK(read->find("shapes")->version == "0.3.1");
    CHECK(read->find("nope") == nullptr);
    CHECK(fb::write_lockfile(*read) == text);
    CHECK(fb::read_lockfile("version = 1\n").value_or(fb::lockfile_t{}).packages.empty());
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
    CHECK(lock_error(whole + whole.substr(std::string_view("version = 1\n").size()))
              .contains("physics twice"));
    CHECK(lock_error(whole + "targets = { linux-x64 = \"nope\" }\n").contains("without"));
    CHECK(lock_error(whole + "dependencies = [1]\n").contains("without"));
}
