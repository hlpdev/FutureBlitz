#include "fb/stack_thread.hpp"

#include <functional>
#include <utility>

#ifndef _WIN32
#include <cstddef>
#include <exception>
#include <memory>
#include <pthread.h>
#include <system_error>
#endif

namespace fb {

#ifdef _WIN32

stack_thread_t::stack_thread_t(std::function<void()> work) : m_thread(std::move(work)) {}

stack_thread_t::stack_thread_t(stack_thread_t&& other) noexcept = default;

stack_thread_t& stack_thread_t::operator=(stack_thread_t&& other) noexcept = default;

stack_thread_t::~stack_thread_t() = default;

bool stack_thread_t::joinable() const {
    return m_thread.joinable();
}

void stack_thread_t::join() {
    m_thread.join();
}

void stack_thread_t::detach() {
    m_thread.detach();
}

#else

namespace {

/// What cmake/stack.cmake gives a main thread, which only takes memory as it's used.
constexpr std::size_t stack_size = std::size_t{ 64 } * 1024 * 1024;

extern "C" void* run_stack_thread(void* const work) noexcept {
    const std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(work));
    (*owned)();
    return nullptr;
}

} // namespace

stack_thread_t::stack_thread_t(std::function<void()> work) {
    auto owned = std::make_unique<std::function<void()>>(std::move(work));
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, stack_size);
    const int failed = pthread_create(&m_thread, &attributes, &run_stack_thread, owned.get());
    pthread_attr_destroy(&attributes);
    if (failed != 0) {
        throw std::system_error(failed, std::generic_category(), "a thread couldn't start");
    }

    static_cast<void>(owned.release());
    m_joinable = true;
}

stack_thread_t::stack_thread_t(stack_thread_t&& other) noexcept
    : m_thread(other.m_thread), m_joinable(std::exchange(other.m_joinable, false)) {}

stack_thread_t& stack_thread_t::operator=(stack_thread_t&& other) noexcept {
    if (m_joinable) {
        std::terminate();
    }

    m_thread = other.m_thread;
    m_joinable = std::exchange(other.m_joinable, false);
    return *this;
}

stack_thread_t::~stack_thread_t() {
    if (m_joinable) {
        std::terminate();
    }
}

bool stack_thread_t::joinable() const {
    return m_joinable;
}

void stack_thread_t::join() {
    pthread_join(m_thread, nullptr);
    m_joinable = false;
}

void stack_thread_t::detach() {
    pthread_detach(m_thread);
    m_joinable = false;
}

#endif

} // namespace fb
