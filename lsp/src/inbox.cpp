#include "fb/lsp/inbox.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace fb::lsp {

void liveness_t::handling() {
    m_since.store(std::chrono::steady_clock::now().time_since_epoch().count());
}

void liveness_t::handled() {
    m_since.store(std::chrono::steady_clock::duration::max().count());
}

bool liveness_t::going() const {
    return !held_for(m_limit);
}

bool liveness_t::held_for(const std::chrono::milliseconds time) const {
    const auto since = m_since.load();
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return since != std::chrono::steady_clock::duration::max().count() &&
           now - std::chrono::steady_clock::duration(since) >= time;
}

namespace {

/// An object's string at `key`, or "" when it has none, or something else there.
std::string_view text_at(const json_t& object, const char* key) {
    if (!object.is_object()) {
        return {};
    }

    const auto found = object.find(key);
    return found != object.end() && found->is_string()
               ? std::string_view(found->get_ref<const std::string&>())
               : std::string_view();
}

/// An object's object at `key`, or null when it has none.
const json_t& object_in(const json_t& object, const char* key) {
    static const json_t none;
    if (!object.is_object()) {
        return none;
    }

    const auto found = object.find(key);
    return found != object.end() && found->is_object() ? *found : none;
}

} // namespace

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
        const json_t& params = object_in(message, "params");
        return text_at(message, "method") == "$/cancelRequest" && params.is_object() &&
               params.value("id", json_t()) == id;
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
        const std::string_view method = text_at(message, "method");
        return (method == "textDocument/didChange" || method == "textDocument/didClose") &&
               text_at(object_in(object_in(message, "params"), "textDocument"), "uri") == uri;
    });
}

} // namespace fb::lsp
