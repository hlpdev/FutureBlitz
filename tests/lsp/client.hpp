#pragma once

#include "fb/lsp/protocol.hpp"
#include "fb/lsp/server.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fb::lsp::test {

/// A folder of files in the system's temporary folder, which goes when it does.
class folder_t {
public:
    /// The files, by their paths from the folder.
    explicit folder_t(const std::vector<std::pair<std::string, std::string>>& files);
    folder_t(const folder_t&) = delete;
    folder_t& operator=(const folder_t&) = delete;
    folder_t(folder_t&&) = delete;
    folder_t& operator=(folder_t&&) = delete;
    ~folder_t();

    [[nodiscard]] const std::filesystem::path& root() const {
        return m_root;
    }

    /// A file's path, from its path in the folder.
    [[nodiscard]] std::filesystem::path path(const std::string& file) const;

    /// A file's URI, from its path in the folder.
    [[nodiscard]] std::string uri(const std::string& file) const;

    void write(const std::string& file, const std::string& text) const;

private:
    std::filesystem::path m_root;
};

/// What a test's client does with the server: it initializes it, sends it messages as an editor
/// would, and keeps what it sends back.
class client_t {
public:
    struct options_t {
        std::vector<std::filesystem::path> folders;
        /// How long the server's checks wait after an edit.
        std::chrono::milliseconds delay{ 0 };
        bool load_folders = true;
        json_t initialization_options = json_t::object();
        /// The position encodings the client offers.
        std::vector<std::string> encodings;
    };

    explicit client_t(options_t options);
    client_t(const client_t&) = delete;
    client_t& operator=(const client_t&) = delete;
    client_t(client_t&&) = delete;
    client_t& operator=(client_t&&) = delete;
    ~client_t();

    /// initialize's result.
    [[nodiscard]] const json_t& initialized() const {
        return m_initialized;
    }

    /// A request's response: its whole message, with its result or its error.
    json_t send(const std::string& method, json_t params);

    /// A request's result, which it must have.
    json_t request(const std::string& method, json_t params);

    void notify(const std::string& method, json_t params);

    void open(const std::string& uri, const std::string& text);
    /// Replaces the document's whole text.
    void change(const std::string& uri, const std::string& text);
    void close(const std::string& uri);

    /// Waits until the server's checks are done.
    void settle();

    /// The diagnostics published last for a file, nullopt when none were.
    [[nodiscard]] std::optional<json_t> diagnostics(const std::string& uri) const;

    /// How many times diagnostics were published for a file.
    [[nodiscard]] std::size_t publications(const std::string& uri) const;

    /// The messages the server sent that aren't responses.
    [[nodiscard]] std::vector<json_t> notifications() const;

    /// A position's JSON.
    static json_t position(std::uint32_t line, std::uint32_t character);

    /// textDocument and position params, at `text`'s first occurrence in the document, plus
    /// `offset` characters.
    [[nodiscard]] json_t at(const std::string& uri, const std::string& text,
                            std::uint32_t offset = 0) const;

    [[nodiscard]] server_t& server() {
        return m_server;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<json_t> m_received;
    std::map<std::string, std::string> m_texts;
    std::map<std::string, std::int64_t> m_versions;
    std::int64_t m_next_id = 1;
    json_t m_initialized;
    server_t m_server;
};

/// The uri and range of a Location.
struct place_t {
    std::string uri;
    std::uint32_t line = 0;
    std::uint32_t character = 0;

    friend bool operator==(const place_t&, const place_t&) = default;
    friend auto operator<=>(const place_t&, const place_t&) = default;
};

[[nodiscard]] place_t place_of(const json_t& location);

} // namespace fb::lsp::test
