#pragma once

#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/lsp/workspace.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace fb::lsp {

/// The language server: it takes the client's JSON-RPC messages, answers its requests, and
/// sends its diagnostics, through `send`, which its workspace's threads call too. docs/tooling.md
/// lists what it does.
class server_t {
public:
    using send_t = std::function<void(const json_t& message)>;

    struct options_t {
        workspace_t::options_t workspace;
    };

    explicit server_t(send_t send, options_t options = {});

    /// Handles one message: a request, which it answers before it returns, or a notification.
    void handle(const json_t& message);

    /// Once the client has said exit: what the process exits with, 0 after a shutdown and 1
    /// without one.
    [[nodiscard]] std::optional<int> exit_code() const {
        return m_exit;
    }

    [[nodiscard]] workspace_t& workspace() {
        return *m_workspace;
    }

private:
    send_t m_send;
    std::mutex m_send_mutex;
    positions_t m_positions;
    bool m_initialized = false;
    bool m_shut_down = false;
    std::optional<int> m_exit;
    /// The client's initializationOptions.formatOnSave: whether a save formats the document.
    bool m_format_on_save = false;
    /// Whether the client watches files for the server when it asks.
    bool m_watches = false;
    std::vector<std::filesystem::path> m_folders;
    /// The requests the server sent the client, by id.
    std::int64_t m_next_request = 1;
    /// Each file's declarations for workspace/symbol, with the text they're from.
    std::map<std::string, std::pair<std::string, json_t>> m_symbols;
    /// The standard library's modules, as an Import names them.
    std::vector<std::string> m_std_modules;
    /// Last, so it's destroyed first, and its threads stop before what they send through.
    std::unique_ptr<workspace_t> m_workspace;

    void send(const json_t& message);
    void respond(const json_t& id, json_t result);
    void fail(const json_t& id, error_code_t code, const std::string& message);
    void publish(const published_t& published);

    /// What a request answers, or nullopt for one the server doesn't know.
    std::optional<json_t> answer(const std::string& method, const json_t& params);
    void notified(const std::string& method, const json_t& params);

    json_t initialize(const json_t& params);
    json_t hover(const json_t& params);
    json_t definition(const json_t& params);
    /// What a request about a name is about: the check of its file, the name, its symbol's key,
    /// where it's declared, and the checks that have its every use.
    struct target_t {
        checked_t checked;
        named_t named;
        symbol_key_t key;
        std::optional<std::filesystem::path> declared;
        std::vector<std::shared_ptr<const compilation_t>> compilations;
    };

    /// The name at a request's position, or nullopt when there's none.
    std::optional<target_t> target(const json_t& params);
    /// Why a name can't be renamed, or nullopt when it can.
    [[nodiscard]] std::optional<std::string> unrenamable(const target_t& found) const;
    json_t references(const json_t& params, bool declaration);
    /// Whether a rename can change a file: one of the workspace's, or an open document outside
    /// it, but for std's, unless std is the workspace's.
    [[nodiscard]] bool editable(const std::filesystem::path& file) const;
    json_t prepare_rename(const json_t& params);
    json_t rename(const json_t& params);
    json_t completion(const json_t& params);
    json_t signature_help(const json_t& params);
    json_t document_symbols(const json_t& params);
    json_t workspace_symbols(const json_t& params);
    json_t semantic_tokens(const json_t& params);
    json_t code_actions(const json_t& params);
    json_t formatting(const json_t& params);

    /// The modules an Import in the file at `path` can name.
    std::vector<std::string> import_names(const std::filesystem::path& path,
                                          const compilation_t& compilation);
};

} // namespace fb::lsp
