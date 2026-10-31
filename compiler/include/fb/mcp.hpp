#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// `fb mcp`: a Model Context Protocol server, which gives an assistant tools that know
// FutureBlitz: searching the spec and std's reference, looking up a name, checking code and
// reading its errors, and formatting code.
namespace fb {

/// The primer on FutureBlitz that `fb mcp` gives an assistant as its instructions, and that
/// `fb new` writes as a package's AGENTS.md, in Markdown.
[[nodiscard]] std::string_view agents_primer();

/// The server, for the package at `root`, or the folder of loose files it is: it answers each
/// message of MCP's JSON-RPC 2.0, one a line, as stdio carries them. It reads the standard
/// library's reference the first time a tool needs it, and the package each time a tool looks at
/// it, so what's saved since is what it sees.
class mcp_server_t {
public:
    explicit mcp_server_t(std::filesystem::path root);
    mcp_server_t(const mcp_server_t&) = delete;
    mcp_server_t& operator=(const mcp_server_t&) = delete;
    mcp_server_t(mcp_server_t&&) noexcept;
    mcp_server_t& operator=(mcp_server_t&&) noexcept;
    ~mcp_server_t();

    /// The answer to one message, a line of JSON, or nullopt for a notification, which has none.
    /// A line that isn't JSON, or isn't a request, gets JSON-RPC's error.
    [[nodiscard]] std::optional<std::string> handle(std::string_view line);

private:
    struct state_t;
    std::unique_ptr<state_t> m_state;
};

} // namespace fb
