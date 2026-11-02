#pragma once

#include "fb/lsp/features.hpp"
#include "fb/lsp/inbox.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/lsp/toml_features.hpp"
#include "fb/lsp/workspace.hpp"

#include <atomic>
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
    server_t(const server_t&) = delete;
    server_t& operator=(const server_t&) = delete;
    /// Stops the lens thread first, as it reads the workspace.
    ~server_t();

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
    /// A request a worker answers, with the version and text of the open document it's about
    /// as they were when it came, which it's answered about: a document changed since has its
    /// positions elsewhere.
    struct request_job_t {
        json_t id;
        std::string method;
        json_t params;
        std::string key;
        std::optional<std::int64_t> version;
        std::string text;
        /// Set when the client cancels it as it's answered, which it's answered as instead.
        std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    };
    /// A thread that answers requests that need a check, in the order they came, so none of them
    /// keeps the message thread, and what's behind it, waiting. Its thread starts with its first
    /// request. A std::thread, as Apple's libc++ has no std::jthread.
    class worker_t {
    public:
        explicit worker_t(std::function<void(const request_job_t&)> answer)
            : m_answer(std::move(answer)) {}
        worker_t(const worker_t&) = delete;
        worker_t& operator=(const worker_t&) = delete;
        ~worker_t() {
            stop();
        }

        void push(request_job_t job);
        /// Takes a request it hasn't started on out of those waiting, and whether it was there;
        /// the one it's answering is answered as cancelled instead.
        bool cancel(const json_t& id);
        /// Waits until it has answered every request it was given.
        void wait_idle();
        /// Ends its thread once the request it's answering is answered, leaving those waiting.
        void stop();

    private:
        std::function<void(const request_job_t&)> m_answer;
        std::deque<request_job_t> m_jobs;
        std::mutex m_mutex;
        std::condition_variable m_ready;
        bool m_stop = false;
        /// Whether it's answering one, which wait_idle() waits for, with those waiting, and the
        /// one.
        bool m_busy = false;
        const request_job_t* m_current = nullptr;
        std::condition_variable m_idle;
        std::thread m_thread;

        void run();
    };
    /// The inlay hints' settings, which the workers read as a message changes them.
    std::mutex m_settings_mutex;
    /// Whether the client asks for semantic tokens again when told, and the open documents whose
    /// tokens were the lexer's alone, as their check wasn't done, which are told once it is.
    bool m_tokens_refresh = false;
    std::set<std::string> m_lexical_tokens;
    /// Whether the client asks for code lenses again when told, and the open documents changed
    /// since their last check, with the version they changed to, whose inlay hints and lenses are
    /// asked for again once a check of it, or of a later one, is done.
    bool m_lenses_refresh = false;
    std::map<std::string, std::int64_t> m_changed;
    std::mutex m_tokens_mutex;
    /// project.toml's documents and requests, whose thread stops before what it sends through.
    std::unique_ptr<toml_features_t> m_toml;
    /// After what its threads send through, so it's destroyed, and they stop, before that.
    std::unique_ptr<workspace_t> m_workspace;
    /// The workers, which answer the requests that need a check: a quick one for what the last
    /// check answers as code is typed, and a slow one for what waits for the check of the text, so
    /// a hover never waits behind a code action. After the workspace, so they stop before it goes,
    /// as they read its checks.
    worker_t m_quick{ [this](const request_job_t& job) { answer_job(job); } };
    worker_t m_slow{ [this](const request_job_t& job) { answer_job(job); } };
    /// The request a worker's thread is answering, which asked_text() and asked_check() read,
    /// or nullptr on another thread.
    static thread_local const request_job_t* m_answering;

    void send(const json_t& message);
    /// Asks the client to watch every file of `folders`, or of its own folders when it's empty.
    void register_watchers(const std::vector<std::filesystem::path>& folders);
    /// The client's answer to one of the server's own requests.
    void answered(const json_t& response);
    void respond(const json_t& id, json_t result);
    void fail(const json_t& id, error_code_t code, const std::string& message);
    /// Answers a request, or says why it can't.
    void answer_request(const json_t& id, const std::string& method, const json_t& params);
    /// Answers a worker's request, about the text it was asked about.
    void answer_job(const request_job_t& job);
    /// The text of a file, as the request being answered sees it: an open document's as it was
    /// when a worker's request about it came, or else as it is.
    [[nodiscard]] std::optional<std::string> asked_text(const std::filesystem::path& path) const;
    /// The check of a file, as the request being answered sees it: for a worker's request about
    /// an open document, that of the version it was asked about, which throws content_modified_t
    /// once the document has moved past it, or else that of its text now, waited for.
    [[nodiscard]] std::optional<checked_t> asked_check(const std::filesystem::path& path);
    /// The worker that answers a request that needs a check of its document, one about what code
    /// names or means, or nullptr for one about a project.toml, or one its text answers, which
    /// the message thread answers at once.
    [[nodiscard]] worker_t* worker_for(const std::string& method, const json_t& params);
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

    /// An open document's last check that's done, and the document's text now, which may have
    /// moved on from the check's: between the text they start with and the text they end with is
    /// what's changed since, where nothing in one has a place in the other.
    struct last_check_t {
        checked_t checked;
        source_file_t now;
        std::uint32_t prefix = 0;
        std::uint32_t suffix = 0;

        [[nodiscard]] const source_file_t& then() const {
            return checked.compilation->sources.get(checked.file);
        }

        /// Where the document's `offset` is in the check's text, or nullopt in what's changed.
        [[nodiscard]] std::optional<std::uint32_t> to_then(std::uint32_t offset) const;
        /// The same, for an offset whose whole line is unchanged, as what's said of a name there
        /// reads what's before and after it, or nullopt.
        [[nodiscard]] std::optional<std::uint32_t> line_to_then(std::uint32_t offset) const;
        /// Where the check's `offset` is in the document's text, or nullopt in what's changed.
        [[nodiscard]] std::optional<std::uint32_t> to_now(std::uint32_t offset) const;
    };

    /// The last check of the document a request names, without waiting for its current text's,
    /// but for a document no check has finished yet, whose first it waits for.
    std::optional<last_check_t> last_check(const json_t& params);
    /// An LSP range in the check's text as it is in the document's now, in place; false when it
    /// was in what's changed.
    [[nodiscard]] bool moved(const last_check_t& last, json_t& range) const;
    /// What a request answers about a name at its position, from the last check: `answer` with
    /// the check's query and the position in its text, and the ranges of `uri`'s that it gives
    /// moved to the document's text. Null when the position is in what's changed since.
    /// Completion's items from the last check, moved to the document's text: each edit ending at
    /// `cursor`, one starting at `typed_at`, where a '.' was typed since, starting there too, and
    /// one whose other edits were in what's changed left out.
    [[nodiscard]] json_t moved_items(const last_check_t& last, json_t found,
                                     std::optional<std::uint32_t> typed_at,
                                     std::uint32_t cursor) const;
    json_t at_last_check(const json_t& params,
                         const std::function<json_t(const query_t&, std::uint32_t)>& answer);
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
