#include "fb/lsp/server.hpp"

#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/lsp/features.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/lsp/workspace.hpp"
#include "fb/module.hpp"
#include "fb/source.hpp"
#include "fb/toolchain.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
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
#include <utility>
#include <vector>

namespace fb::lsp {
namespace {

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

server_t::server_t(send_t send, const options_t options) : m_send(std::move(send)) {
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

    try {
        if (std::optional<json_t> result = answer(method, params)) {
            respond(id, std::move(*result));
        } else {
            fail(id, error_code_t::METHOD_NOT_FOUND, "the server doesn't do " + method);
        }
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
    if (method == "initialize") {
        return initialize(params);
    }

    if (method == "shutdown") {
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

    if (method == "fb/tests") {
        return tests();
    }

    return std::nullopt;
}

void server_t::notified(const std::string& method, const json_t& params) {
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
        return;
    }

    if (method == "textDocument/didOpen") {
        // Only FutureBlitz code is checked as it's typed; a project.toml, say, is read from disk.
        const json_t& document = params.at("textDocument");
        const std::optional<std::filesystem::path> path =
            path_of(document.at("uri").get<std::string>());
        if (path && is_fb_file(*path)) {
            m_workspace->open(*path, document.value("version", std::int64_t{ 0 }),
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
        if (!document) {
            return;
        }

        for (const json_t& change : params.at("contentChanges")) {
            apply_change(document->first, change, m_positions);
        }

        m_workspace->change(*path, params.at("textDocument").value("version", document->second + 1),
                            std::move(document->first));
        return;
    }

    if (method == "textDocument/didClose") {
        if (const std::optional<std::filesystem::path> path = document_path(params)) {
            m_workspace->close(*path);
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
                { "codeActionProvider", { { "codeActionKinds", json_t::array({ "quickfix" }) } } },
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

json_t server_t::hover(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
    if (!checked) {
        return nullptr;
    }

    const query_t query(*checked->compilation, checked->file);
    return lsp::hover(query, m_positions.offset(query.source(), params.at("position")),
                      m_positions);
}

json_t server_t::definition(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
    if (!checked) {
        return nullptr;
    }

    const query_t query(*checked->compilation, checked->file);
    return lsp::definition(query, m_positions.offset(query.source(), params.at("position")),
                           m_positions);
}

std::optional<server_t::target_t> server_t::target(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
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
        compilations = m_workspace->with_dependents(*declared);
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
        return m_workspace->owns(file) || m_workspace->document(file).has_value();
    };
    return lsp::references(found->compilations, std::span(&found->key, 1), declaration, kept,
                           m_positions);
}

bool server_t::editable(const std::filesystem::path& file) const {
    // std is the workspace's own only when it's in one of its folders.
    const std::string standard = file_key(toolchain().std) + "/";
    return m_workspace->owns(file) ||
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
                std::ranges::move(m_workspace->with_dependents(file),
                                  std::back_inserter(compilations));
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

    const std::optional<checked_t> checked = m_workspace->checked(*path);
    if (!checked) {
        return nullptr;
    }

    const query_t query(*checked->compilation, checked->file);
    // The modules an Import can name take a while to list, so only one being typed lists them.
    return lsp::completion(
        query, m_positions.offset(query.source(), params.at("position")),
        [&] { return import_names(*path, *checked->compilation); },
        [&] { return m_workspace->importable(*path); }, m_positions);
}

json_t server_t::signature_help(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
    if (!checked) {
        return nullptr;
    }

    const query_t query(*checked->compilation, checked->file);
    return lsp::signature_help(query, m_positions.offset(query.source(), params.at("position")),
                               m_positions);
}

json_t server_t::document_symbols(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    if (!path) {
        return nullptr;
    }

    const std::optional<std::string> text = m_workspace->text(*path);
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
            std::optional<std::string> text = m_workspace->text(file);
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
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
    if (!checked) {
        return nullptr;
    }

    return lsp::semantic_tokens(query_t(*checked->compilation, checked->file), m_positions);
}

json_t server_t::code_actions(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
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
    return lsp::code_actions(query, span, m_positions);
}

json_t server_t::formatting(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    if (!path) {
        return nullptr;
    }

    const std::optional<std::string> text = m_workspace->text(*path);
    if (!text) {
        return nullptr;
    }

    return lsp::formatting(source_file_t(file_id_t{}, to_utf8(*path), *text),
                           m_workspace->unit_of(*path).width, m_positions);
}

json_t server_t::folding_ranges(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<std::string> text = path ? m_workspace->text(*path) : std::nullopt;
    if (!text) {
        return nullptr;
    }

    return lsp::folding_ranges(source_file_t(file_id_t{}, to_utf8(*path), *text));
}

json_t server_t::selection_ranges(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<std::string> text = path ? m_workspace->text(*path) : std::nullopt;
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
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
    if (!checked) {
        return nullptr;
    }

    const query_t query(*checked->compilation, checked->file);
    return lsp::document_highlights(
        query, m_positions.offset(query.source(), params.at("position")), m_positions);
}

json_t server_t::inlay_hints(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
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
    return lsp::inlay_hints(query, span, m_inlay, m_positions);
}

json_t server_t::type_definition(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
    if (!checked) {
        return nullptr;
    }

    const query_t query(*checked->compilation, checked->file);
    return lsp::type_definition(query, m_positions.offset(query.source(), params.at("position")),
                                m_positions);
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
        const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
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
        return incoming_calls(m_workspace->with_dependents(item->second), item->first, m_positions);
    }

    if (method != "callHierarchy/outgoingCalls") {
        throw std::invalid_argument("the server doesn't do " + method);
    }

    const std::optional<checked_t> checked = m_workspace->checked(item->second);
    if (!checked) {
        return json_t::array();
    }

    return outgoing_calls(query_t(*checked->compilation, checked->file), item->first,
                          params["item"]["data"].value("top", false), m_positions);
}

json_t server_t::type_hierarchy(const std::string& method, const json_t& params) {
    if (method == "textDocument/prepareTypeHierarchy") {
        const std::optional<std::filesystem::path> path = document_path(params);
        const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
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
        return subtypes(m_workspace->with_dependents(item->second), item->first, m_positions);
    }

    if (method != "typeHierarchy/supertypes") {
        throw std::invalid_argument("the server doesn't do " + method);
    }

    const std::optional<checked_t> checked = m_workspace->checked(item->second);
    if (!checked) {
        return json_t::array();
    }

    return supertypes(query_t(*checked->compilation, checked->file), item->first, m_positions);
}

json_t server_t::code_lenses(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<std::string> text = path ? m_workspace->text(*path) : std::nullopt;
    if (!text) {
        return nullptr;
    }

    const std::string uri = params.at("textDocument").at("uri").get<std::string>();
    json_t lenses =
        lsp::code_lenses(source_file_t(file_id_t{}, to_utf8(*path), *text), uri, m_positions);
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

    const json_t found = references(
        { { "textDocument", { { "uri", data.at("uri") } } }, { "position", data.at("position") } },
        false);
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
