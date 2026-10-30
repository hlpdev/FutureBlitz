#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fb {

/// A request fb sends a registry.
struct http_request_t {
    std::string method = "GET";
    std::string url;
    /// Its header fields, like { "Authorization", "Bearer ..." }.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    /// What's sent in place of `body`, without a copy of its own: an archive's bytes, which the
    /// caller keeps while it's sent.
    std::string_view upload;
    /// Where a 200's body goes, as it comes, for an archive too large to hold, in place of the
    /// response's body. Empty for the response's body.
    std::filesystem::path save_to;
    /// The most of a body that's taken; one that grows past it stops the transfer, which fails.
    std::uint64_t most_body = std::uint64_t{ 64 } << 20U;
};

/// What a server answered. Moving it can allocate, as its map's comparator is moved, which
/// clang-tidy reports as an exception escaping.
struct http_response_t { // NOLINT(bugprone-exception-escape)
    long status = 0;
    /// Its header fields, by their names in lowercase; a field it sent twice is its last.
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
};

/// Sends requests and gives what came back, or why nothing did, like a name that doesn't
/// resolve. Tests give their own.
using http_client_t =
    std::function<std::expected<http_response_t, std::string>(const http_request_t&)>;

/// Sends `request` with libcurl, over TLS that trusts the OS's roots, following redirects,
/// through the proxy the environment names, and gives what came back: a response for any status.
[[nodiscard]] std::expected<http_response_t, std::string> send_http(const http_request_t& request);

/// `value` with what a URL's query can't hold as is escaped, as %XX.
[[nodiscard]] std::string url_escape(std::string_view value);

} // namespace fb
