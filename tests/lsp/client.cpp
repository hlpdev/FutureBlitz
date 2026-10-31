#include "client.hpp"

#include "fb/driver.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/server.hpp"
#include "fb/source.hpp"
#include "fb/utf8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fb::lsp::test {
namespace {

/// Whether two file URIs name one file.
bool same_file(const std::string& first, const std::string& second) {
    const std::optional<std::filesystem::path> a = path_of(first);
    const std::optional<std::filesystem::path> b = path_of(second);
    return a && b && file_key(*a) == file_key(*b);
}

} // namespace

folder_t::folder_t(const std::vector<std::pair<std::string, std::string>>& files)
    : m_root(std::filesystem::weakly_canonical(temporary_path("lsp", ""))) {
    std::filesystem::create_directories(m_root);
    for (const auto& [file, text] : files) {
        write(file, text);
    }
}

folder_t::~folder_t() {
    remove_temporary(m_root);
}

std::filesystem::path folder_t::path(const std::string& file) const {
    return m_root / path_from_utf8(file);
}

std::string folder_t::uri(const std::string& file) const {
    return uri_of(path(file));
}

void folder_t::write(const std::string& file, const std::string& text) const {
    const std::filesystem::path where = path(file);
    std::filesystem::create_directories(where.parent_path());
    std::ofstream out(where, std::ios::binary);
    out << text;
}

client_t::client_t(options_t options)
    : m_server(
          [this](const json_t& message) {
              {
                  const std::scoped_lock lock(m_mutex);
                  m_received.push_back(message);
              }
              m_arrived.notify_all();
          },
          { .workspace = { .delay = options.delay,
                           .dependents_delay = options.dependents_delay.value_or(options.delay),
                           .load_folders = options.load_folders, }, }) {
    json_t folders = json_t::array();
    for (const std::filesystem::path& folder : options.folders) {
        folders.push_back({ { "uri", uri_of(folder) }, { "name", to_utf8(folder.filename()) } });
    }

    json_t capabilities = json_t::object();
    if (!options.encodings.empty()) {
        capabilities["general"] = { { "positionEncodings", options.encodings } };
    }

    m_initialized =
        request("initialize", {
                                  { "processId", nullptr },
                                  { "capabilities", capabilities },
                                  { "workspaceFolders", folders },
                                  { "initializationOptions", options.initialization_options },
                              });
    notify("initialized", json_t::object());
}

// A test fails when what its client says as it goes fails.
client_t::~client_t() { // NOLINT(bugprone-exception-escape)
    // What a client says as it goes.
    static_cast<void>(send("shutdown", nullptr));
    notify("exit", nullptr);
}

json_t client_t::send(const std::string& method, json_t params) {
    const std::int64_t id = m_next_id++;
    m_server.handle({
        { "jsonrpc", "2.0" },
        { "id", id },
        { "method", method },
        { "params", std::move(params) },
    });
    const std::scoped_lock lock(m_mutex);
    for (const json_t& message : m_received) {
        if (message.contains("id") && message["id"] == id && !message.contains("method")) {
            return message;
        }
    }

    FAIL("no response to " << method);
    return nullptr;
}

json_t client_t::request(const std::string& method, json_t params) {
    const json_t response = send(method, std::move(params));
    INFO(response.dump());
    REQUIRE(response.contains("result"));
    return response["result"];
}

void client_t::notify(const std::string& method, json_t params) {
    m_server.handle(
        { { "jsonrpc", "2.0" }, { "method", method }, { "params", std::move(params) } });
}

void client_t::open(const std::string& uri, const std::string_view text) {
    m_texts[uri] = text;
    m_versions[uri] = 1;
    mark(uri);
    const json_t document{
        { "uri", uri },
        { "languageId", "futureblitz" },
        { "version", 1 },
        { "text", text },
    };
    notify("textDocument/didOpen", { { "textDocument", document } });
}

void client_t::change(const std::string& uri, const std::string_view text) {
    m_texts[uri] = text;
    const std::int64_t version = ++m_versions[uri];
    mark(uri);
    notify("textDocument/didChange",
           {
               { "textDocument", { { "uri", uri }, { "version", version } } },
               { "contentChanges", json_t::array({ { { "text", text } } }) },
           });
}

void client_t::edit(const std::string& uri, const json_t& changes, const column_unit_t unit) {
    // A BOM is the text's, which a source file leaves out of its offsets.
    std::string& text = m_texts.at(uri);
    const positions_t positions{ .unit = unit };
    for (const json_t& change : changes) {
        const std::size_t bom = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
        const source_file_t file(file_id_t{}, "", text);
        const std::uint32_t begin = positions.offset(file, change["range"]["start"]);
        const std::uint32_t end = positions.offset(file, change["range"]["end"]);
        text.replace(bom + begin, end - begin, change["text"].get<std::string>());
    }

    const std::int64_t version = ++m_versions[uri];
    mark(uri);
    notify("textDocument/didChange",
           {
               { "textDocument", { { "uri", uri }, { "version", version } } },
               { "contentChanges", changes },
           });
}

void client_t::close(const std::string& uri) {
    m_texts.erase(uri);
    notify("textDocument/didClose", { { "textDocument", { { "uri", uri } } } });
}

void client_t::mark(const std::string& uri) {
    const std::scoped_lock lock(m_mutex);
    m_changed_at[uri] = m_received.size();
}

void client_t::settle() {
    m_server.workspace().settle();
}

json_t client_t::diagnostics(const std::string& uri) const {
    const std::scoped_lock lock(m_mutex);
    json_t last;
    for (const json_t& message : m_received) {
        if (message.value("method", "") == "textDocument/publishDiagnostics" &&
            same_file(message["params"]["uri"].get<std::string>(), uri)) {
            last = message["params"]["diagnostics"];
        }
    }

    INFO("diagnostics for " << uri);
    REQUIRE(last.is_array());
    return last;
}

json_t client_t::published(const std::string& uri, const std::chrono::milliseconds timeout) {
    const std::int64_t version = m_versions.at(uri);
    std::unique_lock lock(m_mutex);
    std::size_t next = m_changed_at.at(uri);
    json_t found;
    const bool arrived = m_arrived.wait_for(lock, timeout, [&] {
        for (; next < m_received.size(); ++next) {
            const json_t& message = m_received[next];
            if (message.value("method", "") == "textDocument/publishDiagnostics" &&
                message["params"].value("version", std::int64_t{ -1 }) == version &&
                same_file(message["params"]["uri"].get<std::string>(), uri)) {
                found = message["params"]["diagnostics"];
                return true;
            }
        }

        return false;
    });

    INFO("diagnostics for " << uri << " at version " << version);
    REQUIRE(arrived);
    return found;
}

std::size_t client_t::publications(const std::string& uri) const {
    const std::scoped_lock lock(m_mutex);
    std::size_t count = 0;
    for (const json_t& message : m_received) {
        if (message.value("method", "") == "textDocument/publishDiagnostics" &&
            same_file(message["params"]["uri"].get<std::string>(), uri)) {
            ++count;
        }
    }

    return count;
}

std::vector<json_t> client_t::notifications() const {
    const std::scoped_lock lock(m_mutex);
    std::vector<json_t> found;
    for (const json_t& message : m_received) {
        if (message.contains("method")) {
            found.push_back(message);
        }
    }

    return found;
}

json_t client_t::position(const std::uint32_t line, const std::uint32_t character) {
    return { { "line", line }, { "character", character } };
}

json_t client_t::at(const std::string& uri, const std::string& text,
                    const std::uint32_t offset) const {
    const std::string& document = m_texts.at(uri);
    const std::size_t found = document.find(text);
    REQUIRE(found != std::string::npos);
    const std::size_t where = found + offset;
    std::uint32_t line = 0;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < where; ++i) {
        if (document[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }

    return {
        { "textDocument", { { "uri", uri } } },
        { "position", position(line, static_cast<std::uint32_t>(where - line_start)) },
    };
}

place_t place_of(const json_t& location) {
    return {
        .uri =
            uri_of(path_of(location["uri"].get<std::string>()).value_or(std::filesystem::path())),
        .line = location["range"]["start"]["line"].get<std::uint32_t>(),
        .character = location["range"]["start"]["character"].get<std::uint32_t>(),
    };
}

} // namespace fb::lsp::test
