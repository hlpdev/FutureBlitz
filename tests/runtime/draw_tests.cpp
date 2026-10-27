// std.image's drawing on what's slow to draw edge by edge: a long line chart, a polygon whose
// edges all cross each other, and a pixel that dozens of edges pass through, which each take
// time in proportion to their pixels' edges and still cover what their regions do.

#include "fb/image.hpp"
#include "fb/runtime.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

/// An array of the values, as std.image passes the runtime a T[].
template <typename T>
fb_array_t array_of(std::vector<T>& values) {
    return {
        .header = {},
        .count = static_cast<std::int64_t>(values.size()),
        .capacity = static_cast<std::int64_t>(values.size()),
        .data = values.data(),
    };
}

/// How long `draw` takes, in seconds.
template <typename Draw>
double seconds_of(const Draw& draw) {
    const auto start = std::chrono::steady_clock::now();
    draw();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

constexpr std::uint32_t black = 0x000000FF;

} // namespace

TEST_CASE("a line chart of thousands of points strokes in time for its pixels, not its edges "
          "squared") {
    // 8000 points across a 400 by 100 image, 1.5 wide, gently up and down over a few rows, whose
    // stroke's edges all end in those rows: it took 80 s before, in a release build.
    std::vector<double> coords;
    for (int i = 0; i < 8000; ++i) {
        coords.push_back(i * 400.0 / 8000);
        coords.push_back(50 + (3 * std::sin(i * 0.05)));
    }
    std::vector<std::int32_t> counts{ 8000 };
    std::vector<std::uint8_t> bytes(std::size_t{ 400 } * 100 * 4, 0);
    const fb_array_t points = array_of(coords);
    const fb_array_t sizes = array_of(counts);
    const double took = seconds_of([&] {
        fb_image_stroke(bytes.data(), 400, 100, &points, &sizes, 0, 1.5, 0, 0, 4, black, 1);
    });
    CHECK(took < 10);
    // The line's middle row is covered where it runs, and nothing far above it.
    CHECK(bytes[((((50 * 400) + 200) * 4) + 3)] > 0);
    CHECK(bytes[((((10 * 400) + 200) * 4) + 3)] == 0);
}

TEST_CASE("a polygon of thousands of edges that each cross every other fills in time") {
    // A zigzag of 3000 points from top to bottom of a 200 by 200 image: 3 minutes before.
    std::vector<double> coords;
    for (int i = 0; i < 3000; ++i) {
        coords.push_back(i * 200.0 / 3000);
        coords.push_back(i % 2 == 0 ? 0 : 200);
    }
    std::vector<std::int32_t> counts{ 3000 };
    std::vector<std::uint8_t> bytes(std::size_t{ 200 } * 200 * 4, 0);
    const fb_array_t points = array_of(coords);
    const fb_array_t sizes = array_of(counts);
    const double took =
        seconds_of([&] { fb_image_fill_rings(bytes.data(), 200, 200, &points, &sizes, black, 1); });
    CHECK(took < 10);
}

TEST_CASE("a pixel dozens of edges pass through covers what their region does") {
    // 40 copies of the square from (0.5, 0) to (1.5, 1), so pixel 0 and pixel 1 have 40 edges
    // each, more than are worked out exactly: by the nonzero rule, the copies cover each half.
    std::vector<double> coords;
    std::vector<std::int32_t> counts;
    for (int copy = 0; copy < 40; ++copy) {
        for (const double value : { 0.5, 0.0, 1.5, 0.0, 1.5, 1.0, 0.5, 1.0 }) {
            coords.push_back(value);
        }
        counts.push_back(4);
    }
    std::vector<std::uint8_t> bytes(8, 0);
    const fb_array_t points = array_of(coords);
    const fb_array_t sizes = array_of(counts);
    fb_image_fill_rings(bytes.data(), 2, 1, &points, &sizes, black, 1);
    CHECK(bytes[3] == 128);
    CHECK(bytes[7] == 128);
}
