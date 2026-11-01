#pragma once

#include "fb/lsp/features.hpp"
#include "fb/lsp/inbox.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/lsp/toml_features.hpp"
#include "fb/lsp/workspace.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
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
        toml_features_t::options_t toml;
    };

    explicit server_t(send_t send, options_t options = {});

    /// Handles one message: a request, which it answers before it returns, or a notification.
    void handle(const json_t& message);

    /// The messages waiting behind the one being handled, which it looks at: a request whose
    /// $/cancelRequest waits is cancelled, and a code lens of a document whose change waits is
    /// stale. Without one, each message is handled as it comes. A code lens's resolve is answered
    /// on a thread of its own, after handle() returns, so no edit waits on its check.
    void set_inbox(inbox_t* inbox) {
        m_inbox = inbox;
    }

    /// Once the client has said exit: what the process exits with, 0 after a shutdown and 1
    /// without one.
    [[nodiscard]] std::optional<int> exit_code() const {
        return m_exit;
    }

    [[nodiscard]] workspace_t& workspace() {
        return *m_workspace;
    }

    [[nodiscard]] toml_features_t& toml() {
        return *m_toml;
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
    /// The inlay hints the client's settings ask for, and whether it can be asked to ask for them
    /// again once they change.
    inlay_options_t m_inlay;
    bool m_inlay_refresh = false;
    /// The client's initializationOptions.tests: whether it's sent each file's @Test Functions as
    /// the checks find them, as fb/tests.
    bool m_tests = false;
    /// Whether the client watches files for the server when it asks, and whether a watcher can
    /// have a folder of its own, a RelativePattern.
    bool m_watches = false;
    bool m_relative = false;
    /// The registrations of watchers the client hasn't answered, by their requests' ids: the
    /// folders of RelativePattern ones, or none for the client's folders'.
    std::map<std::string, std::vector<std::filesystem::path>> m_registering;
    std::mutex m_watch_mutex;
    std::vector<std::filesystem::path> m_folders;
    /// The requests the server sent the client, by id.
    std::int64_t m_next_request = 1;
    /// Each file's declarations for workspace/symbol, with the text they're from.
    std::map<std::string, std::pair<std::string, json_t>> m_symbols;
    /// The standard library's modules, as an Import names them.
    std::vector<std::string> m_std_modules;
    inbox_t* m_inbox = nullptr;
    /// For each open document, by its key: the check of it and what imports it, made once for a
    /// version of it, which references, renames, the hierarchies, and every one of its code
    /// lenses share, and the version. A change of a document's text, a file's on disk, or the
    /// folders empties it, but a save doesn't, as the checks read the editor's text.
    struct dependents_t {
        std::int64_t version = 0;
        std::shared_future<std::vector<std::shared_ptr<const compilation_t>>> compilations;
        /// When its check started: without the client watching files, one of its files written
        /// since makes it stale.
        std::filesystem::file_time_type checked;
    };
    std::map<std::string, dependents_t> m_dependents;
    std::mutex m_dependents_mutex;
    /// The code lens resolves waiting for the lens thread, each with its request's id.
    struct lens_job_t {
        json_t id;
        json_t lens;
    };
    std::deque<lens_job_t> m_lens_jobs;
    std::mutex m_lens_mutex;
    std::condition_variable_any m_lens_ready;
    /// project.toml's documents and requests, whose thread stops before what it sends through.
    std::unique_ptr<toml_features_t> m_toml;
    /// After what its threads send through, so it's destroyed, and they stop, before that.
    std::unique_ptr<workspace_t> m_workspace;
    /// After the workspace, so it stops first, as it reads the workspace's checks: the thread
    /// that answers code lens resolves, started with the first.
    std::jthread m_lens_thread;

    void send(const json_t& message);
    /// Asks the client to watch every file of `folders`, or of its own folders when it's empty.
    void register_watchers(const std::vector<std::filesystem::path>& folders);
    /// The client's answer to one of the server's own requests.
    void answered(const json_t& response);
    void respond(const json_t& id, json_t result);
    void fail(const json_t& id, error_code_t code, const std::string& message);
    /// Answers a request, or says why it can't.
    void answer_request(const json_t& id, const std::string& method, const json_t& params);
    /// The lens thread's work: each waiting resolve answered, until it's asked to stop.
    void answer_lenses(const std::stop_token& stop);
    /// The checks of `file` and what imports it, which every use of what it declares is in, made
    /// once for each version of an open document.
    std::vector<std::shared_ptr<const compilation_t>> dependents(const std::filesystem::path& file);
    /// m_dependents emptied.
    void forget_dependents();
    /// Whether a file of `compilation` that isn't open, or std's, was written after `since`, or
    /// is gone.
    [[nodiscard]] bool written_since(const compilation_t& compilation,
                                     std::filesystem::file_time_type since) const;
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
    json_t folding_ranges(const json_t& params);
    json_t selection_ranges(const json_t& params);
    json_t document_highlights(const json_t& params);
    json_t inlay_hints(const json_t& params);
    json_t type_definition(const json_t& params);
    json_t implementations(const json_t& params);
    /// textDocument/prepareCallHierarchy, callHierarchy/incomingCalls, and outgoingCalls.
    json_t call_hierarchy(const std::string& method, const json_t& params);
    /// textDocument/prepareTypeHierarchy, typeHierarchy/supertypes, and subtypes.
    json_t type_hierarchy(const std::string& method, const json_t& params);
    json_t code_lenses(const json_t& params);
    /// fb/changeSignature: a Function's or Method's parameters, or the edits that change them.
    json_t change_signature(const json_t& params);
    /// `fixes` as a WorkspaceEdit, or nullopt when one isn't the workspace's to change.
    [[nodiscard]] std::optional<json_t> workspace_edit(const compilation_t& compilation,
                                                       const std::vector<fix_t>& fixes) const;
    json_t resolve_code_lens(json_t lens);
    /// fb/tests: every file's @Test Functions the checks have found.
    json_t tests();
    /// A file's @Test Functions, as fb/tests has them.
    [[nodiscard]] json_t tests_json(const tests_found_t& found) const;
    /// Takes the client's inlay hint settings, {"types", "parameterNames"}.
    void set_inlay(const json_t& settings);

    /// The modules an Import in the file at `path` can name.
    std::vector<std::string> import_names(const std::filesystem::path& path,
                                          const compilation_t& compilation);
};

} // namespace fb::lsp
