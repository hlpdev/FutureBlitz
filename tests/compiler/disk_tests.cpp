#include "fb/disk.hpp"
#include "fb/driver.hpp"
#include "fb/process.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

} // namespace

TEST_CASE("a disk cache answers as the disk did when it was first asked, until it's cleared",
          "[disk]") {
    const std::filesystem::path folder = fb::temporary_path("disk", "");
    std::filesystem::create_directories(folder);
    const std::filesystem::path file = folder / "a.fb";
    std::error_code error;

    // Without one, the disk answers each time.
    CHECK(!fb::disk::exists(file, error));
    write(file, "one");
    CHECK(fb::disk::exists(file, error));
    CHECK(fb::disk::read(file) == "one");
    std::filesystem::remove(file);

    fb::disk_cache_t cache;
    {
        const fb::disk_scope_t scope(&cache);
        CHECK(!fb::disk::exists(file, error));
        CHECK(!fb::disk::read(file));
        CHECK(fb::disk::entries(folder, error)->empty());
        write(file, "two");
        CHECK(!fb::disk::exists(file, error));
        CHECK(!fb::disk::is_regular_file(file, error));
        CHECK(!fb::disk::read(file));
        CHECK(fb::disk::entries(folder, error)->empty());

        cache.clear();
        CHECK(fb::disk::is_regular_file(file, error));
        CHECK(fb::disk::read(file) == "two");
        const std::vector<fb::disk_entry_t> entries = *fb::disk::entries(folder, error);
        REQUIRE(entries.size() == 1);
        CHECK(entries.front().path.filename() == "a.fb");
        CHECK(entries.front().file);
        CHECK(!entries.front().folder);
        write(file, "three");
        CHECK(fb::disk::read(file) == "two");

        // A scope without a cache of its own uses the one in use.
        {
            const fb::disk_scope_t inner;
            CHECK(fb::disk::read(file) == "two");
        }

        CHECK(fb::disk::read(file) == "two");
    }

    // Out of its scope, the disk answers again, and a scope of its own starts empty.
    CHECK(fb::disk::read(file) == "three");
    {
        const fb::disk_scope_t own;
        CHECK(fb::disk::read(file) == "three");
        write(file, "four");
        CHECK(fb::disk::read(file) == "three");
    }

    CHECK(fb::disk::read(file) == "four");
    fb::remove_temporary(folder);
}

TEST_CASE("a disk cache keeps no answer it was emptied while asking for", "[disk]") {
    fb::disk_cache_t cache;
    const fb::disk_cache_t::key_t key = std::filesystem::path("a.fb").native();
    int asked = 0;
    const auto ask = [&] {
        ++asked;
        return std::make_shared<const std::optional<std::string>>("old");
    };

    // The disk changed while it was asked, so the old answer is the caller's alone.
    CHECK(*cache.remember(&fb::disk_cache_t::texts, key, [&] {
        cache.clear();
        return ask();
    }) == "old");
    CHECK(cache.texts.empty());
    CHECK(*cache.remember(&fb::disk_cache_t::texts, key, ask) == "old");
    CHECK(*cache.remember(&fb::disk_cache_t::texts, key, ask) == "old");
    CHECK(asked == 2);
    CHECK(cache.texts.size() == 1);
}

TEST_CASE("a disk cache's answers are std::filesystem's", "[disk]") {
    const std::filesystem::path folder = fb::temporary_path("disk", "");
    std::filesystem::create_directories(folder / "sub");
    const std::filesystem::path file = folder / "sub" / "a.fb";
    write(file, "text");
    const fb::disk_scope_t scope;
    std::error_code error;

    // A path that isn't there isn't an error for exists(), and is for the rest.
    const std::filesystem::path missing = folder / "missing";
    CHECK(!fb::disk::exists(missing, error));
    CHECK(!error);
    CHECK(!fb::disk::is_regular_file(missing, error));
    CHECK(error);
    CHECK(fb::disk::type(missing, error) == std::filesystem::file_type::not_found);

    CHECK(fb::disk::is_directory(folder / "sub", error));
    CHECK(!error);
    CHECK(fb::disk::weakly_canonical(folder / "sub" / ".." / "sub" / "a.fb", error) ==
          std::filesystem::weakly_canonical(file));
    CHECK(fb::disk::equivalent(file, folder / "sub" / "." / "a.fb", error));
    CHECK(!fb::disk::equivalent(file, folder / "sub", error));
    CHECK(fb::disk::real_path(file) == fb::real_path(file));
    CHECK(!fb::disk::real_path(missing));
    CHECK(fb::disk::files_under(folder) == std::vector<std::filesystem::path>{ file });
    CHECK(!fb::disk::read(folder / "sub"));

    fb::remove_temporary(folder);
}
