#include "fb/lsp/server.hpp"

#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/lsp/features.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/lsp/refactor.hpp"
#include "fb/lsp/toml_features.hpp"
#include "fb/lsp/workspace.hpp"
#include "fb/module.hpp"
#include "fb/source.hpp"
#include "fb/toolchain.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace fb::lsp {
namespace {

/// What a request is about changed since the client asked it, so it's the client's to ask again.
class content_modified_t : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A request on the quick worker needs a check that isn't done, which the slow one waits for.
class needs_check_t : public std::runtime_error {
public:
    needs_check_t() : std::runtime_error("it needs a check that isn't done") {}
};

/// The check a request waits for didn't finish by its deadline.
class past_deadline_t : public std::runtime_error {
public:
    past_deadline_t() : std::runtime_error("the check it needs didn't finish in time") {}
};

/// The file a request's textDocument names. nullopt for a URI that isn't a file's.
std::optional<std::filesystem::path> document_path(const json_t& params) {
    return path_of(params.at("textDocument").at("uri").get<std::string>());
}

/// The dotted name an Import gives a module at `path`, a file or a folder, from `root`, its
/// package's folder, whose own modules start with `prefix`.
std::string dotted(const std::filesystem::path& root, const std::filesystem::path& path,
                   const std::string& prefix) {
    std::string name = prefix;
    std::filesystem::path relative = path.lexically_relative(root);
    if (relative.extension() == ".fb") {
        relative.replace_extension();
    }

    for (const std::filesystem::path& part : relative) {
        name += (name.empty() ? "" : ".") + to_utf8(part);
    }

    return name;
}

/// The names an Import gives a unit's files and their folders.
void unit_modules(const unit_t& unit, const std::string& prefix, std::set<std::string>& out) {
    for (const std::filesystem::path& file : unit.files) {
        out.insert(dotted(unit.root, file, prefix));
        std::string folder = prefix;
        for (const std::filesystem::path& part : file.parent_path().lexically_relative(unit.root)) {
            if (part == "." || part == "..") {
                break;
            }

            folder += "." + to_utf8(part);
            out.insert(folder);
        }
    }
}

/// Whether a file is FutureBlitz code: a .fb file, in any letter case.
bool is_fb_file(const std::filesystem::path& path) {
    std::string extension = to_utf8(path.extension());
    std::ranges::transform(extension, extension.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return extension == ".fb";
}

/// `object`'s field `key` when it's an object, or else an empty object.
json_t object_at(const json_t& object, const char* key) {
    if (object.is_object() && object.contains(key) && object[key].is_object()) {
        return object[key];
    }

    return json_t::object();
}

/// What a hierarchy's item is for, from its data, and the file it's in. nullopt for an item that
/// isn't the server's.
std::optional<std::pair<symbol_key_t, std::filesystem::path>> item_of(const json_t& item) {
    const json_t data = item.value("data", json_t());
    const std::optional<std::filesystem::path> path =
        item.contains("uri") ? path_of(item["uri"].get<std::string>()) : std::nullopt;
    if (!path || !data.is_object()) {
        return std::nullopt;
    }

    return std::pair(
        symbol_key_t{
            .file = data.value("file", std::string()),
            .begin = data.value("begin", std::uint32_t{ 0 }),
            .name = data.value("name", std::string()),
        },
        *path);
}

/// `text` with one of didChange's contentChanges made.
void apply_change(std::string& text, const json_t& change, const positions_t& positions) {
    const std::string replacement = change.at("text").get<std::string>();
    if (!change.contains("range")) {
        text = replacement;
        return;
    }

    // A source file drops a UTF-8 BOM, so its offsets start after one.
    const source_file_t file(file_id_t{}, "", text);
    const std::uint32_t bom = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
    const std::uint32_t begin = positions.offset(file, change["range"].at("start"));
    const std::uint32_t end = std::max(begin, positions.offset(file, change["range"].at("end")));
    text.replace(bom + begin, end - begin, replacement);
}

} // namespace

server_t::server_t(send_t send, const options_t& options)
    : m_send(std::move(send)), m_request_deadline(options.request_deadline) {
    m_toml = std::make_unique<toml_features_t>(
        [this](const json_t& message) { this->send(message); }, m_positions, options.toml);
    m_workspace = std::make_unique<workspace_t>(
        [this](const published_t& published) { publish(published); }, options.workspace);
    // The folders of packages the checks found outside the client's, which only a watcher of
    // their own sees.
    m_workspace->on_outside([this](const std::vector<std::filesystem::path>& folders) {
        if (m_watches && m_relative) {
            register_watchers(folders);
        }
    });
    // Each file's @Test Functions, for the client's Testing panel.
    m_workspace->on_tests([this](const tests_found_t& found) {
        if (m_tests) {
            this->send({
                { "jsonrpc", "2.0" },
                { "method", "fb/tests" },
                { "params", tests_json(found) },
            });
        }
    });
}

server_t::~server_t() {
    m_quick.stop();
    m_slow.stop();
}

void server_t::register_watchers(const std::vector<std::filesystem::path>& folders) {
    json_t watchers = json_t::array();
    if (folders.empty()) {
        watchers.push_back({ { "globPattern", "**/*" } });
    }

    for (const std::filesystem::path& folder : folders) {
        watchers.push_back(
            { { "globPattern", { { "baseUri", uri_of(folder) }, { "pattern", "**/*" } } } });
    }

    std::string id;
    {
        const std::scoped_lock lock(m_watch_mutex);
        id = std::format("fb-{}", m_next_request++);
        m_registering[id] = folders;
    }

    send({
        { "jsonrpc", "2.0" },
        { "id", id },
        { "method", "client/registerCapability" },
        {
            "params",
            {
                {
                    "registrations",
                    json_t::array({
                        {
                            { "id", id },
                            { "method", "workspace/didChangeWatchedFiles" },
                            { "registerOptions", { { "watchers", std::move(watchers) } } },
                        },
                    }),
                },
            },
        },
    });
}

void server_t::answered(const json_t& response) {
    if (!response.contains("id") || !response["id"].is_string()) {
        return;
    }

    std::optional<std::vector<std::filesystem::path>> folders;
    {
        const std::scoped_lock lock(m_watch_mutex);
        const auto found = m_registering.find(response["id"].get<std::string>());
        if (found == m_registering.end()) {
            return;
        }

        if (!response.contains("error")) {
            folders = std::move(found->second);
        }

        m_registering.erase(found);
    }

    // The checks share what they know of the disk only where the client said it watches.
    if (folders && folders->empty()) {
        m_workspace->watched(true);
    } else if (folders) {
        m_workspace->watching(*folders);
    }
}

void server_t::send(const json_t& message) {
    const std::scoped_lock lock(m_send_mutex);
    m_send(message);
}

void server_t::respond(const json_t& id, json_t result) {
    send({ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } });
}

void server_t::fail(const json_t& id, const error_code_t code, const std::string& message) {
    send({
        { "jsonrpc", "2.0" },
        { "id", id },
        { "error", { { "code", static_cast<std::int32_t>(code) }, { "message", message } } },
    });
}

void server_t::publish(const published_t& published) {
    // An open project.toml's are its text's as it's typed, not those of the file on disk.
    if (m_toml->owns(published.path)) {
        return;
    }

    // Once a document's check is done, what was shown of it before, the lexer's tokens, and the
    // last check's inlay hints and lenses, the client asks for again.
    std::vector<std::string> refreshed;
    if (published.checked) {
        const std::scoped_lock lock(m_tokens_mutex);
        const std::string key = file_key(published.path);
        if (m_lexical_tokens.erase(key) > 0 && m_tokens_refresh) {
            refreshed.emplace_back("workspace/semanticTokens/refresh");
        }

        if (const auto changed = m_changed.find(key);
            changed != m_changed.end() &&
            (!published.version || *published.version >= changed->second)) {
            m_changed.erase(changed);
            if (m_inlay_refresh) {
                refreshed.emplace_back("workspace/inlayHint/refresh");
            }

            if (m_lenses_refresh) {
                refreshed.emplace_back("workspace/codeLens/refresh");
            }
        }
    }

    for (const std::string& method : refreshed) {
        std::string id;
        {
            const std::scoped_lock lock(m_watch_mutex);
            id = std::format("fb-{}", m_next_request++);
        }

        send({ { "jsonrpc", "2.0" }, { "id", id }, { "method", method } });
    }

    json_t diagnostics = json_t::array();
    for (const diagnostic_t* diagnostic : published.diagnostics) {
        diagnostics.push_back(diagnostic_json(*published.compilation, *diagnostic, m_positions));
    }

    json_t params{ { "uri", uri_of(published.path) }, { "diagnostics", std::move(diagnostics) } };
    if (published.version) {
        params["version"] = *published.version;
    }

    send({
        { "jsonrpc", "2.0" },
        { "method", "textDocument/publishDiagnostics" },
        { "params", std::move(params) },
    });
}

void server_t::handle(const json_t& message) {
    if (!message.is_object()) {
        fail(nullptr, error_code_t::INVALID_REQUEST, "a message is a JSON object");
        return;
    }

    // A response to one of the server's own requests.
    if (!message.contains("method")) {
        answered(message);
        return;
    }

    const std::string method =
        message["method"].is_string() ? message["method"].get<std::string>() : std::string();
    const json_t params = message.value("params", json_t::object());
    if (!message.contains("id")) {
        // A notification has no answer to say what's wrong with it, so the client's log does.
        try {
            notified(method, params);
        } catch (const std::exception& error) {
            send({
                { "jsonrpc", "2.0" },
                { "method", "window/logMessage" },
                { "params", { { "type", 1 }, { "message", method + ": " + error.what() } } },
            });
        }

        return;
    }

    const json_t& id = message["id"];
    if (!m_initialized && method != "initialize") {
        fail(id, error_code_t::SERVER_NOT_INITIALIZED, "the server isn't initialized yet");
        return;
    }

    if (m_initialized && method == "initialize") {
        fail(id, error_code_t::INVALID_REQUEST, "the server is initialized already");
        return;
    }

    if (m_shut_down) {
        fail(id, error_code_t::INVALID_REQUEST, "the server has shut down");
        return;
    }

    // What waits for a check goes to a worker, so the message thread answers what doesn't at
    // once, formatting a save among them, and reads on.
    if (worker_t* worker = worker_for(method, params)) {
        request_job_t job{
            .id = id,
            .method = method,
            .params = params,
            .deadline = std::chrono::steady_clock::now() + m_request_deadline,
            .quick = worker == &m_quick,
        };
        const json_t document =
            params.is_object() ? params.value("textDocument", json_t()) : json_t();
        const json_t uri = document.is_object() ? document.value("uri", json_t()) : json_t();
        const std::optional<std::filesystem::path> path =
            uri.is_string() ? path_of(uri.get<std::string>()) : std::nullopt;
        if (std::optional<std::pair<std::string, std::int64_t>> open =
                path ? m_workspace->document(*path) : std::nullopt) {
            job.key = file_key(*path);
            job.version = open->second;
            job.text = std::move(open->first);
        }

        worker->push(std::move(job));
        return;
    }

    answer_request(id, method, params);
}

thread_local const server_t::request_job_t* server_t::m_answering = nullptr;

void server_t::answer_job(const request_job_t& job) {
    m_answering = &job;
    answer_request(job.id, job.method, job.params);
    m_answering = nullptr;
}

void server_t::check_deadline() {
    if (m_answering != nullptr && std::chrono::steady_clock::now() >= m_answering->deadline) {
        throw past_deadline_t();
    }
}

std::optional<std::string> server_t::asked_text(const std::filesystem::path& path) const {
    if (m_answering != nullptr && m_answering->version && m_answering->key == file_key(path)) {
        return m_answering->text;
    }

    return m_workspace->text(path);
}

std::optional<checked_t> server_t::asked_check(const std::filesystem::path& path) {
    if (m_answering == nullptr) {
        return m_workspace->checked(path);
    }

    // The quick worker asks for what's done, which starts a check that's waiting for its delay.
    const auto until =
        m_answering->quick ? std::chrono::steady_clock::now() : m_answering->deadline;
    if (!m_answering->version || m_answering->key != file_key(path)) {
        std::optional<checked_t> checked = m_workspace->checked(path, until);
        if (!checked) {
            if (m_answering->quick) {
                throw needs_check_t();
            }

            check_deadline();
        }

        return checked;
    }

    std::optional<checked_t> checked = m_workspace->checked(path, *m_answering->version, until);
    if (!checked) {
        const std::optional<std::pair<std::string, std::int64_t>> document =
            m_workspace->document(path);
        if (document && document->second != *m_answering->version) {
            throw content_modified_t("the document changed since it was asked about");
        }

        if (m_answering->quick) {
            throw needs_check_t();
        }

        check_deadline();
    }

    return checked;
}

void server_t::answer_request(const json_t& id, const std::string& method, const json_t& params) {
    try {
        if (m_inbox != nullptr && m_inbox->take_cancel(id)) {
            fail(id, error_code_t::REQUEST_CANCELLED, "the client cancelled it");
            return;
        }

        // Those behind a stuck check, which their time ran out waiting for, go at once.
        check_deadline();

        if (std::optional<json_t> result = answer(method, params)) {
            // A worker's request the client cancelled while it was answered.
            if (m_answering != nullptr && m_answering->id == id && m_answering->cancelled->load()) {
                fail(id, error_code_t::REQUEST_CANCELLED, "the client cancelled it");
            } else {
                respond(id, std::move(*result));
            }
        } else {
            fail(id, error_code_t::METHOD_NOT_FOUND, "the server doesn't do " + method);
        }
    } catch (const content_modified_t& error) {
        fail(id, error_code_t::CONTENT_MODIFIED, error.what());
    } catch (const needs_check_t&) {
        request_job_t job = *m_answering;
        job.quick = false;
        m_slow.push(std::move(job));
    } catch (const past_deadline_t& error) {
        fail(id, error_code_t::REQUEST_FAILED, error.what());
    } catch (const json_t::exception& error) {
        fail(id, error_code_t::INVALID_PARAMS, error.what());
    } catch (const std::invalid_argument& error) {
        fail(id, error_code_t::INVALID_PARAMS, error.what());
    } catch (const std::runtime_error& error) {
        fail(id, error_code_t::REQUEST_FAILED, error.what());
    } catch (const std::exception& error) {
        fail(id, error_code_t::INTERNAL_ERROR, error.what());
    }
}

std::optional<json_t> server_t::answer(const std::string& method, const json_t& params) {
    if (std::optional<json_t> manifest =
            m_initialized ? m_toml->answer(method, params) : std::nullopt) {
        return manifest;
    }

    if (method == "initialize") {
        return initialize(params);
    }

    // The requests before it are answered first, those the workers have too.
    if (method == "shutdown") {
        m_quick.wait_idle();
        m_slow.wait_idle();
        m_shut_down = true;
        return nullptr;
    }

    if (method == "textDocument/hover") {
        return hover(params);
    }

    if (method == "textDocument/definition") {
        return definition(params);
    }

    if (method == "textDocument/references") {
        return references(
            params, params.value("context", json_t::object()).value("includeDeclaration", true));
    }

    if (method == "textDocument/prepareRename") {
        return prepare_rename(params);
    }

    if (method == "textDocument/rename") {
        return rename(params);
    }

    if (method == "textDocument/completion") {
        return completion(params);
    }

    if (method == "textDocument/signatureHelp") {
        return signature_help(params);
    }

    if (method == "textDocument/documentSymbol") {
        return document_symbols(params);
    }

    if (method == "workspace/symbol") {
        return workspace_symbols(params);
    }

    if (method == "textDocument/semanticTokens/full") {
        return semantic_tokens(params);
    }

    if (method == "textDocument/codeAction") {
        return code_actions(params);
    }

    if (method == "fb/changeSignature") {
        return change_signature(params);
    }

    if (method == "textDocument/formatting") {
        return formatting(params);
    }

    // A save formats the document when the client asked for it.
    if (method == "textDocument/willSaveWaitUntil") {
        return m_format_on_save ? formatting(params) : json_t(json_t::array());
    }

    if (method == "textDocument/foldingRange") {
        return folding_ranges(params);
    }

    if (method == "textDocument/selectionRange") {
        return selection_ranges(params);
    }

    if (method == "textDocument/documentHighlight") {
        return document_highlights(params);
    }

    if (method == "textDocument/inlayHint") {
        return inlay_hints(params);
    }

    if (method == "textDocument/typeDefinition") {
        return type_definition(params);
    }

    if (method == "textDocument/implementation") {
        return implementations(params);
    }

    if (method == "textDocument/prepareCallHierarchy" || method.starts_with("callHierarchy/")) {
        return call_hierarchy(method, params);
    }

    if (method == "textDocument/prepareTypeHierarchy" || method.starts_with("typeHierarchy/")) {
        return type_hierarchy(method, params);
    }

    if (method == "textDocument/codeLens") {
        return code_lenses(params);
    }

    if (method == "codeLens/resolve") {
        return resolve_code_lens(params);
    }

    // What the extension asks to see the server's still answering, which waits for nothing.
    if (method == "fb/ping") {
        return nullptr;
    }

    if (method == "fb/tests") {
        return tests();
    }

    return std::nullopt;
}

server_t::worker_t* server_t::worker_for(const std::string& method, const json_t& params) {
    // What the last check answers as code is typed, and what waits for the check of the text.
    static const std::set<std::string, std::less<>> quick{
        "textDocument/hover",          "textDocument/definition",        "textDocument/completion",
        "textDocument/signatureHelp",  "textDocument/documentHighlight", "textDocument/inlayHint",
        "textDocument/typeDefinition",
    };
    static const std::set<std::string, std::less<>> slow{
        "textDocument/references",
        "textDocument/prepareRename",
        "textDocument/rename",
        "textDocument/codeAction",
        "textDocument/implementation",
        "textDocument/prepareCallHierarchy",
        "callHierarchy/incomingCalls",
        "callHierarchy/outgoingCalls",
        "textDocument/prepareTypeHierarchy",
        "typeHierarchy/supertypes",
        "typeHierarchy/subtypes",
        "codeLens/resolve",
        "fb/changeSignature",
    };
    const json_t document = params.is_object() ? params.value("textDocument", json_t()) : json_t();
    const json_t uri = document.is_object() ? document.value("uri", json_t()) : json_t();
    if (uri.is_string() && uri.get<std::string>().ends_with("project.toml")) {
        return nullptr;
    }

    if (quick.contains(method)) {
        return &m_quick;
    }

    return slow.contains(method) ? &m_slow : nullptr;
}

void server_t::worker_t::push(request_job_t job) {
    {
        const std::scoped_lock lock(m_mutex);
        m_jobs.push_back(std::move(job));
        if (!m_thread.joinable()) {
            m_thread = std::thread([this] { run(); });
        }
    }

    m_ready.notify_one();
}

bool server_t::worker_t::cancel(const json_t& id) {
    const std::scoped_lock lock(m_mutex);
    if (m_current != nullptr && m_current->id == id) {
        m_current->cancelled->store(true);
        return false;
    }

    return std::erase_if(m_jobs, [&](const request_job_t& job) { return job.id == id; }) > 0;
}

void server_t::worker_t::wait_idle() {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] { return m_jobs.empty() && !m_busy; });
}

void server_t::worker_t::stop() {
    {
        const std::scoped_lock lock(m_mutex);
        m_stop = true;
    }

    m_ready.notify_one();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void server_t::worker_t::run() {
    while (true) {
        request_job_t job;
        {
            std::unique_lock lock(m_mutex);
            m_ready.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop) {
                return;
            }

            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            m_busy = true;
            m_current = &job;
        }

        m_answer(job);
        {
            const std::scoped_lock lock(m_mutex);
            m_busy = false;
            m_current = nullptr;
        }

        m_idle.notify_all();
    }
}

std::vector<std::shared_ptr<const compilation_t>>
server_t::dependents(const std::filesystem::path& file) {
    const std::optional<std::pair<std::string, std::int64_t>> document =
        m_workspace->document(file);
    if (!document) {
        return m_workspace->with_dependents(file);
    }

    if (m_answering != nullptr && m_answering->version && m_answering->key == file_key(file) &&
        *m_answering->version != document->second) {
        throw content_modified_t("the document changed since it was asked about");
    }

    // The first to ask for a version makes its checks, which the rest wait for.
    std::promise<std::vector<std::shared_ptr<const compilation_t>>> made;
    std::shared_future<std::vector<std::shared_ptr<const compilation_t>>> known;
    std::filesystem::file_time_type checked;
    const std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now();
    const auto make = [&] {
        return dependents_t{
            .version = document->second,
            .compilations = made.get_future().share(),
            .checked = now,
        };
    };
    {
        const std::scoped_lock lock(m_dependents_mutex);
        dependents_t& found = m_dependents[file_key(file)];
        if (found.compilations.valid() && found.version == document->second) {
            known = found.compilations;
            checked = found.checked;
        } else {
            found = make();
        }
    }

    if (known.valid()) {
        if (m_answering != nullptr &&
            known.wait_until(m_answering->deadline) != std::future_status::ready) {
            throw past_deadline_t();
        }

        // Without the client watching, a file that isn't open, but std's, may have changed on
        // disk since.
        std::vector<std::shared_ptr<const compilation_t>> compilations = known.get();
        const bool written = !m_workspace->watched() &&
                             std::ranges::any_of(compilations, [&](const auto& compilation) {
                                 return written_since(*compilation, checked);
                             });
        if (!written) {
            return compilations;
        }

        const std::scoped_lock lock(m_dependents_mutex);
        m_dependents[file_key(file)] = make();
    }

    try {
        std::vector<std::shared_ptr<const compilation_t>> compilations =
            m_workspace->with_dependents(file);
        made.set_value(compilations);
        return compilations;
    } catch (...) {
        made.set_exception(std::current_exception());
        throw;
    }
}

bool server_t::written_since(const compilation_t& compilation,
                             const std::filesystem::file_time_type since) const {
    for (const module_t& module : compilation.ast.modules().modules()) {
        const std::filesystem::path path = compilation.disk_path(module.file);
        if (path.empty() || module.path.starts_with("std/") || m_workspace->document(path)) {
            continue;
        }

        std::error_code error;
        const std::filesystem::file_time_type written =
            std::filesystem::last_write_time(path, error);
        if (error || written > since) {
            return true;
        }
    }

    return false;
}

void server_t::forget_dependents() {
    const std::scoped_lock lock(m_dependents_mutex);
    m_dependents.clear();
}

void server_t::notified(const std::string& method, const json_t& params) {
    // A request a worker hasn't started on is cancelled there.
    if (method == "$/cancelRequest") {
        const json_t id = params.is_object() ? params.value("id", json_t()) : json_t();
        if (m_quick.cancel(id) || m_slow.cancel(id)) {
            fail(id, error_code_t::REQUEST_CANCELLED, "the client cancelled it");
        }

        return;
    }

    // What a change of any file can change the references of. A save changes no document's text,
    // nor does a change on disk of an open document's file, which the checks read from the
    // editor.
    if (method == "textDocument/didOpen" || method == "textDocument/didChange" ||
        method == "textDocument/didClose" || method == "workspace/didChangeWorkspaceFolders") {
        forget_dependents();
    } else if (method == "workspace/didChangeWatchedFiles") {
        const json_t changes = params.value("changes", json_t::array());
        const bool documents =
            changes.is_array() && std::ranges::all_of(changes, [&](const json_t& change) {
                const json_t uri = change.is_object() ? change.value("uri", json_t()) : json_t();
                const std::optional<std::filesystem::path> path =
                    uri.is_string() ? path_of(uri.get<std::string>()) : std::nullopt;
                return path && m_workspace->document(*path).has_value();
            });
        if (!documents) {
            forget_dependents();
        }
    }

    if (method == "exit") {
        m_exit = m_shut_down ? 0 : 1;
        return;
    }

    if (!m_initialized) {
        return;
    }

    if (method == "initialized") {
        // The client watches the files, for changes made outside it, like a git checkout.
        if (m_watches) {
            register_watchers({});
        }

        m_workspace->add_folders(m_folders);
        m_toml->add_folders(m_folders);
        return;
    }

    if (method == "textDocument/didOpen") {
        // FutureBlitz code and project.toml are checked as they're typed; another file, a .md,
        // say, is read from disk.
        const json_t& document = params.at("textDocument");
        const std::optional<std::filesystem::path> path =
            path_of(document.at("uri").get<std::string>());
        if (path && is_fb_file(*path)) {
            m_workspace->open(*path, document.value("version", std::int64_t{ 0 }),
                              document.at("text").get<std::string>());
        } else if (path && is_manifest(*path)) {
            m_toml->open(*path, document.value("version", std::int64_t{ 0 }),
                         document.at("text").get<std::string>());
        }

        return;
    }

    if (method == "textDocument/didChange") {
        const std::optional<std::filesystem::path> path = document_path(params);
        if (!path) {
            return;
        }

        std::optional<std::pair<std::string, std::int64_t>> document = m_workspace->document(*path);
        const bool manifest = !document && is_manifest(*path);
        if (manifest) {
            document = m_toml->document(*path);
        }

        if (!document) {
            return;
        }

        for (const json_t& change : params.at("contentChanges")) {
            apply_change(document->first, change, m_positions);
        }

        const std::int64_t version =
            params.at("textDocument").value("version", document->second + 1);
        if (manifest) {
            m_toml->change(*path, version, std::move(document->first));
        } else {
            // Before the change, so its check's publish finds it, but the version's, so an earlier
            // check's doesn't.
            {
                const std::scoped_lock lock(m_tokens_mutex);
                m_changed[file_key(*path)] = version;
            }

            m_workspace->change(*path, version, std::move(document->first));
        }

        return;
    }

    if (method == "textDocument/didClose") {
        // As didOpen has them: a closed file of another kind isn't checked as FutureBlitz code.
        if (const std::optional<std::filesystem::path> path = document_path(params);
            path && is_fb_file(*path)) {
            m_workspace->close(*path);
        } else if (path && is_manifest(*path)) {
            m_toml->close(*path);
        }

        return;
    }

    if (method == "workspace/didChangeWatchedFiles") {
        std::vector<std::filesystem::path> paths;
        for (const json_t& change : params.at("changes")) {
            if (std::optional<std::filesystem::path> path =
                    path_of(change.at("uri").get<std::string>())) {
                paths.push_back(std::move(*path));
            }
        }

        m_workspace->changed_on_disk(paths);
        return;
    }

    // The client's settings, of which inlay hints' are the server's.
    if (method == "workspace/didChangeConfiguration") {
        set_inlay(
            object_at(object_at(params, "settings"), "futureblitz").value("inlayHints", json_t()));
        if (m_inlay_refresh) {
            std::string id;
            {
                const std::scoped_lock lock(m_watch_mutex);
                id = std::format("fb-{}", m_next_request++);
            }

            send({
                { "jsonrpc", "2.0" },
                { "id", id },
                { "method", "workspace/inlayHint/refresh" },
            });
        }

        return;
    }

    if (method == "workspace/didChangeWorkspaceFolders") {
        const auto folders = [&](const char* which) {
            std::vector<std::filesystem::path> found;
            for (const json_t& folder : params.at("event").value(which, json_t::array())) {
                if (std::optional<std::filesystem::path> path =
                        path_of(folder.at("uri").get<std::string>())) {
                    found.push_back(std::move(*path));
                }
            }

            return found;
        };
        m_workspace->remove_folders(folders("removed"));
        m_workspace->add_folders(folders("added"));
        m_toml->remove_folders(folders("removed"));
        m_toml->add_folders(folders("added"));
    }
}

json_t server_t::initialize(const json_t& params) {
    const json_t capabilities = object_at(params, "capabilities");
    // UTF-8 is what the compiler counts in, so it's the cheapest when the client can.
    const json_t encodings =
        object_at(capabilities, "general").value("positionEncodings", json_t());
    std::string encoding = "utf-16";
    const bool utf8 = encodings.is_array() && std::ranges::contains(encodings, json_t("utf-8"));
    if (utf8) {
        encoding = "utf-8";
    }

    const json_t watching =
        object_at(object_at(capabilities, "workspace"), "didChangeWatchedFiles");
    const bool watches = watching.value("dynamicRegistration", false);
    const bool relative = watching.value("relativePatternSupport", false);
    const json_t options = object_at(params, "initializationOptions");
    const bool format_on_save = options.value("formatOnSave", false);
    set_inlay(options.value("inlayHints", json_t()));
    m_tests = options.value("tests", false);
    m_inlay_refresh =
        object_at(object_at(capabilities, "workspace"), "inlayHint").value("refreshSupport", false);
    m_tokens_refresh = object_at(object_at(capabilities, "workspace"), "semanticTokens")
                           .value("refreshSupport", false);
    m_lenses_refresh =
        object_at(object_at(capabilities, "workspace"), "codeLens").value("refreshSupport", false);

    // The folders, or the one the client opened before it had several.
    std::vector<std::filesystem::path> folders;
    if (params.contains("workspaceFolders") && params["workspaceFolders"].is_array()) {
        for (const json_t& folder : params["workspaceFolders"]) {
            if (std::optional<std::filesystem::path> path =
                    path_of(folder.at("uri").get<std::string>())) {
                folders.push_back(std::move(*path));
            }
        }
    } else if (params.contains("rootUri") && params["rootUri"].is_string()) {
        if (std::optional<std::filesystem::path> path =
                path_of(params["rootUri"].get<std::string>())) {
            folders.push_back(std::move(*path));
        }
    }

    // It's initialized once its params are read without a mistake, and before any document is
    // open, so the threads that publish read the positions' unit only after it's set.
    m_positions.unit = utf8 ? column_unit_t::UTF8 : column_unit_t::UTF16;
    m_watches = watches;
    m_relative = relative;
    m_format_on_save = format_on_save;
    // A project.toml's quick fix has placeholders where the client takes snippet edits.
    m_toml->set_snippet_edits(object_at(object_at(capabilities, "workspace"), "workspaceEdit")
                                  .value("snippetEditSupport", false));
    m_folders = std::move(folders);
    m_initialized = true;
    return {
        {
            "capabilities",
            {
                { "positionEncoding", encoding },
                {
                    "textDocumentSync",
                    {
                        { "openClose", true },
                        { "change", 2 },
                        { "willSaveWaitUntil", m_format_on_save },
                        { "save", { { "includeText", false } } },
                    },
                },
                { "hoverProvider", true },
                { "definitionProvider", true },
                { "referencesProvider", true },
                { "renameProvider", { { "prepareProvider", true } } },
                { "completionProvider", { { "triggerCharacters", json_t::array({ "." }) } } },
                {
                    "signatureHelpProvider",
                    { { "triggerCharacters", json_t::array({ "(", "," }) } },
                },
                { "documentSymbolProvider", true },
                { "workspaceSymbolProvider", true },
                {
                    "semanticTokensProvider",
                    { { "legend", semantic_tokens_legend() }, { "full", true } },
                },
                {
                    "codeActionProvider",
                    {
                        {
                            "codeActionKinds",
                            json_t::array({
                                "quickfix",
                                "refactor.extract",
                                "refactor.inline",
                                "source.organizeImports",
                            }),
                        },
                    },
                },
                { "documentFormattingProvider", true },
                { "foldingRangeProvider", true },
                { "selectionRangeProvider", true },
                { "documentHighlightProvider", true },
                { "inlayHintProvider", true },
                { "typeDefinitionProvider", true },
                { "implementationProvider", true },
                { "callHierarchyProvider", true },
                { "typeHierarchyProvider", true },
                { "codeLensProvider", { { "resolveProvider", true } } },
                {
                    "workspace",
                    {
                        {
                            "workspaceFolders",
                            { { "supported", true }, { "changeNotifications", true } },
                        },
                    },
                },
            },
        },
        {
            "serverInfo",
            { { "name", "fb-lsp" }, { "version", std::string(build_config::version) } },
        },
    };
}

std::optional<std::uint32_t> server_t::last_check_t::to_then(const std::uint32_t offset) const {
    const auto now_size = static_cast<std::uint32_t>(now.text().size());
    const auto then_size = static_cast<std::uint32_t>(then().text().size());
    if (offset <= prefix) {
        return offset;
    }

    if (offset >= now_size - suffix) {
        return then_size - (now_size - offset);
    }

    return std::nullopt;
}

std::optional<std::uint32_t>
server_t::last_check_t::line_to_then(const std::uint32_t offset) const {
    const std::string_view text = now.text();
    const std::size_t newline = offset == 0 ? std::string_view::npos : text.rfind('\n', offset - 1);
    const auto begin =
        static_cast<std::uint32_t>(newline == std::string_view::npos ? 0 : newline + 1);
    const auto end = static_cast<std::uint32_t>(std::min(text.find('\n', offset), text.size()));
    const auto size = static_cast<std::uint32_t>(text.size());
    if (end < prefix || begin > size - suffix || (begin == size - suffix && begin == prefix)) {
        return to_then(offset);
    }

    return std::nullopt;
}

std::optional<std::uint32_t> server_t::last_check_t::to_now(const std::uint32_t offset) const {
    const auto now_size = static_cast<std::uint32_t>(now.text().size());
    const auto then_size = static_cast<std::uint32_t>(then().text().size());
    if (offset <= prefix) {
        return offset;
    }

    if (offset >= then_size - suffix) {
        return now_size - (then_size - offset);
    }

    return std::nullopt;
}

std::optional<server_t::last_check_t> server_t::last_check(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    if (!path) {
        return std::nullopt;
    }

    std::optional<checked_t> checked = m_workspace->checked_last(*path);
    if (!checked) {
        checked = asked_check(*path);
    }

    const std::optional<std::string> text = asked_text(*path);
    if (!checked || !text) {
        return std::nullopt;
    }

    last_check_t last{
        .checked = *checked,
        .now = source_file_t(checked->file,
                             std::string(checked->compilation->sources.get(checked->file).path()),
                             *text),
    };
    // What's the same at the start and at the end, without overlapping.
    const std::string_view then = last.then().text();
    const std::string_view now = last.now.text();
    const std::size_t most = std::min(then.size(), now.size());
    while (last.prefix < most && then[last.prefix] == now[last.prefix]) {
        ++last.prefix;
    }

    while (last.suffix < most - last.prefix &&
           then[then.size() - 1 - last.suffix] == now[now.size() - 1 - last.suffix]) {
        ++last.suffix;
    }

    return last;
}

bool server_t::moved(const last_check_t& last, json_t& range) const {
    const std::optional<std::uint32_t> begin =
        last.to_now(m_positions.offset(last.then(), range.at("start")));
    const std::optional<std::uint32_t> end =
        last.to_now(m_positions.offset(last.then(), range.at("end")));
    if (!begin || !end) {
        return false;
    }

    range = m_positions.range(last.now, { .file = last.now.id(), .begin = *begin, .end = *end });
    return true;
}

json_t server_t::at_last_check(const json_t& params,
                               const std::function<json_t(const query_t&, std::uint32_t)>& answer) {
    const std::optional<last_check_t> last = last_check(params);
    if (!last) {
        return nullptr;
    }

    const std::optional<std::uint32_t> at =
        last->line_to_then(m_positions.offset(last->now, params.at("position")));
    if (!at) {
        // Its line changed since: the check of the document's text, waited for.
        const std::optional<std::filesystem::path> path = document_path(params);
        const std::optional<checked_t> current = path ? asked_check(*path) : std::nullopt;
        if (!current) {
            return nullptr;
        }

        const query_t query(*current->compilation, current->file);
        return answer(query, m_positions.offset(query.source(), params.at("position")));
    }

    json_t result = answer(query_t(*last->checked.compilation, last->checked.file), *at);
    // Its ranges in the document, moved to its text now; one in what's changed goes. An item is
    // in it by its file, as a client's URI of it can be spelled otherwise, as VS Code's
    // "file:///c%3A/" on Windows is.
    const std::optional<std::filesystem::path> here = document_path(params);
    const auto in_document = [&](const json_t& item) {
        if (!item.contains("uri")) {
            return true;
        }

        const std::optional<std::filesystem::path> there =
            item["uri"].is_string() ? path_of(item["uri"].get<std::string>()) : std::nullopt;
        return here && there && file_key(*here) == file_key(*there);
    };
    const auto move = [&](json_t& item) {
        if (item.is_object() && item.contains("range") && in_document(item)) {
            return moved(*last, item["range"]);
        }

        return true;
    };
    if (result.is_array()) {
        json_t kept = json_t::array();
        for (json_t& item : result) {
            if (move(item)) {
                kept.push_back(std::move(item));
            }
        }

        return kept;
    }

    if (result.is_object() && !move(result)) {
        // A hover's range alone is what can't move.
        if (result.contains("contents")) {
            result.erase("range");
        } else {
            return nullptr;
        }
    }

    return result;
}

json_t server_t::hover(const json_t& params) {
    return at_last_check(params, [&](const query_t& query, const std::uint32_t offset) {
        return lsp::hover(query, offset, m_positions);
    });
}

json_t server_t::definition(const json_t& params) {
    return at_last_check(params, [&](const query_t& query, const std::uint32_t offset) {
        return lsp::definition(query, offset, m_positions);
    });
}

std::optional<server_t::target_t> server_t::target(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? asked_check(*path) : std::nullopt;
    if (!checked) {
        return std::nullopt;
    }

    const query_t query(*checked->compilation, checked->file);
    const std::optional<named_t> named =
        query.name_at(m_positions.offset(query.source(), params.at("position")));
    if (!named) {
        return std::nullopt;
    }

    // The checks of the file it's declared in and every file that imports it, or for a builtin,
    // this one's.
    std::vector<std::shared_ptr<const compilation_t>> compilations{ checked->compilation };
    const std::optional<std::filesystem::path> declared = query.declared_in(named->symbol);
    if (declared) {
        compilations = dependents(*declared);
    }

    return target_t{
        .checked = *checked,
        .named = *named,
        .key = query.key(named->symbol),
        .declared = declared,
        .compilations = std::move(compilations),
    };
}

json_t server_t::references(const json_t& params, const bool declaration) {
    const std::optional<target_t> found = target(params);
    if (!found) {
        return json_t::array();
    }

    const auto kept = [this](const std::filesystem::path& file) {
        return m_workspace->owned(file) || m_workspace->document(file).has_value();
    };
    return lsp::references(found->compilations, std::span(&found->key, 1), declaration, kept,
                           m_positions);
}

bool server_t::editable(const std::filesystem::path& file) const {
    // std is the workspace's own only when it's in one of its folders.
    const std::string standard = file_key(toolchain().std) + "/";
    return m_workspace->owned(file) ||
           (m_workspace->document(file).has_value() && !file_key(file).starts_with(standard));
}

std::optional<std::string> server_t::unrenamable(const target_t& found) const {
    const query_t query(*found.checked.compilation, found.checked.file);
    const symbol_t& symbol = query.analysis().symbol(found.named.symbol);
    if (symbol.kind == symbol_kind_t::MODULE) {
        return "a module's name, or an Import's As name, isn't renamed here; change its Import";
    }

    // A New is always named New.
    if (symbol.kind == symbol_kind_t::CONSTRUCTOR ||
        query.source().slice(found.named.span) == "New") {
        return "a New is always called New";
    }

    if (!found.declared || !editable(*found.declared)) {
        return "only a name the workspace declares can be renamed";
    }

    return std::nullopt;
}

json_t server_t::prepare_rename(const json_t& params) {
    const std::optional<target_t> found = target(params);
    if (!found || unrenamable(*found)) {
        return nullptr;
    }

    const source_file_t& file = found->checked.compilation->sources.get(found->checked.file);
    return {
        { "range", m_positions.range(file, found->named.span) },
        { "placeholder", std::string(file.slice(found->named.span)) },
    };
}

json_t server_t::rename(const json_t& params) {
    const std::string name = params.at("newName").get<std::string>();
    if (!is_identifier(name)) {
        throw std::invalid_argument(
            std::format("'{}' isn't a name: a name is ASCII letters, digits, and '_', and isn't a "
                        "keyword",
                        name));
    }

    std::optional<target_t> found = target(params);
    if (!found) {
        throw std::runtime_error("there's no name here to rename");
    }

    if (const std::optional<std::string> why = unrenamable(*found)) {
        throw std::runtime_error(*why);
    }

    // What overrides it or implements it changes with it, from the checks of each of their
    // files and what imports those, until there are no more.
    std::vector<std::shared_ptr<const compilation_t>>& compilations = found->compilations;
    std::set<std::string> checked_files;
    if (found->declared) {
        checked_files.insert(file_key(*found->declared));
    }

    rename_set_t set = rename_set(compilations, found->key);
    for (bool more = true; more;) {
        more = false;
        for (const std::filesystem::path& file : set.files) {
            if (checked_files.insert(file_key(file)).second) {
                std::ranges::move(dependents(file), std::back_inserter(compilations));
                more = true;
            }
        }

        if (more) {
            set = rename_set(compilations, found->key);
        }
    }

    for (const symbol_key_t& key : set.keys) {
        if (!key.file.empty() && !editable(path_from_utf8(key.file))) {
            throw std::runtime_error(
                std::format("'{}' is tied to a declaration in {}, which a rename can't change",
                            key.name, key.file));
        }
    }

    if (const std::optional<std::string> conflict = rename_conflict(compilations, set.keys, name)) {
        throw std::runtime_error(*conflict);
    }

    return rename_edit(lsp::references(
                           compilations, set.keys, true,
                           [this](const std::filesystem::path& file) { return editable(file); },
                           m_positions),
                       name);
}

json_t server_t::completion(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    if (!path) {
        return nullptr;
    }

    // From the check of the document's text, if it's done; else from the last one, where the
    // text since it is only what's typed at the cursor; else the check's, waited for.
    const std::optional<last_check_t> last = last_check(params);
    if (!last) {
        return nullptr;
    }

    const query_t query(*last->checked.compilation, last->checked.file);
    const std::uint32_t cursor = m_positions.offset(last->now, params.at("position"));
    const auto modules = [&] { return import_names(*path, *last->checked.compilation); };
    const auto importable = [&] { return m_workspace->importable(*path); };
    const auto now_size = static_cast<std::uint32_t>(last->now.text().size());
    const auto then_size = static_cast<std::uint32_t>(last->then().text().size());
    const std::string_view typed =
        last->now.text().substr(last->prefix, now_size - last->suffix - last->prefix);
    const bool inserted =
        then_size - last->suffix == last->prefix && cursor == last->prefix + typed.size();
    const bool word = !typed.empty() && std::ranges::all_of(typed, [](const char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    });
    if (const std::optional<std::uint32_t> at = last->line_to_then(cursor)) {
        json_t found = lsp::completion(query, *at, modules, importable, m_positions);
        return moved_items(*last, std::move(found), std::nullopt, cursor);
    }

    if (inserted && word) {
        json_t found = lsp::completion(query, last->prefix, modules, importable, m_positions);
        return moved_items(*last, std::move(found), std::nullopt, cursor);
    }

    if (inserted && (typed == "." || typed == "?.")) {
        json_t found = lsp::member_completion(query, last->prefix, m_positions);
        return moved_items(*last, std::move(found), last->prefix, cursor);
    }

    const std::optional<checked_t> checked = asked_check(*path);
    if (!checked) {
        return nullptr;
    }

    const query_t current(*checked->compilation, checked->file);
    return lsp::completion(
        current, m_positions.offset(current.source(), params.at("position")),
        [&] { return import_names(*path, *checked->compilation); }, importable, m_positions);
}

json_t server_t::moved_items(const last_check_t& last, json_t found,
                             const std::optional<std::uint32_t> typed_at,
                             const std::uint32_t cursor) const {
    // Each item's edit ends at the cursor, and one that started where the '.' was typed starts
    // there too; what else it edits is where it is in the document now.
    const json_t at = m_positions.position(last.now, cursor);
    const json_t typed_position =
        typed_at ? m_positions.position(last.then(), *typed_at) : json_t();
    json_t& items = found.is_object() ? found["items"] : found;
    if (!items.is_array()) {
        return found;
    }

    json_t kept = json_t::array();
    for (json_t& item : items) {
        bool fits = true;
        if (item.contains("textEdit")) {
            json_t& range = item["textEdit"]["range"];
            if (typed_at && range["start"] == typed_position) {
                range["start"] = at;
            } else {
                json_t start{ { "start", range["start"] }, { "end", range["start"] } };
                fits = moved(last, start);
                range["start"] = start["start"];
            }

            range["end"] = at;
        }

        if (item.contains("additionalTextEdits")) {
            for (json_t& edit : item["additionalTextEdits"]) {
                fits = fits && moved(last, edit["range"]);
            }
        }

        if (fits) {
            kept.push_back(std::move(item));
        }
    }

    items = std::move(kept);
    if (found.is_object() && last.prefix != last.now.text().size()) {
        found["isIncomplete"] = true;
    }

    return found;
}

json_t server_t::signature_help(const json_t& params) {
    // From the check of the document's text, if it's done; else from the last one, when what's
    // typed since is a call's '(' or ',' at the cursor; else the check's, waited for.
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<last_check_t> last = path ? last_check(params) : std::nullopt;
    if (!last) {
        return nullptr;
    }

    const query_t query(*last->checked.compilation, last->checked.file);
    const std::uint32_t cursor = m_positions.offset(last->now, params.at("position"));
    const auto now_size = static_cast<std::uint32_t>(last->now.text().size());
    const auto then_size = static_cast<std::uint32_t>(last->then().text().size());
    const std::string_view typed =
        last->now.text().substr(last->prefix, now_size - last->suffix - last->prefix);
    const bool inserted =
        then_size - last->suffix == last->prefix && cursor == last->prefix + typed.size();
    if (const std::optional<std::uint32_t> at = last->line_to_then(cursor)) {
        return lsp::signature_help(query, *at, m_positions);
    }

    if (inserted && typed == "(") {
        return lsp::signature_help_before(query, last->prefix, 0, m_positions);
    }

    if (inserted && !typed.empty() && typed.find_first_not_of(", ") == std::string_view::npos) {
        json_t found = lsp::signature_help(query, last->prefix, m_positions);
        if (found.is_object()) {
            found["activeParameter"] = found["activeParameter"].get<std::uint32_t>() +
                                       static_cast<std::uint32_t>(std::ranges::count(typed, ','));
        }

        return found;
    }

    const std::optional<checked_t> checked = asked_check(*path);
    if (!checked) {
        return nullptr;
    }

    const query_t current(*checked->compilation, checked->file);
    return lsp::signature_help(current, m_positions.offset(current.source(), params.at("position")),
                               m_positions);
}

json_t server_t::document_symbols(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    if (!path) {
        return nullptr;
    }

    const std::optional<std::string> text = asked_text(*path);
    if (!text) {
        return nullptr;
    }

    return lsp::document_symbols(source_file_t(file_id_t{}, to_utf8(*path), *text), m_positions);
}

json_t server_t::workspace_symbols(const json_t& params) {
    const std::string query = params.value("query", std::string());
    constexpr std::size_t most = 1000;
    json_t out = json_t::array();
    for (const unit_t& unit : m_workspace->units()) {
        if (!m_workspace->owns(unit.root)) {
            continue;
        }

        for (const std::filesystem::path& file : unit.files) {
            std::optional<std::string> text = asked_text(file);
            if (!text) {
                continue;
            }

            // Each file's declarations are found once for each text it has.
            auto& [known, symbols] = m_symbols[file_key(file)];
            if (known != *text || symbols.is_null()) {
                symbols = lsp::workspace_symbols(source_file_t(file_id_t{}, to_utf8(file), *text),
                                                 uri_of(file), {}, m_positions);
                known = std::move(*text);
            }

            for (const json_t& symbol : symbols) {
                if (fuzzy_match(symbol["name"].get<std::string>(), query)) {
                    out.push_back(symbol);
                    if (out.size() >= most) {
                        return out;
                    }
                }
            }
        }
    }

    return out;
}

json_t server_t::semantic_tokens(const json_t& params) {
    // The check of the document's text, if it's done; else what the lexer knows now, and the
    // check's tokens once it's done, which the client is told to ask for.
    const std::optional<std::filesystem::path> path = document_path(params);
    if (!path) {
        return nullptr;
    }

    // Marked lexical before the check's looked for, so one published between is told, and not
    // once it's found.
    const std::string key = file_key(*path);
    {
        const std::scoped_lock lock(m_tokens_mutex);
        m_lexical_tokens.insert(key);
    }

    if (const std::optional<checked_t> checked = m_workspace->checked_now(*path)) {
        {
            const std::scoped_lock lock(m_tokens_mutex);
            m_lexical_tokens.erase(key);
        }

        return lsp::semantic_tokens(query_t(*checked->compilation, checked->file), m_positions);
    }

    const std::optional<std::string> text = asked_text(*path);
    if (!text) {
        return nullptr;
    }

    return lsp::lexical_tokens(source_file_t(file_id_t{}, to_utf8(*path), *text), m_positions);
}

json_t server_t::code_actions(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? asked_check(*path) : std::nullopt;
    if (!checked) {
        return nullptr;
    }

    const query_t query(*checked->compilation, checked->file);
    const json_t& range = params.at("range");
    const span_t span{
        .file = checked->file,
        .begin = m_positions.offset(query.source(), range.at("start")),
        .end = m_positions.offset(query.source(), range.at("end")),
    };
    json_t actions =
        lsp::code_actions(query, span, m_positions,
                          [this](const std::filesystem::path& file) { return editable(file); });

    // A kind the client asks for, or without `only`, all but source actions, which a menu of
    // their own asks for.
    const json_t only = object_at(params, "context").value("only", json_t());
    const auto wanted = [&](const std::string& kind) {
        if (!only.is_array()) {
            return !kind.starts_with("source");
        }

        return std::ranges::any_of(only, [&](const json_t& asked) {
            return asked.is_string() && (kind == asked.get<std::string>() ||
                                         kind.starts_with(asked.get<std::string>() + "."));
        });
    };

    // Refactorings and organizing the Imports are of the document's text as its check had it.
    const std::optional<std::string> current = asked_text(*path);
    if (!current || *current != query.source().text()) {
        return actions;
    }

    const auto action = [&](const std::string& title, const std::string& kind,
                            const std::vector<fix_t>& fixes) {
        if (std::optional<json_t> edit = workspace_edit(query.compilation(), fixes)) {
            actions.push_back(
                { { "title", title }, { "kind", kind }, { "edit", std::move(*edit) } });
        }
    };
    for (const refactoring_t& refactoring : refactorings(query, span)) {
        if (wanted(refactoring.kind)) {
            action(refactoring.title, refactoring.kind, refactoring.edits);
        }
    }

    if (wanted("source.organizeImports")) {
        if (const std::vector<fix_t> fixes = organized_imports(query); !fixes.empty()) {
            action("Organize Imports", "source.organizeImports", fixes);
        }
    }

    return actions;
}

std::optional<json_t> server_t::workspace_edit(const compilation_t& compilation,
                                               const std::vector<fix_t>& fixes) const {
    json_t changes = json_t::object();
    std::set<std::tuple<std::string, std::uint32_t, std::uint32_t, std::string>> seen;
    for (const fix_t& fix : fixes) {
        const std::filesystem::path path = compilation.disk_path(fix.span.file);
        if (path.empty() || !editable(path)) {
            return std::nullopt;
        }

        const std::string uri = uri_of(path);
        if (!seen.emplace(uri, fix.span.begin, fix.span.end, fix.replacement).second) {
            continue;
        }

        changes[uri].push_back({
            { "range", m_positions.range(compilation.sources.get(fix.span.file), fix.span) },
            { "newText", fix.replacement },
        });
    }

    return json_t{ { "changes", std::move(changes) } };
}

json_t server_t::change_signature(const json_t& params) {
    const std::optional<target_t> found = target(params);
    if (!found) {
        throw std::runtime_error("there's no Function or Method here");
    }

    const query_t query(*found->checked.compilation, found->checked.file);
    // Without new parameters, the ones it has, for the editor to change.
    if (!params.contains("parameters")) {
        const std::expected<std::string, std::string> written =
            signature_parameters(query, found->named.symbol);
        if (!written) {
            throw std::runtime_error(written.error());
        }

        return { { "parameters", *written } };
    }

    // Its declaration and calls in each check that has them, the same edit once.
    const std::string written = params.at("parameters").get<std::string>();
    json_t changes = json_t::object();
    for (const std::shared_ptr<const compilation_t>& compilation : found->compilations) {
        const query_t each(*compilation, compilation->file);
        const std::expected<std::vector<fix_t>, std::string> fixes =
            lsp::change_signature(each, found->key, written);
        if (!fixes) {
            throw std::runtime_error(fixes.error());
        }

        const std::optional<json_t> edit = workspace_edit(*compilation, *fixes);
        if (!edit) {
            throw std::runtime_error("a call of it isn't the workspace's to change");
        }

        for (const auto& [uri, edits] : edit->at("changes").items()) {
            for (const json_t& one : edits) {
                if (!std::ranges::contains(changes[uri], one)) {
                    changes[uri].push_back(one);
                }
            }
        }
    }

    return { { "changes", std::move(changes) } };
}

json_t server_t::formatting(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    if (!path) {
        return nullptr;
    }

    const std::optional<std::string> text = asked_text(*path);
    if (!text) {
        return nullptr;
    }

    return lsp::formatting(source_file_t(file_id_t{}, to_utf8(*path), *text),
                           m_workspace->unit_of(*path).width, m_positions);
}

json_t server_t::folding_ranges(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<std::string> text = path ? asked_text(*path) : std::nullopt;
    if (!text) {
        return nullptr;
    }

    return lsp::folding_ranges(source_file_t(file_id_t{}, to_utf8(*path), *text));
}

json_t server_t::selection_ranges(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<std::string> text = path ? asked_text(*path) : std::nullopt;
    if (!text) {
        return nullptr;
    }

    const source_file_t file(file_id_t{}, to_utf8(*path), *text);
    std::vector<std::uint32_t> offsets;
    for (const json_t& position : params.at("positions")) {
        offsets.push_back(m_positions.offset(file, position));
    }

    return lsp::selection_ranges(file, offsets, m_positions);
}

json_t server_t::document_highlights(const json_t& params) {
    return at_last_check(params, [&](const query_t& query, const std::uint32_t offset) {
        return lsp::document_highlights(query, offset, m_positions);
    });
}

json_t server_t::inlay_hints(const json_t& params) {
    // The last check's hints in the range, each where it is in the document now; those in what
    // changed since come with the next check, which the client's told to ask for.
    const std::optional<last_check_t> last = last_check(params);
    if (!last) {
        return nullptr;
    }

    const json_t& range = params.at("range");
    const std::uint32_t begin = m_positions.offset(last->now, range.at("start"));
    const std::uint32_t end = m_positions.offset(last->now, range.at("end"));
    const span_t span{
        .file = last->checked.file,
        .begin = last->to_then(begin).value_or(std::min(begin, last->prefix)),
        .end = last->to_then(end).value_or(static_cast<std::uint32_t>(last->then().text().size()) -
                                           last->suffix),
    };
    inlay_options_t options;
    {
        const std::scoped_lock lock(m_settings_mutex);
        options = m_inlay;
    }

    const query_t query(*last->checked.compilation, last->checked.file);
    json_t kept = json_t::array();
    for (json_t& hint : lsp::inlay_hints(query, span, options, m_positions)) {
        const std::optional<std::uint32_t> at =
            last->to_now(m_positions.offset(last->then(), hint.at("position")));
        if (at) {
            hint["position"] = m_positions.position(last->now, *at);
            kept.push_back(std::move(hint));
        }
    }

    return kept;
}

json_t server_t::type_definition(const json_t& params) {
    return at_last_check(params, [&](const query_t& query, const std::uint32_t offset) {
        return lsp::type_definition(query, offset, m_positions);
    });
}

json_t server_t::implementations(const json_t& params) {
    const std::optional<target_t> found = target(params);
    if (!found) {
        return json_t::array();
    }

    return lsp::implementations(found->compilations, found->key, m_positions);
}

json_t server_t::call_hierarchy(const std::string& method, const json_t& params) {
    if (method == "textDocument/prepareCallHierarchy") {
        const std::optional<std::filesystem::path> path = document_path(params);
        const std::optional<checked_t> checked = path ? asked_check(*path) : std::nullopt;
        if (!checked) {
            return nullptr;
        }

        const query_t query(*checked->compilation, checked->file);
        return prepare_call_hierarchy(
            query, m_positions.offset(query.source(), params.at("position")), m_positions);
    }

    const std::optional<std::pair<symbol_key_t, std::filesystem::path>> item =
        item_of(params.at("item"));
    if (!item) {
        return json_t::array();
    }

    // Who calls it is in the checks of its file and what imports it, and what it calls in its
    // file's.
    if (method == "callHierarchy/incomingCalls") {
        return incoming_calls(dependents(item->second), item->first, m_positions);
    }

    if (method != "callHierarchy/outgoingCalls") {
        throw std::invalid_argument("the server doesn't do " + method);
    }

    const std::optional<checked_t> checked = asked_check(item->second);
    if (!checked) {
        return json_t::array();
    }

    return outgoing_calls(query_t(*checked->compilation, checked->file), item->first,
                          params["item"]["data"].value("top", false), m_positions);
}

json_t server_t::type_hierarchy(const std::string& method, const json_t& params) {
    if (method == "textDocument/prepareTypeHierarchy") {
        const std::optional<std::filesystem::path> path = document_path(params);
        const std::optional<checked_t> checked = path ? asked_check(*path) : std::nullopt;
        if (!checked) {
            return nullptr;
        }

        const query_t query(*checked->compilation, checked->file);
        return prepare_type_hierarchy(
            query, m_positions.offset(query.source(), params.at("position")), m_positions);
    }

    const std::optional<std::pair<symbol_key_t, std::filesystem::path>> item =
        item_of(params.at("item"));
    if (!item) {
        return json_t::array();
    }

    if (method == "typeHierarchy/subtypes") {
        return subtypes(dependents(item->second), item->first, m_positions);
    }

    if (method != "typeHierarchy/supertypes") {
        throw std::invalid_argument("the server doesn't do " + method);
    }

    const std::optional<checked_t> checked = asked_check(item->second);
    if (!checked) {
        return json_t::array();
    }

    return supertypes(query_t(*checked->compilation, checked->file), item->first, m_positions);
}

json_t server_t::code_lenses(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<std::string> text = path ? asked_text(*path) : std::nullopt;
    if (!text) {
        return nullptr;
    }

    const std::string uri = params.at("textDocument").at("uri").get<std::string>();
    json_t lenses =
        lsp::code_lenses(source_file_t(file_id_t{}, to_utf8(*path), *text), uri, m_positions);
    // Each says which version of the document it's for, which a resolve of it checks.
    if (const std::optional<std::pair<std::string, std::int64_t>> document =
            m_workspace->document(*path)) {
        for (json_t& lens : lenses) {
            lens["data"]["version"] = document->second;
        }
    }
    // A program's main file runs and debugs from its top, as the editor's Run menu does.
    const unit_t unit = m_workspace->unit_of(*path);
    if (unit.kind == unit_t::kind_t::LOOSE || (unit.kind == unit_t::kind_t::PACKAGE && unit.main &&
                                               file_key(*unit.main) == file_key(*path))) {
        const json_t top{
            { "start", { { "line", 0 }, { "character", 0 } } },
            { "end", { { "line", 0 }, { "character", 0 } } },
        };
        for (const auto& [title, command] :
             { std::pair("Debug", "futureblitz.debug"), std::pair("Run", "futureblitz.run") }) {
            lenses.insert(lenses.begin(),
                          json_t{
                              { "range", top },
                              { "command", { { "title", title }, { "command", command } } },
                          });
        }
    }

    return lenses;
}

json_t server_t::resolve_code_lens(json_t lens) {
    const json_t data = lens.value("data", json_t());
    if (!data.is_object() || lens.contains("command")) {
        return lens;
    }

    // A lens of a version the document has moved on from, or will once a change waiting behind
    // it is read, is the client's to ask for again, before its check and after.
    const std::string uri = data.at("uri").get<std::string>();
    const json_t params{
        { "textDocument", { { "uri", uri } } },
        { "position", data.at("position") },
    };
    const std::optional<std::filesystem::path> path = document_path(params);
    const auto stale = [&] {
        const std::optional<std::pair<std::string, std::int64_t>> document =
            path ? m_workspace->document(*path) : std::nullopt;
        if ((data.contains("version") &&
             (!document || document->second != data.at("version").get<std::int64_t>())) ||
            (m_inbox != nullptr && m_inbox->changes(uri))) {
            throw content_modified_t("the document changed since its code lenses were given");
        }
    };
    stale();

    const std::optional<checked_t> checked = path ? asked_check(*path) : std::nullopt;
    if (!checked) {
        return lens;
    }

    const query_t query(*checked->compilation, checked->file);
    const std::optional<named_t> named =
        query.name_at(m_positions.offset(query.source(), data.at("position")));
    if (!named) {
        return lens;
    }

    // One check of the document and what imports it counts the references of all its lenses,
    // until it changes.
    std::vector<std::shared_ptr<const compilation_t>> compilations{ checked->compilation };
    if (const std::optional<std::filesystem::path> declared = query.declared_in(named->symbol)) {
        compilations = dependents(*declared);
    }

    stale();
    const symbol_key_t key = query.key(named->symbol);
    const auto kept = [this](const std::filesystem::path& file) {
        return m_workspace->owned(file) || m_workspace->document(file).has_value();
    };
    const json_t found =
        lsp::references(compilations, std::span(&key, 1), false, kept, m_positions);
    const std::size_t count = found.size();
    lens["command"] = {
        { "title", count == 1 ? std::string("1 reference") : std::format("{} references", count) },
        { "command", "futureblitz.showReferences" },
        { "arguments", json_t::array({ data.at("uri"), data.at("position"), found }) },
    };
    return lens;
}

json_t server_t::tests() {
    json_t out = json_t::array();
    for (const tests_found_t& found : m_workspace->tests()) {
        out.push_back(tests_json(found));
    }

    return out;
}

json_t server_t::tests_json(const tests_found_t& found) const {
    json_t tests = json_t::array();
    const compilation_t& compilation = *found.compilation;
    for (const auto& [name, function] : found.tests) {
        const symbol_t& symbol = compilation.analysis.symbol(function);
        span_t whole = symbol.span;
        if (symbol.declaration) {
            const stmt_t& statement = compilation.ast.get(*symbol.declaration);
            whole = {
                .file = statement.span.file,
                .begin = declaration_begin(statement),
                .end = statement.span.end,
            };
        }

        tests.push_back({
            { "name", name },
            { "label", std::string(symbol.name) },
            { "range", m_positions.range(compilation.sources.get(whole.file), whole) },
        });
    }

    return { { "uri", uri_of(found.path) }, { "tests", std::move(tests) } };
}

void server_t::set_inlay(const json_t& settings) {
    const std::scoped_lock lock(m_settings_mutex);
    if (settings.is_object()) {
        m_inlay.types = settings.value("types", m_inlay.types);
        m_inlay.parameters = settings.value("parameterNames", m_inlay.parameters);
    }
}

std::vector<std::string> server_t::import_names(const std::filesystem::path& path,
                                                const compilation_t& compilation) {
    std::set<std::string> names;
    // std's, found once.
    if (m_std_modules.empty()) {
        std::set<std::string> found;
        const std::filesystem::path& folder = toolchain().std;
        unit_modules(
            { .kind = unit_t::kind_t::STANDARD, .root = folder, .files = package_files(folder) },
            "std", found);
        found.erase("std");
        m_std_modules.assign(found.begin(), found.end());
    }

    names.insert(m_std_modules.begin(), m_std_modules.end());
    // The package's own, and those of the packages its check loaded.
    const unit_t unit = m_workspace->unit_of(path);
    if (unit.kind == unit_t::kind_t::PACKAGE && !unit.name.empty()) {
        unit_modules(unit, unit.name, names);
    }

    for (const module_t& module : compilation.ast.modules().modules()) {
        if (!module.name.empty() && !module.path.starts_with("std/") &&
            !module.name.contains('(')) {
            names.insert(module.name);
        }
    }

    // A package's own files don't import its tests/ folder, whose tests import it as another
    // package does.
    const std::string tests = file_key(unit.root / "tests");
    const std::string key = file_key(path);
    if (unit.kind == unit_t::kind_t::PACKAGE && !unit.name.empty() && key != tests &&
        !key.starts_with(tests + "/")) {
        std::erase_if(names, [&](const std::string& name) {
            return name == unit.name + ".tests" || name.starts_with(unit.name + ".tests.");
        });
    }

    return { names.begin(), names.end() };
}

} // namespace fb::lsp
