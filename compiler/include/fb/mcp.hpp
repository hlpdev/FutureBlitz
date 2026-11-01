#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// `fb mcp`: a Model Context Protocol server, which gives an assistant tools that know
// FutureBlitz: searching the spec and std's reference, looking up a name, checking code and
// reading its errors, and formatting code.
namespace fb {

/// The primer on FutureBlitz that `fb mcp` gives an assistant as its instructions, and that
/// `fb new` writes as a package's AGENTS.md, in Markdown.
[[nodiscard]] std::string_view agents_primer();

/// How `fb mcp`'s server works.
struct mcp_options_t {
    /// How long a tool call has before it's answered with an error, and left to finish on its own,
    /// its answer unsent.
    std::chrono::milliseconds deadline{ 30'000 };
};

/// The server, for the package at `root`, or the folder of loose files it is: it takes each
/// message of MCP's JSON-RPC 2.0, a line each, as stdio carries them, and sends each answer as a
/// line. It reads the standard library's reference on a thread of its own once it's initialized,
/// and the package each time a tool looks at it, so what's saved since is what it sees. Its tools
/// never run a package's build step: they take what its last run made, as fb build left it.
class mcp_server_t {
public:
    /// What takes each answer, a line of JSON without its line break, from the server's threads
    /// too, one at a time.
    using send_t = std::function<void(const std::string& line)>;

    mcp_server_t(const std::filesystem::path& root, send_t send, mcp_options_t options = {});
    mcp_server_t(const mcp_server_t&) = delete;
    mcp_server_t& operator=(const mcp_server_t&) = delete;
    mcp_server_t(mcp_server_t&&) = delete;
    mcp_server_t& operator=(mcp_server_t&&) = delete;
    /// Sends nothing more, once an answer being sent is; calls still running finish on their own.
    ~mcp_server_t();

    /// Takes one message: answers a request at once, but a tool call, which runs on a thread of
    /// its own and is answered when it's done, unless it's cancelled first or its deadline comes.
    /// A notification and a client's response get no answer, and a message that isn't JSON-RPC's
    /// gets its error. A batch, an array of messages, is taken when 2025-03-26 is what the
    /// initialize agreed on, and answered as one.
    void receive(std::string_view line);

    /// Waits until every call taken so far is answered or cancelled, for the tests.
    void settle();

private:
    struct core_t;
    std::shared_ptr<core_t> m_core;
};

} // namespace fb
