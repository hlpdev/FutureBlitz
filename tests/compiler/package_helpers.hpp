#pragma once

// What the package manager's tests share: a cache folder of their own, packages written to disk,
// and a registry on disk, in the same files a registry on the network serves.

#include "fb/driver.hpp"
#include "fb/registry.hpp"
#include "fb/registry_api.hpp"
#include "fb/utf8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fb::testing {

/// An HTTP client that never reaches the network, for tests whose registries are on disk, so a
/// mistake can't send them to the internet.
inline std::expected<http_response_t, std::string> offline(const http_request_t& request) {
    FAIL("a test asked the network for " << request.url);
    return std::unexpected("no network in tests");
}

/// Sets the environment variable `name` to `value`, or with nullopt, unsets it.
inline void set_environment(const std::string_view name, const std::optional<std::string>& value) {
#ifdef _WIN32
    const std::wstring wide_name = path_from_utf8(name).wstring();
    const std::wstring wide_value = value ? path_from_utf8(*value).wstring() : std::wstring();
    (void)_wputenv_s(wide_name.c_str(), wide_value.c_str());
#else
    if (value) {
        (void)setenv(std::string(name).c_str(), value->c_str(), 1);
    } else {
        (void)unsetenv(std::string(name).c_str());
    }
#endif
}

/// An environment variable set for as long as it lives, then put back.
class environment_t {
public:
    environment_t(std::string name, const std::optional<std::string>& value)
        : m_name(std::move(name)), m_old(environment_variable(m_name)) {
        set_environment(m_name, value);
    }

    environment_t(const environment_t&) = delete;
    environment_t& operator=(const environment_t&) = delete;
    environment_t(environment_t&&) = delete;
    environment_t& operator=(environment_t&&) = delete;

    // Putting it back is best effort.
    ~environment_t() {
        try {
            set_environment(m_name, m_old);
        } catch (...) {
            return;
        }
    }

private:
    std::string m_name;
    std::optional<std::string> m_old;
};

/// A folder in the temporary folder for a test, with FB_HOME, fb's cache, in it, removed when it
/// goes.
class sandbox_t {
public:
    sandbox_t()
        : m_folder(std::filesystem::weakly_canonical(temporary_path("packages", ""))),
          m_home("FB_HOME", to_utf8(m_folder / "home")) {
        std::filesystem::create_directories(m_folder);
    }

    sandbox_t(const sandbox_t&) = delete;
    sandbox_t& operator=(const sandbox_t&) = delete;
    sandbox_t(sandbox_t&&) = delete;
    sandbox_t& operator=(sandbox_t&&) = delete;

    ~sandbox_t() {
        (void)remove_temporary(m_folder);
    }

    [[nodiscard]] std::filesystem::path operator/(const std::string_view path) const {
        return m_folder / path_from_utf8(path);
    }

    [[nodiscard]] const std::filesystem::path& folder() const {
        return m_folder;
    }

    void write(const std::string_view path, const std::string_view text) const {
        const std::filesystem::path file = m_folder / path_from_utf8(path);
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

    [[nodiscard]] std::string read(const std::string_view path) const {
        const std::ifstream in(m_folder / path_from_utf8(path), std::ios::binary);
        std::ostringstream bytes;
        bytes << in.rdbuf();
        return std::move(bytes).str();
    }

    /// A library package in `folder`, ready to publish: its project.toml, with `dependencies`
    /// as [dependencies]' lines and the registry on disk beside it as `local` in [registries], a
    /// license, a readme, and `files`, by their paths in it.
    void library(const std::string_view folder, const std::string_view name,
                 const std::string_view version, const std::string_view dependencies,
                 const std::vector<std::pair<std::string, std::string>>& files) const {
        write(std::string(folder) + "/project.toml",
              std::string("[project]\nname = \"") + std::string(name) + "\"\nversion = \"" +
                  std::string(version) +
                  "\"\ntype = \"library\"\ndescription = \"A test package\"\nlicense = "
                  "\"MIT\"\n\n[registries]\nlocal = { index = \"../registry/index\" "
                  "}\n\n[dependencies]\n" +
                  std::string(dependencies));
        write(std::string(folder) + "/LICENSE", "MIT License\n");
        write(std::string(folder) + "/README.md", std::string("# ") + std::string(name) + "\n");
        for (const auto& [path, text] : files) {
            write(std::string(folder) + "/" + path, text);
        }
    }

    /// A registry on disk in `folder`: its index's folder, with config.json, and its archives'.
    /// Gives its canonical index.
    [[nodiscard]] std::string registry(const std::string_view folder = "registry") const {
        write(std::string(folder) + "/index/config.json",
              R"({ "v": 1, "dl": "../dl/{name}/{version}/{file}" })");
        std::filesystem::create_directories(m_folder / path_from_utf8(folder) / "dl");
        return canonical_index(std::string(folder) + "/index", m_folder);
    }

    /// Publishes the package in `folder` to the registry on disk at `index`.
    void publish(const std::string_view folder, const std::string_view index) const {
        const std::expected<prepared_package_t, std::string> prepared =
            prepare_package(m_folder / path_from_utf8(folder), index, false);
        INFO((prepared ? std::string() : prepared.error()));
        REQUIRE(prepared);
        const std::expected<std::vector<std::string>, std::string> published =
            publish_package(*prepared, "", { .index = std::string(index), .http = offline });
        INFO((published ? std::string() : published.error()));
        REQUIRE(published);
    }

private:
    std::filesystem::path m_folder;
    environment_t m_home;
};

} // namespace fb::testing
