#pragma once

#include <functional>

#ifdef _WIN32
#include <thread>
#else
#include <pthread.h>
#endif

namespace fb {

/// A thread that can run the compiler, whose passes recurse as deep as code nests, with the stack
/// cmake/stack.cmake gives a main thread for it. A std::thread can't say how big its stack is, and
/// gets 512 KiB on macOS, which code nested a few hundred deep overflows, and 8 MiB on Linux; on
/// Windows it has what the executable's header says, which is that already. Where there isn't the
/// memory for it, it starts with the system's default instead.
class stack_thread_t {
public:
    stack_thread_t() = default;
    explicit stack_thread_t(std::function<void()> work);
#ifndef _WIN32
    /// For the tests, what starts it in place of pthread_create.
    using start_t = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    stack_thread_t(std::function<void()> work, start_t start);
#endif
    stack_thread_t(const stack_thread_t&) = delete;
    stack_thread_t& operator=(const stack_thread_t&) = delete;
    stack_thread_t(stack_thread_t&& other) noexcept;
    stack_thread_t& operator=(stack_thread_t&& other) noexcept;
    /// One that's still joinable ends the process, as a std::thread's does.
    ~stack_thread_t();

    [[nodiscard]] bool joinable() const;
    void join();
    void detach();

private:
#ifdef _WIN32
    std::thread m_thread;
#else
    pthread_t m_thread{};
    bool m_joinable = false;
#endif
};

} // namespace fb
