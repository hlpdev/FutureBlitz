#pragma once

// std.text.markdown's part of the runtime, over md4c, an archive of its own that a program links
// only when it calls one of the fb_markdown_ functions.

#include "fb/runtime.hpp"

#include <cstddef>
#include <cstdint>

namespace fb {

/// Whether md4c reads Markdown of this many bytes, which it counts in 32 bits: less than 4 GiB.
/// The fb_markdown_ functions panic at more.
[[nodiscard]] bool markdown_fits(std::size_t bytes);

} // namespace fb

extern "C" {

/// The Markdown as HTML. `extensions` is std's MarkdownExtensions: 1 for tables, 2 task lists, 4
/// strikethrough, and 8 autolinks without '<' and '>'; 0 is CommonMark alone. The caller owns
/// the result.
fb_string_t* fb_markdown_html(const fb_string_t* text, std::int32_t extensions);

/// The Markdown's blocks, spans, and text, in the order md4c reads them, which fb_markdown_free
/// lets go of: entering each block or span, its text, and leaving it. Text in a code block, a
/// code span, or an HTML block is the block's or span's own, and text next to text is one.
void* fb_markdown_parse(const fb_string_t* text, std::int32_t extensions);
void fb_markdown_free(void* events);
std::int64_t fb_markdown_count(const void* events);
/// The event's step: 0 for entering a block or span, 1 for leaving one, and 2 for text.
std::int32_t fb_markdown_step(const void* events, std::int64_t index);
/// Its kind, std's MarkdownKind.
std::int32_t fb_markdown_kind(const void* events, std::int64_t index);
/// A heading's level, an ordered list's start, a task item's 1 for open or 2 for done, and a
/// table cell's alignment, 1 left, 2 center, or 3 right; 0 for the rest.
std::int64_t fb_markdown_number(const void* events, std::int64_t index);
/// Whether a list is tight, or a link an autolink.
std::int32_t fb_markdown_flag(const void* events, std::int64_t index);
/// One of its texts, entities decoded: 0 for its text, 1 a link's URL or an image's source, 2
/// their title, 3 a code block's info string, and 4 its language, the info's first word. The
/// caller owns the result.
fb_string_t* fb_markdown_text(const void* events, std::int64_t index, std::int32_t which);

} // extern "C"
