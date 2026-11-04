#pragma once

#include "fb/lsp/protocol.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>

namespace fb::lsp {

/// The client's messages that are read but not handled yet, which a reader thread adds as they
/// come, so the server can see what's waiting behind the one it's on: a $/cancelRequest of a
/// request it hasn't started, or a change of a document whose code lenses it's asked for.
/// Whether the message thread's going on, as a thread that can't be held up tells for it: it can be
/// busy with one message for a while, but one that takes this long isn't coming back.
class liveness_t {
public:
    explicit liveness_t(std::chrono::milliseconds limit) : m_limit(limit) {}

    void handling();
    void handled();
    [[nodiscard]] bool going() const;

private:
    std::chrono::milliseconds m_limit;
    /// When the message being handled started, since the clock's start; idle at its maximum.
    std::atomic<std::chrono::steady_clock::rep> m_since{
        std::chrono::steady_clock::duration::max().count(),
    };
};

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
