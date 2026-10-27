// std.image's runtime parts on input made to make them hold memory, measured by the most of the
// heap operator new holds while they run: a GIF frame whose descriptor is far larger than its
// screen holds no more than the screen, resizing to a long thin image little more than the image
// and what's between its passes, and a font whose glyph is made to be slow to draw holds little
// and takes little time.

#include "fb/image.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>
#include <tuple>
#include <vector>

#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace {

/// The bytes operator new holds, and the most it's held since the last start().
std::atomic<std::int64_t> held{ 0 };
std::atomic<std::int64_t> most{ 0 };

/// A block's size, as malloc has it. A block of the C++ library's, which on some targets has
/// an operator new of its own, is counted by the same, so either frees what the other gave.
std::size_t size_of(void* block) {
#ifdef _WIN32
    return _msize(block);
#elifdef __APPLE__
    return malloc_size(block);
#else
    return malloc_usable_size(block);
#endif
}

/// Starts measuring, from what's held now.
std::int64_t start() {
    const std::int64_t now = held.load();
    most.store(now);
    return now;
}

/// An array of the bytes, as std.image passes the runtime a UInt8[].
fb_array_t array_of(std::vector<std::uint8_t>& bytes) {
    return {
        .header = {},
        .count = static_cast<std::int64_t>(bytes.size()),
        .capacity = static_cast<std::int64_t>(bytes.size()),
        .data = bytes.data(),
    };
}

/// Big-endian bytes of `value`, `count` of them, added to `bytes`.
void put(std::vector<std::uint8_t>& bytes, const std::uint64_t value, const int count) {
    for (int i = count - 1; i >= 0; --i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8U * static_cast<unsigned>(i))));
    }
}

void pad(std::vector<std::uint8_t>& bytes) {
    while (bytes.size() % 4 != 0) {
        bytes.push_back(0);
    }
}

/// A TrueType font of `units` an em whose Z is one contour of `points` points off the curve,
/// zigzagging 30000 units across, and whose B is a composite of `parts` Zs.
std::vector<std::uint8_t> zigzag_font(const int units, const int points, const int parts) {
    std::vector<std::uint8_t> head;
    put(head, 0x00010000, 4);
    put(head, 0x00010000, 4);
    put(head, 0, 4);
    put(head, 0x5F0F3CF5, 4);
    put(head, 0, 2);
    put(head, static_cast<std::uint64_t>(units), 2);
    head.resize(head.size() + 28, 0);
    put(head, 2, 2);
    put(head, 1, 2);
    put(head, 0, 2);

    std::vector<std::uint8_t> hhea;
    put(hhea, 0x00010000, 4);
    put(hhea, 800, 2);
    put(hhea, 65536 - 200, 2);
    put(hhea, 0, 2);
    hhea.resize(hhea.size() + 24, 0);
    put(hhea, 3, 2);

    std::vector<std::uint8_t> maxp;
    put(maxp, 0x00005000, 4);
    put(maxp, 3, 2);

    std::vector<std::uint8_t> hmtx;
    for (int i = 0; i < 3; ++i) {
        put(hmtx, 500, 2);
        put(hmtx, 0, 2);
    }

    // cmap's format 4: B to glyph 2 and Z to glyph 1.
    std::vector<std::uint8_t> cmap;
    for (const int value : {
             0,
             1,
             3,
             1,
             0,
             12,
             4,
             40,
             0,
             6,
             4,
             1,
             2,
             0x42,
             0x5A,
             0xFFFF,
             0,
             0x42,
             0x5A,
             0xFFFF,
             (2 - 0x42 + 65536) % 65536,
             (1 - 0x5A + 65536) % 65536,
             1,
             0,
             0,
             0,
         }) {
        put(cmap, static_cast<std::uint64_t>(value), 2);
    }

    // glyf: 0 empty, 1 the zigzag, 2 the composite.
    std::vector<std::uint8_t> glyf;
    const std::size_t zigzag = glyf.size();
    put(glyf, 1, 2);
    glyf.resize(glyf.size() + 8, 0);
    put(glyf, static_cast<std::uint64_t>(points - 1), 2);
    put(glyf, 0, 2);
    for (int flagged = 0; flagged < points; flagged += 256) {
        glyf.push_back(0x2C);
        glyf.push_back(255);
    }
    for (int i = 0; i < points; ++i) {
        put(glyf, i % 2 == 0 ? 30000 : 65536 - 30000, 2);
    }
    glyf.resize(glyf.size() + static_cast<std::size_t>(points), 1);
    pad(glyf);
    const std::size_t composite = glyf.size();
    put(glyf, 65535, 2);
    glyf.resize(glyf.size() + 8, 0);
    for (int i = 0; i < parts; ++i) {
        put(glyf, i + 1 < parts ? 0x0023 : 0x0003, 2);
        put(glyf, 1, 2);
        put(glyf, 0, 2);
        put(glyf, 0, 2);
    }
    pad(glyf);
    const std::size_t end = glyf.size();

    std::vector<std::uint8_t> loca;
    for (const std::size_t offset : { std::size_t{ 0 }, zigzag, composite, end }) {
        put(loca, offset, 4);
    }

    const std::vector<std::pair<std::string_view, std::vector<std::uint8_t>*>> tables{
        { "head", &head }, { "hhea", &hhea }, { "maxp", &maxp }, { "hmtx", &hmtx },
        { "cmap", &cmap }, { "glyf", &glyf }, { "loca", &loca },
    };
    std::vector<std::uint8_t> font;
    put(font, 0x00010000, 4);
    put(font, tables.size(), 2);
    put(font, 0, 6);
    std::size_t offset = 12 + (16 * tables.size());
    for (const auto& [tag, table] : tables) {
        font.insert(font.end(), tag.begin(), tag.end());
        put(font, 0, 4);
        put(font, offset, 4);
        put(font, table->size(), 4);
        offset += (table->size() + 3) / 4 * 4;
    }
    for (const auto& [tag, table] : tables) {
        font.insert(font.end(), table->begin(), table->end());
        pad(font);
    }
    return font;
}

/// A TrueType font whose A is a composite of 16 composites of 8192 parts each, every part one
/// simple glyph of 32767 contours of two points, though the font has only their contour ends and
/// flags.
std::vector<std::uint8_t> fan_out_font() {
    const auto composite = [](std::vector<std::uint8_t>& glyf, const int part, const int count) {
        put(glyf, 0xFFFF, 2);
        glyf.resize(glyf.size() + 8, 0);
        for (int i = 0; i < count; ++i) {
            put(glyf, i + 1 < count ? 0x0022 : 0x0002, 2);
            put(glyf, static_cast<std::uint64_t>(part), 2);
            put(glyf, 0, 2);
        }
    };

    std::vector<std::uint8_t> glyf;
    std::vector<std::size_t> loca{ 0, 0 };
    put(glyf, 32767, 2);
    glyf.resize(glyf.size() + 8, 0);
    for (int i = 0; i < 32767; ++i) {
        put(glyf, (2 * static_cast<std::uint64_t>(i)) + 1, 2);
    }
    put(glyf, 0, 2);
    for (int left = 65534; left > 0; left -= 256) {
        glyf.push_back(0x39);
        glyf.push_back(static_cast<std::uint8_t>(std::min(left, 256) - 1));
    }
    pad(glyf);
    loca.push_back(glyf.size());
    composite(glyf, 1, 8192);
    loca.push_back(glyf.size());
    composite(glyf, 2, 16);
    loca.push_back(glyf.size());

    std::vector<std::uint8_t> locas;
    for (const std::size_t offset : loca) {
        put(locas, offset, 4);
    }
    std::vector<std::uint8_t> head;
    put(head, 0x00010000, 4);
    head.resize(head.size() + 8, 0);
    put(head, 0x5F0F3CF5, 4);
    put(head, 0, 2);
    put(head, 1000, 2);
    head.resize(head.size() + 30, 0);
    put(head, 1, 2);
    put(head, 0, 2);
    std::vector<std::uint8_t> hhea;
    put(hhea, 0x00010000, 4);
    put(hhea, 800, 2);
    put(hhea, 65536 - 200, 2);
    hhea.resize(hhea.size() + 26, 0);
    put(hhea, 4, 2);
    std::vector<std::uint8_t> maxp;
    put(maxp, 0x00005000, 4);
    put(maxp, 4, 2);
    std::vector<std::uint8_t> hmtx;
    for (int i = 0; i < 4; ++i) {
        put(hmtx, 500, 2);
        put(hmtx, 0, 2);
    }
    // cmap's format 4: A to glyph 3.
    std::vector<std::uint8_t> cmap;
    for (const int value : {
             0,
             1,
             3,
             1,
             0,
             12,
             4,
             32,
             0,
             4,
             0,
             0,
             0,
             0x41,
             0xFFFF,
             0,
             0x41,
             0xFFFF,
             65536 + 3 - 0x41,
             1,
             0,
             0,
         }) {
        put(cmap, static_cast<std::uint64_t>(value), 2);
    }

    const std::vector<std::pair<std::string_view, std::vector<std::uint8_t>*>> tables{
        { "cmap", &cmap }, { "glyf", &glyf },  { "head", &head }, { "hhea", &hhea },
        { "hmtx", &hmtx }, { "loca", &locas }, { "maxp", &maxp },
    };
    std::vector<std::uint8_t> font;
    put(font, 0x00010000, 4);
    put(font, tables.size(), 2);
    put(font, 0, 6);
    std::size_t offset = 12 + (16 * tables.size());
    for (const auto& [tag, table] : tables) {
        font.insert(font.end(), tag.begin(), tag.end());
        put(font, 0, 4);
        put(font, offset, 4);
        put(font, table->size(), 4);
        offset += (table->size() + 3) / 4 * 4;
    }
    for (const auto& [tag, table] : tables) {
        font.insert(font.end(), table->begin(), table->end());
        pad(font);
    }
    return font;
}

/// The most of the heap drawing `text` in the font of `bytes` at `size` pixels an em into a 16
/// by 16 image holds, and how long it takes, in seconds.
std::pair<std::int64_t, double> drawn(std::vector<std::uint8_t>& bytes, const std::string_view text,
                                      const double size) {
    const fb_array_t data = array_of(bytes);
    void* font = fb_font_parse(&data);
    fb_string_t* error = fb_font_error(font);
    CHECK(error->length == 0);
    fb_string_release(error);
    std::vector<std::uint8_t> pixels(std::size_t{ 16 } * 16 * 4, 0);
    const fb_array_t image = array_of(pixels);
    fb_string_t* string = fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
    const std::int64_t before = start();
    const auto started = std::chrono::steady_clock::now();
    fb_image_fill_text(&image, 16, 16, font, string, size, 0, 12, 0x000000FF, 1);
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const std::int64_t most_held = most.load() - before;
    fb_string_release(string);
    fb_font_free(font);
    return { most_held, took };
}

} // namespace

// Every test's operator new and delete count what they hold.
void* operator new(const std::size_t size) {
    void* block = std::malloc(size == 0 ? 1 : size);
    if (block == nullptr) {
        throw std::bad_alloc();
    }

    const std::int64_t now = held += static_cast<std::int64_t>(size_of(block));
    std::int64_t before = most.load();
    while (now > before && !most.compare_exchange_weak(before, now)) {}
    return block;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }

    held -= static_cast<std::int64_t>(size_of(pointer));
    std::free(pointer);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, std::size_t /*size*/) noexcept {
    operator delete(pointer);
}

TEST_CASE("a GIF frame far larger than its screen holds no more memory than the screen") {
    // A 1 by 1 GIF whose frame says it's 65535 by 65535, with one pixel's codes.
    std::vector<std::uint8_t> gif{
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x2c, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0x00, 0x02, 0x02, 0x4c, 0x01, 0x00, 0x3b,
    };
    const fb_array_t data = array_of(gif);
    const std::int64_t before = start();
    void* image = fb_image_gif_decode(&data, 1, 100);
    const std::int64_t most_held = most.load() - before;
    CHECK(fb_image_result(image) == 0);
    CHECK(fb_image_width(image) == 1);
    CHECK(fb_image_height(image) == 1);
    fb_image_free(image);
    // Less than a megabyte, where the descriptor would have it 4 GB.
    CHECK(most_held < 1'000'000);
}

TEST_CASE("resizing to a long thin image holds little more memory than the image") {
    std::vector<std::uint8_t> pixel{ 10, 20, 30, 255 };
    const fb_array_t source = array_of(pixel);
    // Two million pixels one way: the image's bytes are 8 MB, and what's between the passes, 16
    // bytes a pixel of the new width by the old height, 32 MB the second way, and the taps a run
    // of columns at a time. Each taps' own vector made it 108 MB and 123 MB.
    for (const auto& [width, height, most_bytes] :
         { std::tuple{ 1, 2'000'000, 12'000'000 }, std::tuple{ 2'000'000, 1, 48'000'000 } }) {
        CAPTURE(width, height);
        fb_array_t output{};
        const std::int64_t before = start();
        fb_image_resize(&source, 1, 1, width, height, 2, &output);
        const std::int64_t most_held = most.load() - before;
        CHECK(output.count == std::int64_t{ width } * height * 4);
        CHECK(static_cast<const std::uint8_t*>(output.data)[output.count - 2] == 30);
        fb_array_free(&output);
        CHECK(most_held < most_bytes);
    }
}

TEST_CASE("a CFF glyph of hundreds of thousands of curves far outside the image draws at once") {
    // A 552-byte CFF font whose A calls a global subroutine 7 deep, 4 times at each level, down to
    // one hhcurveto of 12 curves that swing 32000 units out and back: 196608 cubics, within the
    // reader's limits, which cut into 1024 chords each held 4 GB for 9 s.
    std::vector<std::uint8_t> bytes{
        79,  84,  84,  79,  0,   6,   0,   0,   0,   0,   0,   0,   67,  70,  70,  32,  0,   0,
        0,   0,   0,   0,   0,   108, 0,   0,   1,   36,  99,  109, 97,  112, 0,   0,   0,   0,
        0,   0,   1,   144, 0,   0,   0,   44,  104, 101, 97,  100, 0,   0,   0,   0,   0,   0,
        1,   188, 0,   0,   0,   54,  104, 104, 101, 97,  0,   0,   0,   0,   0,   0,   1,   244,
        0,   0,   0,   36,  104, 109, 116, 120, 0,   0,   0,   0,   0,   0,   2,   24,  0,   0,
        0,   8,   109, 97,  120, 112, 0,   0,   0,   0,   0,   0,   2,   32,  0,   0,   0,   6,
        1,   0,   4,   4,   0,   1,   4,   0,   0,   0,   1,   0,   0,   0,   2,   65,  0,   1,
        4,   0,   0,   0,   1,   0,   0,   0,   18,  29,  0,   0,   1,   14,  17,  29,  0,   0,
        0,   0,   29,  0,   0,   1,   36,  18,  0,   0,   0,   8,   4,   0,   0,   0,   1,   0,
        0,   0,   123, 0,   0,   0,   132, 0,   0,   0,   141, 0,   0,   0,   150, 0,   0,   0,
        159, 0,   0,   0,   168, 0,   0,   0,   177, 0,   0,   0,   186, 28,  125, 0,   28,  131,
        0,   139, 28,  125, 0,   28,  131, 0,   28,  125, 0,   139, 28,  131, 0,   28,  125, 0,
        28,  131, 0,   139, 28,  125, 0,   28,  131, 0,   28,  125, 0,   139, 28,  131, 0,   28,
        125, 0,   28,  131, 0,   139, 28,  125, 0,   28,  131, 0,   28,  125, 0,   139, 28,  131,
        0,   28,  125, 0,   28,  131, 0,   139, 28,  125, 0,   28,  131, 0,   28,  125, 0,   139,
        28,  131, 0,   28,  125, 0,   28,  131, 0,   139, 28,  125, 0,   28,  131, 0,   28,  125,
        0,   139, 28,  131, 0,   28,  125, 0,   28,  131, 0,   139, 28,  125, 0,   28,  131, 0,
        28,  125, 0,   139, 28,  131, 0,   27,  11,  32,  29,  32,  29,  32,  29,  32,  29,  11,
        33,  29,  33,  29,  33,  29,  33,  29,  11,  34,  29,  34,  29,  34,  29,  34,  29,  11,
        35,  29,  35,  29,  35,  29,  35,  29,  11,  36,  29,  36,  29,  36,  29,  36,  29,  11,
        37,  29,  37,  29,  37,  29,  37,  29,  11,  38,  29,  38,  29,  38,  29,  38,  29,  11,
        0,   2,   4,   0,   0,   0,   1,   0,   0,   0,   2,   0,   0,   0,   8,   14,  139, 139,
        21,  39,  29,  14,  0,   0,   0,   1,   0,   3,   0,   1,   0,   0,   0,   12,  0,   4,
        0,   32,  0,   0,   0,   4,   0,   0,   0,   0,   0,   0,   0,   65,  255, 255, 0,   0,
        0,   65,  255, 255, 255, 192, 0,   1,   0,   0,   0,   0,   0,   1,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   95,  15,  60,  245, 0,   0,   0,   16,  0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   0,   0,
        3,   32,  255, 56,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   2,   1,   244, 0,   0,
        1,   244, 0,   0,   0,   0,   80,  0,   0,   2,   0,   0,
    };
    const auto [most_held, took] = drawn(bytes, "A", 16);
    CHECK(most_held < 64'000'000);
    CHECK(took < 5);
}

TEST_CASE("a TrueType glyph of a million curves zigzagging past the image draws at once") {
    // Z is one contour of 65535 points off the curve, zigzagging 30000 units across, and B a
    // composite of 15 Zs, within the reader's limits, which held 6.5 GB for 91 s.
    std::vector<std::uint8_t> bytes = zigzag_font(1000, 65535, 15);
    for (const std::string_view text : { "Z", "B" }) {
        CAPTURE(text);
        const auto [most_held, took] = drawn(bytes, text, 12);
        CHECK(most_held < 64'000'000);
        CHECK(took < 5);
    }
}

TEST_CASE("a TrueType composite whose parts fan out reads a bounded amount and has no outline") {
    // 16 composites of 8192 parts each of a glyph of 32767 contour ends and 65535 flags but no
    // coordinates: each of the 65536 visits read every contour end again, 2 billion reads, 58 s.
    std::vector<std::uint8_t> bytes = fan_out_font();
    const auto [most_held, took] = drawn(bytes, "A", 16);
    CHECK(most_held < 64'000'000);
    CHECK(took < 5);
}
