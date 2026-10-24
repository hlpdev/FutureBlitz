#include "fb/runtime.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

// std.text.regex's patterns, over PCRE2's 8-bit library: UTF-8 with Unicode's classes, as a
// String is, and limits on each search for a match that end it instead of letting it run on. The
// fb_regex_ functions are an archive of their own, so only a program that calls them links PCRE2.
// The compiler's check of a literal pattern (compiler/src/pattern.cpp) compiles it with the same
// options.

using fb::runtime::new_string;
using fb::runtime::text_of;

namespace {

/// std.text.regex's RegexFlags, which the runtime takes as they are.
constexpr std::int32_t ignore_case = 1;
constexpr std::int32_t multiline = 2;
constexpr std::int32_t dot_all = 4;
constexpr std::int32_t extended = 8;

/// fb_regex_match's options: after an empty match, the next try at the same place takes only
/// one that isn't empty there.
constexpr std::int32_t not_empty_here = 1;

/// How many patterns each thread keeps compiled after their last Regex goes.
constexpr std::size_t cached_patterns = 16;

/// A compiled pattern, which each Regex made of it holds, and its thread's cache.
struct regex_t {
    pcre2_code* code;
    std::atomic<std::int64_t> owners;
};

void release(regex_t* regex) {
    if (regex->owners.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        pcre2_code_free(regex->code);
        delete regex;
    }
}

struct cached_t {
    std::string pattern;
    std::int32_t flags;
    regex_t* regex;
};

/// The patterns a thread compiled most recently, the newest first, so a program that makes a
/// Regex of one pattern again, as String's shortcuts do on each call, compiles it once.
struct cache_t {
    std::vector<cached_t> recent;

    cache_t() = default;
    cache_t(const cache_t&) = delete;
    cache_t& operator=(const cache_t&) = delete;
    cache_t(cache_t&&) = delete;
    cache_t& operator=(cache_t&&) = delete;

    ~cache_t() {
        for (const cached_t& cached : recent) {
            release(cached.regex);
        }
    }
};

thread_local cache_t t_cache;

const pcre2_code* code_of(const void* regex) {
    return static_cast<const regex_t*>(regex)->code;
}

const PCRE2_UCHAR* units_of(const std::string_view text) {
    return reinterpret_cast<const PCRE2_UCHAR*>(text.data());
}

void push_int(fb_array_t* into, const std::int32_t value) {
    *static_cast<std::int32_t*>(fb_array_push(into, sizeof value, nullptr)) = value;
}

/// A search's steps so far, and the most it takes.
struct budget_t {
    std::int64_t steps;
    std::int64_t limit;
};

/// PCRE2's own count of a match's steps starts again at each place a search tries, so a search
/// that tries every place can take its limit times the text's length. A callout before each of a
/// pattern's items counts them over the whole search instead, and ends it past its limit.
int count_step(pcre2_callout_block* /*callout*/, void* data) {
    auto* budget = static_cast<budget_t*>(data);
    budget->steps += 1;
    return budget->steps > budget->limit ? PCRE2_ERROR_MATCHLIMIT : 0;
}

/// `pattern` compiled, with a callout before each item, or null after adding PCRE2's error
/// number and the byte it's at to `problem`.
pcre2_code* compile(const std::string_view pattern, const std::int32_t flags, fb_array_t* problem) {
    std::uint32_t options =
        PCRE2_UTF | PCRE2_UCP | PCRE2_NO_UTF_CHECK | PCRE2_NEVER_BACKSLASH_C | PCRE2_AUTO_CALLOUT;
    const auto bits = static_cast<std::uint32_t>(flags);
    if ((bits & static_cast<std::uint32_t>(ignore_case)) != 0) {
        options |= PCRE2_CASELESS;
    }

    if ((bits & static_cast<std::uint32_t>(multiline)) != 0) {
        options |= PCRE2_MULTILINE;
    }

    if ((bits & static_cast<std::uint32_t>(dot_all)) != 0) {
        options |= PCRE2_DOTALL;
    }

    if ((bits & static_cast<std::uint32_t>(extended)) != 0) {
        options |= PCRE2_EXTENDED;
    }

    pcre2_compile_context* context = pcre2_compile_context_create(nullptr);
    if (context == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    // A line ends at "\n", "\r\n", or a lone "\r", as String's SplitLines has it.
    pcre2_set_newline(context, PCRE2_NEWLINE_ANYCRLF);
    int error = 0;
    PCRE2_SIZE offset = 0;
    pcre2_code* code =
        pcre2_compile(units_of(pattern), pattern.size(), options, &error, &offset, context);
    pcre2_compile_context_free(context);
    if (code == nullptr) {
        push_int(problem, error);
        push_int(problem, static_cast<std::int32_t>(offset));
    }

    return code;
}

/// The pattern's own limit `what`, PCRE2_INFO_MATCHLIMIT, DEPTHLIMIT, or HEAPLIMIT, that a
/// (*LIMIT_MATCH=n), (*LIMIT_DEPTH=n), or (*LIMIT_HEAP=n) at its start sets, or the most a
/// std::uint32_t holds without one.
std::uint32_t own_limit(const void* regex, const std::uint32_t what) {
    std::uint32_t value = 0;
    return pcre2_pattern_info(code_of(regex), what, &value) == 0 ? value : UINT32_MAX;
}

std::uint32_t info_of(const void* regex, const std::uint32_t what) {
    std::uint32_t value = 0;
    pcre2_pattern_info(code_of(regex), what, &value);
    return value;
}

/// The name table's entry `index`: its group's number, in its first two bytes, high byte first,
/// then its name, ending in a zero byte.
PCRE2_SPTR entry_of(const void* regex, const std::int32_t index) {
    PCRE2_SPTR table = nullptr;
    pcre2_pattern_info(code_of(regex), PCRE2_INFO_NAMETABLE, static_cast<void*>(&table));
    const std::uint32_t size = info_of(regex, PCRE2_INFO_NAMEENTRYSIZE);
    return table + (static_cast<std::size_t>(index) * size);
}

} // namespace

void* fb_regex_new(const fb_string_t* pattern, const std::int32_t flags, fb_array_t* problem) {
    const std::string_view text = text_of(pattern);
    std::vector<cached_t>& recent = t_cache.recent;
    const auto found = std::ranges::find_if(recent, [&](const cached_t& cached) {
        return cached.flags == flags && cached.pattern == text;
    });
    if (found != recent.end()) {
        std::rotate(recent.begin(), found, found + 1);
        recent.front().regex->owners.fetch_add(1, std::memory_order_relaxed);
        return recent.front().regex;
    }

    pcre2_code* code = compile(text, flags, problem);
    if (code == nullptr) {
        return nullptr;
    }

    // One owner for the Regex, and one for the cache.
    auto* regex = new regex_t{ .code = code, .owners = 2 };
    recent.insert(recent.begin(), { .pattern = std::string(text), .flags = flags, .regex = regex });
    if (recent.size() > cached_patterns) {
        release(recent.back().regex);
        recent.pop_back();
    }

    return regex;
}

fb_string_t* fb_regex_message(const std::int32_t error) {
    std::array<PCRE2_UCHAR, 256> message{};
    const int length = pcre2_get_error_message(error, message.data(), message.size());
    return new_string({ reinterpret_cast<const char*>(message.data()),
                        static_cast<std::size_t>(length < 0 ? 0 : length) });
}

void fb_regex_free(void* regex) {
    release(static_cast<regex_t*>(regex));
}

std::int32_t fb_regex_match(const void* regex, const fb_string_t* subject, const std::int64_t start,
                            const std::int64_t match_limit, const std::int64_t memory_limit,
                            const std::int32_t options, fb_array_t* positions) {
    pcre2_match_data* data = pcre2_match_data_create_from_pattern(code_of(regex), nullptr);
    pcre2_match_context* context = pcre2_match_context_create(nullptr);
    if (data == nullptr || context == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    budget_t budget{ .steps = 0, .limit = match_limit };
    pcre2_set_callout(context, count_step, &budget);
    const auto steps = static_cast<std::uint32_t>(match_limit);
    // PCRE2 counts its memory in kibibytes.
    const auto kibibytes = static_cast<std::uint32_t>((memory_limit + 1023) / 1024);
    pcre2_set_match_limit(context, steps);
    pcre2_set_depth_limit(context, steps);
    pcre2_set_heap_limit(context, kibibytes);
    std::uint32_t flags = PCRE2_NO_UTF_CHECK;
    if (options == not_empty_here) {
        flags |= PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED;
    }

    const std::string_view text = text_of(subject);
    const int found = pcre2_match(code_of(regex), units_of(text), text.size(),
                                  static_cast<PCRE2_SIZE>(start), flags, data, context);
    // Past a limit, the pattern's own is the one it passed where that's the lower one, unless the
    // callout's count of the whole search ended it.
    const auto own = [&](const std::uint32_t what, const std::uint32_t mine) {
        return budget.steps <= budget.limit && own_limit(regex, what) < mine;
    };
    std::int32_t result = 1;
    if (found == PCRE2_ERROR_NOMATCH) {
        result = 0;
    } else if (found == PCRE2_ERROR_MATCHLIMIT) {
        result = own(PCRE2_INFO_MATCHLIMIT, steps) ? -3 : -1;
    } else if (found == PCRE2_ERROR_DEPTHLIMIT) {
        result = own(PCRE2_INFO_DEPTHLIMIT, steps) ? -4 : -1;
    } else if (found == PCRE2_ERROR_HEAPLIMIT) {
        result = own(PCRE2_INFO_HEAPLIMIT, kibibytes) ? -5 : -2;
    } else if (found == PCRE2_ERROR_NOMEMORY) {
        fb_panic("out of memory", nullptr);
    } else if (found < 0) {
        fb_panic("std.text.regex's match failed in PCRE2", nullptr);
    } else {
        // Each group's start and end, or -1 and -1 for one that took no part.
        fb_array_clear(positions);
        const PCRE2_SIZE* vector = pcre2_get_ovector_pointer(data);
        const std::uint32_t pairs = pcre2_get_ovector_count(data);
        for (std::uint32_t i = 0; i < pairs * 2; ++i) {
            push_int(positions,
                     vector[i] == PCRE2_UNSET ? -1 : static_cast<std::int32_t>(vector[i]));
        }
    }

    pcre2_match_context_free(context);
    pcre2_match_data_free(data);
    return result;
}

std::int32_t fb_regex_group_count(const void* regex) {
    return static_cast<std::int32_t>(info_of(regex, PCRE2_INFO_CAPTURECOUNT));
}

std::int32_t fb_regex_has_cr_or_lf(const void* regex) {
    return static_cast<std::int32_t>(info_of(regex, PCRE2_INFO_HASCRORLF));
}

std::int32_t fb_regex_name_count(const void* regex) {
    return static_cast<std::int32_t>(info_of(regex, PCRE2_INFO_NAMECOUNT));
}

fb_string_t* fb_regex_name(const void* regex, const std::int32_t index) {
    return new_string(reinterpret_cast<const char*>(entry_of(regex, index) + 2));
}

std::int32_t fb_regex_name_group(const void* regex, const std::int32_t index) {
    const PCRE2_SPTR entry = entry_of(regex, index);
    const auto high = static_cast<std::uint32_t>(entry[0]);
    const auto low = static_cast<std::uint32_t>(entry[1]);
    return static_cast<std::int32_t>((high << 8U) | low);
}
