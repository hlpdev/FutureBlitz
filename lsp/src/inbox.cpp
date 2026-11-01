#include "fb/lsp/inbox.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace fb::lsp {

void inbox_t::push(json_t message) {
    {
        const std::scoped_lock lock(m_mutex);
        m_messages.push_back(std::move(message));
    }

    m_ready.notify_one();
}

void inbox_t::close() {
    {
        const std::scoped_lock lock(m_mutex);
        m_closed = true;
    }

    m_ready.notify_all();
}

std::optional<json_t> inbox_t::pop() {
    std::unique_lock lock(m_mutex);
    m_ready.wait(lock, [this] { return m_closed || !m_messages.empty(); });
    if (m_messages.empty()) {
        return std::nullopt;
    }

    json_t message = std::move(m_messages.front());
    m_messages.pop_front();
    return message;
}

std::optional<json_t> inbox_t::try_pop() {
    const std::scoped_lock lock(m_mutex);
    if (m_messages.empty()) {
        return std::nullopt;
    }

    json_t message = std::move(m_messages.front());
    m_messages.pop_front();
    return message;
}

bool inbox_t::take_cancel(const json_t& id) {
    const std::scoped_lock lock(m_mutex);
    const auto found = std::ranges::find_if(m_messages, [&](const json_t& message) {
        return message.is_object() && message.value("method", "") == "$/cancelRequest" &&
               message.contains("params") && message["params"].is_object() &&
               message["params"].value("id", json_t()) == id;
    });
    if (found == m_messages.end()) {
        return false;
    }

    m_messages.erase(found);
    return true;
}

bool inbox_t::changes(const std::string_view uri) const {
    const std::scoped_lock lock(m_mutex);
    return std::ranges::any_of(m_messages, [&](const json_t& message) {
        if (!message.is_object()) {
            return false;
        }

        const std::string method = message.value("method", "");
        if (method != "textDocument/didChange" && method != "textDocument/didClose") {
            return false;
        }

        const json_t& params = message.value("params", json_t::object());
        return params.is_object() && params.contains("textDocument") &&
               params["textDocument"].is_object() && params["textDocument"].value("uri", "") == uri;
    });
}

} // namespace fb::lsp
