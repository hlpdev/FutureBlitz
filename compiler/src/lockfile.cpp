#include "fb/lockfile.hpp"

#include "fb/manifest.hpp"
#include "fb/semver.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// Whether text is 64 lowercase hex digits, a SHA-256.
bool is_sha256(const std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](const char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

/// A TOML basic string of `text`, with what it can't hold as is escaped.
std::string toml_string(const std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            out += c;
        }
    }

    return out + "\"";
}

/// A hash as fb.lock writes it, "sha256:" and its digits, read, or nullopt.
std::optional<std::string> hash_of(const toml::node* node) {
    const std::optional<std::string> text =
        node != nullptr ? node->value<std::string>() : std::nullopt;
    if (!text || !text->starts_with("sha256:") || !is_sha256(text->substr(7))) {
        return std::nullopt;
    }

    return text->substr(7);
}

} // namespace

const locked_package_t* lockfile_t::find(const std::string_view name) const {
    const auto found = std::ranges::find(packages, name, &locked_package_t::name);
    return found != packages.end() ? &*found : nullptr;
}

std::string write_lockfile(lockfile_t lockfile) {
    std::ranges::sort(lockfile.packages, {}, &locked_package_t::name);
    std::string out = std::format("# fb.lock: the exact versions this package builds with, which "
                                  "fb writes. Don't edit it.\nversion = {}\n",
                                  lockfile_version);
    for (locked_package_t& package : lockfile.packages) {
        std::ranges::sort(package.dependencies);
        out += std::format("\n[[package]]\nname = {}\nversion = {}\nsource = {}\nchecksum = "
                           "\"sha256:{}\"\n",
                           toml_string(package.name), toml_string(package.version),
                           toml_string(package.source), package.checksum);
        out += "dependencies = [";
        for (std::size_t i = 0; i < package.dependencies.size(); ++i) {
            out += std::format("{}{}", i > 0 ? ", " : "", toml_string(package.dependencies[i]));
        }

        out += "]\n";
        if (!package.targets.empty()) {
            out += "targets = { ";
            bool first = true;
            for (const auto& [target, checksum] : package.targets) {
                out += std::format("{}{} = \"sha256:{}\"", first ? "" : ", ", target, checksum);
                first = false;
            }

            out += " }\n";
        }
    }

    return out;
}

std::expected<lockfile_t, std::string> read_lockfile(const std::string_view text) {
    toml::parse_result parsed = toml::parse(text, std::string_view("fb.lock"));
    if (!parsed) {
        return std::unexpected(
            std::format("fb.lock isn't valid TOML: {}", parsed.error().description()));
    }

    const toml::table& root = parsed.table();
    const std::optional<std::int64_t> version = root["version"].value_exact<std::int64_t>();
    if (version != lockfile_version) {
        return std::unexpected(std::format(
            "fb.lock is of another format than this fb's, version {}; fb update writes it anew",
            lockfile_version));
    }

    lockfile_t lockfile;
    const toml::array* packages = root["package"].as_array();
    if (packages == nullptr) {
        return lockfile;
    }

    for (const toml::node& node : *packages) {
        const toml::table* table = node.as_table();
        const std::string wrong = "fb.lock has a package without a name, a version, a source, "
                                  "and a checksum; fb update writes it anew";
        if (table == nullptr) {
            return std::unexpected(wrong);
        }

        locked_package_t package;
        const std::optional<std::string> name = (*table)["name"].value<std::string>();
        const std::optional<std::string> version_text = (*table)["version"].value<std::string>();
        const std::optional<std::string> source = (*table)["source"].value<std::string>();
        const std::optional<std::string> checksum = hash_of(table->get("checksum"));
        if (!name || package_name_error(*name) || !version_text || !parse_version(*version_text) ||
            !source || !checksum) {
            return std::unexpected(wrong);
        }

        package.name = *name;
        package.version = *version_text;
        package.source = *source;
        package.checksum = *checksum;
        if (const toml::array* dependencies = (*table)["dependencies"].as_array()) {
            for (const toml::node& dependency : *dependencies) {
                const std::optional<std::string> each = dependency.value<std::string>();
                if (!each) {
                    return std::unexpected(wrong);
                }

                package.dependencies.push_back(*each);
            }
        }

        if (const toml::table* targets = (*table)["targets"].as_table()) {
            for (const auto& [target, hash] : *targets) {
                const std::optional<std::string> each = hash_of(&hash);
                if (!each) {
                    return std::unexpected(wrong);
                }

                package.targets.emplace(std::string(target.str()), *each);
            }
        }

        if (lockfile.find(package.name) != nullptr) {
            return std::unexpected(
                std::format("fb.lock has {} twice; fb update writes it anew", package.name));
        }

        lockfile.packages.push_back(std::move(package));
    }

    std::ranges::sort(lockfile.packages, {}, &locked_package_t::name);
    return lockfile;
}

} // namespace fb
