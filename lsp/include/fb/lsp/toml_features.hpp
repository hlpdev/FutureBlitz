#pragma once

#include "fb/driver.hpp"
#include "fb/http_client.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/registry.hpp"
#include "fb/registry_api.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fb::lsp {

/// Whether a file is a package's project.toml.
[[nodiscard]] bool is_manifest(const std::filesystem::path& path);

/// project.toml in the editor: its open documents, each checked as it's typed as fb build reads
/// it, with the errors published; completion and hover for its tables and keys, from what fb
/// reads, and for its dependencies, from their registries; and quick fixes. docs/tooling.md lists
/// what each does.
class toml_features_t {
public:
    struct options_t {
        /// How long an edited document's check waits for the next edit.
        std::chrono::milliseconds delay{ 150 };
        /// Whether registries are only read from the cache, never the network.
        bool offline = false;
        /// What sends HTTP requests: send_http(), or a test's.
        http_client_t http = send_http;
        /// How long what a registry said is kept before it's asked again.
        std::chrono::seconds registry_life{ 60 };
    };

    using send_t = std::function<void(const json_t& message)>;

    /// `send` takes the notifications it sends, from its own thread too, and `positions` is the
    /// server's, which counts in the client's unit once it's initialized.
    toml_features_t(send_t send, const positions_t& positions, options_t options);
    toml_features_t(const toml_features_t&) = delete;
    toml_features_t& operator=(const toml_features_t&) = delete;
    toml_features_t(toml_features_t&&) = delete;
    toml_features_t& operator=(toml_features_t&&) = delete;
    /// Stops its checks, waiting for the one running.
    ~toml_features_t();

    /// The client's folders, whose packages' builds made a registry package's build step's files.
    void add_folders(const std::vector<std::filesystem::path>& folders);
    void remove_folders(const std::vector<std::filesystem::path>& folders);

    /// Whether the editor has the project.toml at `path` open, so its diagnostics are this one's
    /// to publish, not those of its package's checks, which read it from disk.
    [[nodiscard]] bool owns(const std::filesystem::path& path) const;

    /// An open project.toml's text and version.
    [[nodiscard]] std::optional<std::pair<std::string, std::int64_t>>
    document(const std::filesystem::path& path) const;

    void open(const std::filesystem::path& path, std::int64_t version, std::string text);
    void change(const std::filesystem::path& path, std::int64_t version, std::string text);
    /// A project.toml closed, whose diagnostics are its text's on disk again.
    void close(const std::filesystem::path& path);

    /// Answers textDocument/hover, completion, and codeAction for an open project.toml, or
    /// nullopt for any other request or document.
    [[nodiscard]] std::optional<json_t> answer(const std::string& method, const json_t& params);

    /// Waits until the checks of the documents as they are now are done, for the tests.
    void settle();

private:
    /// What a registry said, and when.
    template <typename Value>
    struct asked_t {
        Value value;
        std::chrono::steady_clock::time_point when;
    };

    struct document_t {
        std::filesystem::path path;
        std::int64_t version = 0;
        std::string text;
        /// Its last check's, for the quick fixes that come with its errors.
        std::shared_ptr<const compilation_t> checked;
    };

    send_t m_send;
    const positions_t& m_positions;
    options_t m_options;
    mutable std::mutex m_mutex;
    std::condition_variable m_work;
    std::condition_variable m_done;
    std::map<std::string, document_t> m_documents;
    /// The documents to check, and closed ones to check from disk once, by when.
    std::map<std::string, std::chrono::steady_clock::time_point> m_due;
    std::map<std::string, std::filesystem::path> m_closed;
    std::vector<std::filesystem::path> m_folders;
    bool m_checking = false;
    bool m_stopping = false;
    /// Registry answers, by index and package name, and by index and search; only the server's
    /// thread asks.
    std::map<std::string, asked_t<std::vector<index_entry_t>>> m_versions;
    std::map<std::string, asked_t<std::vector<found_package_t>>> m_searches;
    std::thread m_thread;

    void run();
    /// Checks one document, or a closed one from disk, and publishes its errors.
    void check(const std::string& key);

    [[nodiscard]] json_t hover(const document_t& document, const json_t& params);
    [[nodiscard]] json_t completion(const document_t& document, const json_t& params);
    [[nodiscard]] json_t code_actions(const document_t& document, const json_t& params);

    /// A package's versions, oldest first, from the registry at `index`, a canonical one, or the
    /// cache's offline or when it can't be reached. Empty when neither has it.
    const std::vector<index_entry_t>& versions(const std::string& index, const std::string& name);
    /// The registry's packages whose names start with `prefix`, the closest first, or the
    /// cache's offline or when it can't be reached.
    const std::vector<found_package_t>& search(const std::string& index, const std::string& prefix);
};

} // namespace fb::lsp
