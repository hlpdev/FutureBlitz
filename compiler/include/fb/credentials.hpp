#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace fb {

/// Where fb keeps registries' tokens, by a key naming each registry.
class credential_store_t {
public:
    credential_store_t() = default;
    credential_store_t(const credential_store_t&) = delete;
    credential_store_t& operator=(const credential_store_t&) = delete;
    credential_store_t(credential_store_t&&) = delete;
    credential_store_t& operator=(credential_store_t&&) = delete;
    virtual ~credential_store_t() = default;

    /// The token kept under `key`, or nullopt when there's none. Fails when the store can't be
    /// reached.
    [[nodiscard]] virtual std::expected<std::optional<std::string>, std::string>
    get(std::string_view key) = 0;

    /// Keeps `token` under `key`, in place of what was there.
    [[nodiscard]] virtual std::expected<void, std::string> set(std::string_view key,
                                                               std::string_view token) = 0;

    /// Removes what's under `key`; true when there was something.
    [[nodiscard]] virtual std::expected<bool, std::string> remove(std::string_view key) = 0;
};

/// The OS's own store: Windows' Credential Manager, the macOS keychain, or on Linux the Secret
/// Service, through libsecret, which fails as it's used when the machine has none.
[[nodiscard]] std::unique_ptr<credential_store_t> os_credential_store();

/// The key a registry's token is kept under: "futureblitz:" and its canonical index.
[[nodiscard]] std::string credential_key(std::string_view index);

} // namespace fb
