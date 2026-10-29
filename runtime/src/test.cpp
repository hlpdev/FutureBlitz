#include "fb/runtime.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// fb test's runner: a test executable's main hands it its tests, and it runs them, on the pool
// but for the ones that use the main thread's Globals, and reports each one's start, failures,
// and end. fb test reads the report, from the file FB_TEST_REPORT names, and starts the
// executable again after a test that panics, with the tests that didn't finish, which
// FB_TEST_PLAN's file lists. Run alone, the executable runs them all and prints what they did.

namespace {

/// How many Causes a thrown error's message shows after it, as an uncaught error's report does.
constexpr std::size_t max_causes = 100;

/// One test: its name, with its module's first, and the function that runs it, which reports
/// what it throws.
struct test_t {
    std::string name;
    void (*run)();
    /// It uses what only the main thread can, like a Global, so it runs there, one at a time.
    bool main_thread = false;
    /// What it found, for a run without fb test, which prints them once it ends.
    bool failed = false;
    bool skipped = false;
    std::vector<std::string> messages;
};

struct runner_t {
    std::mutex mutex;
    std::vector<test_t> tests;
    /// FB_TEST_REPORT's file, or null without one, when what the tests did is printed.
    std::FILE* report = nullptr;
};

runner_t& runner() {
    static runner_t instance;
    return instance;
}

/// Opens the file at `path`, UTF-8, as std::fopen does with `mode`. On Windows, fopen reads a
/// path in the ANSI code page, so a path beyond it, like a temporary folder under a user's name
/// with letters it doesn't have, opens as UTF-16.
std::FILE* open_file(const std::string& path, const char* mode) {
#ifdef _WIN32
    const auto wide = [](const std::string_view text) {
        const int size =
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring converted(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            converted.data(), size);
        return converted;
    };
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

/// The test the calling thread runs, by its index, or nullopt.
thread_local std::optional<std::size_t> t_test;

/// A record of the report, which fb test reads: its kind, the test's index or -1, and its text's
/// length on a line, then the text, which can have line breaks, and a line break. Each goes out
/// whole, flushed, so a panic right after doesn't lose it.
void record(const std::string_view kind, const std::optional<std::size_t> test,
            const std::string_view text = {}) {
    runner_t& run = runner();
    const std::scoped_lock lock(run.mutex);
    if (run.report == nullptr) {
        if (test && kind == "fail") {
            run.tests[*test].failed = true;
            run.tests[*test].messages.emplace_back(text);
        } else if (test && kind == "skip") {
            run.tests[*test].skipped = true;
            run.tests[*test].messages.emplace_back(text);
        }

        return;
    }

    const long long index = test ? static_cast<long long>(*test) : -1;
    std::fprintf(run.report, "%.*s %lld %llu\n", static_cast<int>(kind.size()), kind.data(), index,
                 static_cast<unsigned long long>(text.size()));
    std::fwrite(text.data(), 1, text.size(), run.report);
    std::fputc('\n', run.report);
    std::fflush(run.report);
}

/// The test the calling thread runs. Expect and the rest are only for a test's own code, so
/// elsewhere they panic with what they'd report.
std::size_t current(const std::string_view message) {
    if (!t_test) {
        const std::string text =
            std::format("{} (only a test's own thread can report to fb test)", message);
        fb_panic(text.c_str(), nullptr);
    }

    return *t_test;
}

/// "IOError: no such file at main.fb:3:5", then a "caused by" line like it for each Cause, as an
/// uncaught error's report has them.
std::string error_text(const fb_object_t* error) {
    const auto fields = [](const fb_object_t* object) -> const fb_error_fields_t& {
        return *reinterpret_cast<const fb_error_fields_t*>(object + 1);
    };
    std::vector<const fb_class_t*> types;
    for (const fb_object_t* at = error; at != nullptr && types.size() <= max_causes;
         at = fields(at).cause) {
        types.push_back(at->type);
    }

    std::string text;
    std::size_t shown = 0;
    const fb_object_t* at = error;
    for (; at != nullptr && shown <= max_causes; ++shown) {
        const fb_error_fields_t& own = fields(at);
        text += std::format("{}{}", shown == 0 ? "" : "\ncaused by ",
                            fb::runtime::shown_class_name(at->type, types));
        if (own.message->length != 0) {
            text += std::format(": {}", fb::runtime::text_of(own.message));
        }

        if (own.line != 0) {
            text +=
                std::format(" at {}:{}:{}", fb::runtime::text_of(own.file), own.line, own.column);
        }

        at = own.cause;
    }

    if (at != nullptr) {
        text += "\ncaused by more errors, which aren't shown";
    }

    return text;
}

/// A panic ends the program; the test that panicked failed, and fb test runs the rest again.
void panicked(const std::string_view message, const fb_location_t* location) {
    std::string text = std::format("panic: {}", message);
    if (location != nullptr) {
        text += std::format(" at {}:{}:{}", location->file, location->line, location->column);
    }

    record("panic", t_test, text);
}

/// Once the Globals are let go of, the objects each test made that are left are its leaks.
void check_leaks() {
    // By test, then by Class, in the order each was first made.
    std::map<std::size_t, std::vector<std::pair<const fb_class_t*, std::int64_t>>> leaks;
    for (const fb::runtime::owned_object_t& object : fb::runtime::owned_objects()) {
        auto& counts = leaks[static_cast<std::size_t>(object.owner - 1)];
        const auto found = std::ranges::find(counts, object.type,
                                             &std::pair<const fb_class_t*, std::int64_t>::first);
        if (found != counts.end()) {
            ++found->second;
        } else {
            counts.emplace_back(object.type, 1);
        }
    }

    for (const auto& [test, counts] : leaks) {
        std::vector<const fb_class_t*> types;
        for (const auto& [type, count] : counts) {
            types.push_back(type);
        }

        for (const auto& [type, count] : counts) {
            record("leak", test,
                   std::format("leak: {} {} {} never released", count,
                               fb::runtime::shown_class_name(type, types),
                               count == 1 ? "object was" : "objects were"));
        }
    }

    record("done", std::nullopt);
}

/// Runs a test on the calling thread, whose objects are the test's while it runs.
void run_test(const std::size_t index) {
    t_test = index;
    fb::runtime::set_object_owner(static_cast<std::int64_t>(index) + 1);
    record("start", index);
    runner().tests[index].run();
    record("end", index);
    fb::runtime::set_object_owner(0);
    t_test.reset();
}

void run_job(void* context) {
    run_test(*static_cast<std::size_t*>(context));
}

/// The plan's tests, by their indexes, one per line after its first, which says "serial" when
/// they run one at a time, on the main thread.
struct plan_t {
    std::vector<std::size_t> tests;
    bool serial = false;
};

std::optional<plan_t> read_plan(const std::string& path, const std::size_t count) {
    std::FILE* file = open_file(path, "rb");
    if (file == nullptr) {
        return std::nullopt;
    }

    std::string text;
    std::vector<char> buffer(4096);
    for (std::size_t read = 0; (read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0;) {
        text.append(buffer.data(), read);
    }

    std::fclose(file);
    plan_t plan;
    std::size_t start = 0;
    for (bool first = true; start < text.size(); first = false) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = std::string_view(text).substr(start, end - start);
        start = end + 1;
        if (first) {
            plan.serial = line == "serial";
            continue;
        }

        std::size_t index = 0;
        bool number = !line.empty();
        for (const char c : line) {
            number = number && c >= '0' && c <= '9';
            index = (index * 10) + static_cast<std::size_t>(c - '0');
        }

        if (number && index < count) {
            plan.tests.push_back(index);
        }
    }

    return plan;
}

/// Without fb test, what each test did, and how many passed.
void print_results(const std::vector<std::size_t>& ran) {
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const std::size_t index : ran) {
        const test_t& test = runner().tests[index];
        std::string_view result = "ok";
        std::size_t* counted = &passed;
        if (test.failed) {
            result = "FAILED";
            counted = &failed;
        } else if (test.skipped) {
            result = "skipped";
            counted = &skipped;
        }

        ++*counted;
        std::printf("test %s ... %.*s\n", test.name.c_str(), static_cast<int>(result.size()),
                    result.data());
        for (const std::string& message : test.messages) {
            std::printf("    %s\n", message.c_str());
        }
    }

    std::printf("%llu passed, %llu failed, %llu skipped\n", static_cast<unsigned long long>(passed),
                static_cast<unsigned long long>(failed), static_cast<unsigned long long>(skipped));
    std::fflush(stdout);
}

} // namespace

extern "C" {

void fb_test_add(const fb_string_t* name, void (*run)(), const bool main_thread) {
    runner().tests.push_back({
        .name = std::string(fb::runtime::text_of(name)),
        .run = run,
        .main_thread = main_thread,
    });
}

void fb_test_run() {
    runner_t& run = runner();
    const std::optional<std::string> report = fb::runtime::environment("FB_TEST_REPORT");
    if (report) {
        run.report = open_file(*report, "ab");
        if (run.report == nullptr) {
            const std::string message = std::format("can't write fb test's report, {}", *report);
            fb_panic(message.c_str(), nullptr);
        }
    }

    plan_t plan;
    if (const std::optional<std::string> path = fb::runtime::environment("FB_TEST_PLAN")) {
        std::optional<plan_t> read = read_plan(*path, run.tests.size());
        if (!read) {
            const std::string message = std::format("can't read fb test's plan, {}", *path);
            fb_panic(message.c_str(), nullptr);
        }

        plan = std::move(*read);
    } else {
        for (std::size_t i = 0; i < run.tests.size(); ++i) {
            plan.tests.push_back(i);
        }
    }

    fb::runtime::at_panic(panicked);
    if (report) {
        fb::runtime::at_leak_check(check_leaks);
    }

    // The ones that can run on another thread go to the pool, each a job, while the main thread
    // runs the rest. A serial plan runs them all there, one at a time, so a crash names its test.
    std::vector<fb_job_t*> jobs;
    for (std::size_t& index : plan.tests) {
        if (!plan.serial && !run.tests[index].main_thread) {
            jobs.push_back(fb_job_start(run_job, &index));
        }
    }

    for (const std::size_t index : plan.tests) {
        if (plan.serial || run.tests[index].main_thread) {
            run_test(index);
        }
    }

    for (fb_job_t* job : jobs) {
        fb_job_wait(job);
        fb_job_release(job);
    }

    if (!report) {
        print_results(plan.tests);
    }
}

void fb_test_failed(const fb_string_t* message) {
    const std::string_view text = fb::runtime::text_of(message);
    record("fail", current(text), text);
}

void fb_test_skipped(const fb_string_t* reason) {
    const std::string_view text = fb::runtime::text_of(reason);
    const std::size_t test = current(std::format("skipped: {}", text));
    record("skip", test, text);
}

void fb_test_threw(const fb_object_t* error) {
    const std::string text = std::format("threw {}", error_text(error));
    record("fail", current(text), text);
}

void fb_test_threw_other(const fb_string_t* message, const fb_object_t* error) {
    const std::string text =
        std::format("{}, but it threw {}", fb::runtime::text_of(message), error_text(error));
    record("fail", current(text), text);
}
}
