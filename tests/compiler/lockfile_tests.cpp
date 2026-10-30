#include "fb/lockfile.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <string>
#include <string_view>

namespace {

const std::string hash_a(64, 'a');
const std::string hash_b(64, 'b');
const std::string hash_c(64, 'c');

std::string lock_error(const std::string_view text) {
    const std::expected<fb::lockfile_t, std::string> read = fb::read_lockfile(text);
    return read ? std::string() : read.error();
}

} // namespace

TEST_CASE("fb.lock writes each package, sorted, and reads back the same", "[lockfile]") {
    const fb::lockfile_t lockfile{ .packages = {
                                       {
                                           .name = "shapes",
                                           .version = "0.3.1",
                                           .source = "file:///C:/registry/index/",
                                           .checksum = hash_b,
                                       },
                                       {
                                           .name = "physics",
                                           .version = "1.2.3-beta.1",
                                           .source = "https://index.futureblitz.dev/",
                                           .checksum = hash_a,
                                           .dependencies = { "shapes", "math" },
                                           .targets = { { "windows-x64", hash_c },
                                                        { "linux-x64", hash_b } },
                                       },
                                   } };
    const std::string text = fb::write_lockfile(lockfile);
    CHECK(text == "# fb.lock: the exact versions this package builds with, which fb writes. "
                  "Don't edit it.\n"
                  "version = 1\n"
                  "\n"
                  "[[package]]\n"
                  "name = \"physics\"\n"
                  "version = \"1.2.3-beta.1\"\n"
                  "source = \"https://index.futureblitz.dev/\"\n"
                  "checksum = \"sha256:" +
                      hash_a +
                      "\"\n"
                      "dependencies = [\"math\", \"shapes\"]\n"
                      "targets = { linux-x64 = \"sha256:" +
                      hash_b + "\", windows-x64 = \"sha256:" + hash_c +
                      "\" }\n"
                      "\n"
                      "[[package]]\n"
                      "name = \"shapes\"\n"
                      "version = \"0.3.1\"\n"
                      "source = \"file:///C:/registry/index/\"\n"
                      "checksum = \"sha256:" +
                      hash_b +
                      "\"\n"
                      "dependencies = []\n");

    const std::expected<fb::lockfile_t, std::string> read = fb::read_lockfile(text);
    REQUIRE(read);
    REQUIRE(read->packages.size() == 2);
    CHECK(read->packages[0].name == "physics");
    CHECK(read->packages[0].dependencies == std::vector<std::string>{ "math", "shapes" });
    CHECK(read->packages[0].targets.at("windows-x64") == hash_c);
    CHECK(read->find("shapes")->version == "0.3.1");
    CHECK(read->find("nope") == nullptr);
    CHECK(fb::write_lockfile(*read) == text);
    CHECK(fb::read_lockfile("version = 1\n")->packages.empty());
}

TEST_CASE("an fb.lock fb can't read says why", "[lockfile]") {
    CHECK(lock_error("[[package]\n").contains("isn't valid TOML"));
    CHECK(lock_error("version = 2\n").contains("another format"));
    CHECK(lock_error("").contains("another format"));
    const std::string package = "version = 1\n[[package]]\nname = \"physics\"\nversion = "
                                "\"1.0.0\"\nsource = \"x\"\n";
    CHECK(lock_error(package).contains("without a name, a version, a source, and a checksum"));
    CHECK(lock_error(package + "checksum = \"sha256:abc\"\n").contains("without"));
    CHECK(lock_error(package + "checksum = \"md5:" + hash_a + "\"\n").contains("without"));
    const std::string whole = package + "checksum = \"sha256:" + hash_a + "\"\n";
    CHECK(lock_error(whole).empty());
    CHECK(lock_error(whole + whole.substr(std::string_view("version = 1\n").size()))
              .contains("physics twice"));
    CHECK(lock_error(whole + "targets = { linux-x64 = \"nope\" }\n").contains("without"));
    CHECK(lock_error(whole + "dependencies = [1]\n").contains("without"));
}
