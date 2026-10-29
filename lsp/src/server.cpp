#include "fb/lsp/server.hpp"

#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/lsp/features.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/lsp/workspace.hpp"
#include "fb/module.hpp"
#include "fb/source.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

/// `text` with one of didChange's contentChanges made.
void apply_change(std::string& text, const json_t& change, const positions_t& positions) {
    const std::string replacement = change.at("text").get<std::string>();
    if (!change.contains("range")) {
        text = replacement;
        return;
    }

    const source_file_t file(file_id_t{}, "", text);
    const std::uint32_t begin = positions.offset(file, change["range"].at("start"));
    const std::uint32_t end = std::max(begin, positions.offset(file, change["range"].at("end")));
    text.replace(begin, end - begin, replacement);
}

} // namespace

server_t::server_t(send_t send, const options_t options) : m_send(std::move(send)) {
    m_workspace = std::make_unique<workspace_t>(
        [this](const published_t& published) { publish(published); }, options.workspace);
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
            send({
                { "jsonrpc", "2.0" },
                { "id", std::format("fb-{}", m_next_request++) },
                { "method", "client/registerCapability" },
                {
                    "params",
                    {
                        {
                            "registrations",
                            json_t::array({
                                {
                                    { "id", "fb-files" },
                                    { "method", "workspace/didChangeWatchedFiles" },
                                    {
                                        "registerOptions",
                                        {
                                            {
                                                "watchers",
                                                json_t::array({
                                                    { { "globPattern", "**/*.fb" } },
                                                    { { "globPattern", "**/project.toml" } },
                                                }),
                                            },
                                        },
                                    },
                                },
                            }),
                        },
                    },
                },
            });
        }

        m_workspace->add_folders(m_folders);
        return;
    }

    if (method == "textDocument/didOpen") {
        const json_t& document = params.at("textDocument");
        if (const std::optional<std::filesystem::path> path =
                path_of(document.at("uri").get<std::string>())) {
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

    if (method == "workspace/didChangeWorkspaceFolders") {
        std::vector<std::filesystem::path> added;
        for (const json_t& folder : params.at("event").value("added", json_t::array())) {
            if (std::optional<std::filesystem::path> path =
                    path_of(folder.at("uri").get<std::string>())) {
                added.push_back(std::move(*path));
            }
        }

        m_workspace->add_folders(added);
    }
}

json_t server_t::initialize(const json_t& params) {
    m_initialized = true;
    const json_t capabilities = params.value("capabilities", json_t::object());
    // UTF-8 is what the compiler counts in, so it's the cheapest when the client can.
    const json_t encodings =
        capabilities.value("general", json_t::object()).value("positionEncodings", json_t::array());
    std::string encoding = "utf-16";
    if (std::ranges::contains(encodings, json_t("utf-8"))) {
        encoding = "utf-8";
        m_positions.unit = column_unit_t::UTF8;
    }

    m_watches = capabilities.value("workspace", json_t::object())
                    .value("didChangeWatchedFiles", json_t::object())
                    .value("dynamicRegistration", false);
    const json_t options = params.value("initializationOptions", json_t::object());
    m_format_on_save = options.is_object() && options.value("formatOnSave", false);

    // The folders, or the one the client opened before it had several.
    if (params.contains("workspaceFolders") && params["workspaceFolders"].is_array()) {
        for (const json_t& folder : params["workspaceFolders"]) {
            if (std::optional<std::filesystem::path> path =
                    path_of(folder.at("uri").get<std::string>())) {
                m_folders.push_back(std::move(*path));
            }
        }
    } else if (params.contains("rootUri") && params["rootUri"].is_string()) {
        if (std::optional<std::filesystem::path> path =
                path_of(params["rootUri"].get<std::string>())) {
            m_folders.push_back(std::move(*path));
        }
    }

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

json_t server_t::references(const json_t& params, const bool declaration, const bool renaming) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
    if (!checked) {
        return nullptr;
    }

    const query_t query(*checked->compilation, checked->file);
    const std::optional<named_t> named =
        query.name_at(m_positions.offset(query.source(), params.at("position")));
    if (!named) {
        return json_t::array();
    }

    // The checks of the file it's declared in and every file that imports it, or for a builtin,
    // this one's.
    std::vector<std::shared_ptr<const compilation_t>> compilations{ checked->compilation };
    if (const std::optional<std::filesystem::path> declared = query.declared_in(named->symbol)) {
        compilations = m_workspace->with_dependents(*declared);
    }

    // A rename changes only the files that are the workspace's to change.
    const auto kept = [this, renaming](const std::filesystem::path& file) {
        return renaming ? editable(file)
                        : m_workspace->owns(file) || m_workspace->document(file).has_value();
    };
    return lsp::references(compilations, query.key(named->symbol), declaration, kept, m_positions);
}

bool server_t::editable(const std::filesystem::path& file) const {
    // std is the workspace's own only when it's in one of its folders.
    const std::string standard = file_key(path_from_utf8(build_config::std_directory)) + "/";
    return m_workspace->owns(file) ||
           (m_workspace->document(file).has_value() && !file_key(file).starts_with(standard));
}

json_t server_t::prepare_rename(const json_t& params) {
    const std::optional<std::filesystem::path> path = document_path(params);
    const std::optional<checked_t> checked = path ? m_workspace->checked(*path) : std::nullopt;
    if (!checked) {
        return nullptr;
    }

    const query_t query(*checked->compilation, checked->file);
    const std::optional<named_t> named =
        query.name_at(m_positions.offset(query.source(), params.at("position")));
    if (!named) {
        return nullptr;
    }

    // Only what the workspace declares, in its own files, can be renamed.
    const std::optional<std::filesystem::path> declared = query.declared_in(named->symbol);
    const symbol_kind_t kind = query.analysis().symbol(named->symbol).kind;
    if (!declared || !editable(*declared) || kind == symbol_kind_t::MODULE) {
        return nullptr;
    }

    return {
        { "range", m_positions.range(query.source(), named->span) },
        { "placeholder", std::string(query.source().slice(named->span)) },
    };
}

json_t server_t::rename(const json_t& params) {
    const std::string name = params.at("newName").get<std::string>();
    if (!is_identifier(name)) {
        throw std::invalid_argument(
            std::format("'{}' isn't a name: a name is letters, digits, and '_', and isn't a "
                        "keyword",
                        name));
    }

    if (prepare_rename(params).is_null()) {
        throw std::runtime_error("only a name the workspace declares can be renamed");
    }

    return rename_edit(references(params, true, true), name);
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
    return lsp::completion(query, m_positions.offset(query.source(), params.at("position")),
                           import_names(*path, *checked->compilation), m_positions);
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

std::vector<std::string> server_t::import_names(const std::filesystem::path& path,
                                                const compilation_t& compilation) {
    std::set<std::string> names;
    // std's, found once.
    if (m_std_modules.empty()) {
        std::set<std::string> found;
        const std::filesystem::path folder = path_from_utf8(build_config::std_directory);
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

    return { names.begin(), names.end() };
}

} // namespace fb::lsp
