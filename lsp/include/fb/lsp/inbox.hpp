#pragma once

#include "fb/lsp/protocol.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>

namespace fb::lsp {

/// The client's messages that are read but not handled yet, which a reader thread adds as they
/// come, so the server can see what's waiting behind the one it's on: a $/cancelRequest of a
/// request it hasn't started, or a change of a document whose code lenses it's asked for.
class inbox_t {
public:
    void push(json_t message);

    /// No more messages come, once the waiting ones are taken.
    void close();

    /// The first waiting message, waiting for one; nullopt once it's closed and empty.
    [[nodiscard]] std::optional<json_t> pop();

    /// The first waiting message, or nullopt without waiting when there's none.
    [[nodiscard]] std::optional<json_t> try_pop();

    /// Whether a $/cancelRequest of `id` is waiting, which it takes out.
    [[nodiscard]] bool take_cancel(const json_t& id);

    /// Whether a didChange or didClose of the document at `uri` is waiting.
    [[nodiscard]] bool changes(std::string_view uri) const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<json_t> m_messages;
    bool m_closed = false;
};

} // namespace fb::lsp
