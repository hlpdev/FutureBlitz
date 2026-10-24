// std.image's drawing: regions of straight edges filled by the nonzero rule, each pixel by the
// exact part of its square the region covers, or, without antialiasing, by whether its center is
// in it; ellipses as polygons within a hundredth of a pixel of them; and strokes as the region
// their pieces cover together, a rectangle along each segment, a join at each corner, and a cap
// at each end. It computes in doubles, without fused multiply-adds, with sines of its own, so
// every target gives the same pixels.
//
// A row of pixels is cut into bands where an edge starts, ends, or crosses another, so in each
// band the edges go straight across in an order of their own; going across a band, the winding
// says which stretches between edges are filled, trapezoids that don't overlap, and each pixel
// adds up the exact area of them in its square.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using namespace fb::image;

namespace {

struct point_t {
    double x = 0;
    double y = 0;
};

using ring_t = std::vector<point_t>;

/// An edge going down, from (x0, y0) to (x1, y1), and which way it winds.
struct edge_t {
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
    int winding = 0;

    /// Where it is at `y`, from y0 to y1: its ends exactly.
    [[nodiscard]] double x_at(const double y) const {
        if (y <= y0) {
            return x0;
        }

        if (y >= y1) {
            return x1;
        }
        return x0 + ((y - y0) * (x1 - x0) / (y1 - y0));
    }
};

enum cap_t : std::uint8_t { CAP_BUTT = 0, CAP_ROUND = 1, CAP_SQUARE = 2 };
enum join_t : std::uint8_t { JOIN_MITER = 0, JOIN_ROUND = 1, JOIN_BEVEL = 2 };

/// How far an ellipse's polygon is from it at most, in pixels.
constexpr double tolerance = 0.01;
/// The most sides a quarter of an ellipse has.
constexpr int most_sides = 1024;

/// sin(angle), from 0 to pi / 2, by its series to the 19th power, within 3e-16 there, as
/// transform.cpp's sines are, rather than the C library's, which can differ from one OS to
/// another.
double sine(const double angle) {
    const double squared = angle * angle;
    double sum = 0;
    for (int power = 19; power > 1; power -= 2) {
        sum = (1 - sum) * squared / (power * (power - 1));
    }
    return angle * (1 - sum);
}

/// The sides a quarter of an ellipse whose larger radius is `radius` has: the fewest from 2 up
/// whose chords are within the tolerance of its arc.
int quarter_sides(const double radius) {
    constexpr double half_pi = std::numbers::pi / 2;
    int sides = 2;
    while (sides < most_sides) {
        // A chord of an angle a is r(1 - cos(a / 2)) from its arc at most.
        const double half = half_pi / (2.0 * sides);
        if (radius * (1 - sine(half_pi - half)) <= tolerance) {
            break;
        }
        ++sides;
    }
    return sides;
}

/// The ellipse's polygon, from its rightmost point, by quarters that are each the last turned.
ring_t ellipse(const point_t center, const double rx, const double ry) {
    constexpr double half_pi = std::numbers::pi / 2;
    const int sides = quarter_sides(std::max(rx, ry));
    std::vector<double> sines(static_cast<std::size_t>(sides) + 1);
    for (int k = 0; k <= sides; ++k) {
        sines[static_cast<std::size_t>(k)] = sine(half_pi * k / sides);
    }

    ring_t ring;
    for (int quarter = 0; quarter < 4; ++quarter) {
        for (int k = 0; k < sides; ++k) {
            const double s = sines[static_cast<std::size_t>(k)];
            const double c = sines[static_cast<std::size_t>(sides - k)];
            std::array<double, 2> turned{ c, s };
            for (int t = 0; t < quarter; ++t) {
                turned = { -turned[1], turned[0] };
            }
            ring.push_back({ .x = center.x + (rx * turned[0]), .y = center.y + (ry * turned[1]) });
        }
    }
    return ring;
}

/// Twice the ring's area, above 0 for one way around and below for the other.
double twice_area(const ring_t& ring) {
    double sum = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const point_t& a = ring[i];
        const point_t& b = ring[(i + 1) % ring.size()];
        sum += (a.x * b.y) - (b.x * a.y);
    }
    return sum;
}

/// The ring, the way round whose area is above 0, as every piece of a stroke is, so pieces that
/// overlap add their windings rather than cancel them.
ring_t positive(ring_t ring) {
    if (twice_area(ring) < 0) {
        std::ranges::reverse(ring);
    }
    return ring;
}

/// The edges of rings, closed, but those that go across, which cover nothing.
std::vector<edge_t> edges_of(const std::span<const ring_t> rings) {
    std::vector<edge_t> edges;
    for (const ring_t& ring : rings) {
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const point_t a = ring[i];
            const point_t b = ring[(i + 1) % ring.size()];
            if (a.y < b.y) {
                edges.push_back({ .x0 = a.x, .y0 = a.y, .x1 = b.x, .y1 = b.y, .winding = 1 });
            } else if (a.y > b.y) {
                edges.push_back({ .x0 = b.x, .y0 = b.y, .x1 = a.x, .y1 = a.y, .winding = -1 });
            }
        }
    }
    return edges;
}

/// The mean over t from 0 to 1 of u0 + (u1 - u0) t kept from 0 to 1: how much of a pixel's
/// column a line covers to its left, from u0 to u1 across it, in pieces whose sizes divide out
/// exactly where they're the whole.
double mean_covered(const double u0, const double u1) {
    const double a = std::min(u0, u1);
    const double b = std::max(u0, u1);
    if (b - a == 0) {
        return std::clamp(a, 0.0, 1.0);
    }

    double mean = 0;
    const double low = std::max(a, 0.0);
    const double high = std::min(b, 1.0);
    if (high > low) {
        mean += ((high - low) / (b - a)) * ((high + low) / 2);
    }
    if (b > 1) {
        mean += (b - std::max(a, 1.0)) / (b - a);
    }
    return mean;
}

/// A row's coverage, a pixel's for each column, and the part every column to the left of one
/// has, which a sum across adds.
struct row_t {
    std::vector<double> cover;
    std::vector<double> left;

    explicit row_t(const std::size_t width) : cover(width, 0), left(width + 1, 0) {}

    /// Adds `sign` times what a line from xa at ya to xb at yb covers of each pixel left of it.
    void add(const double sign, const double xa, const double xb, const double height) {
        const auto width = static_cast<double>(cover.size());
        const double low = std::min(xa, xb);
        const double high = std::max(xa, xb);
        // Every column wholly to the line's left is covered for its height.
        const double first = std::clamp(std::floor(low), 0.0, width);
        left[0] += sign * height;
        left[static_cast<std::size_t>(first)] -= sign * height;
        const double last = std::clamp(std::ceil(high), 0.0, width);
        for (auto column = static_cast<std::size_t>(first); column < static_cast<std::size_t>(last);
             ++column) {
            const auto at = static_cast<double>(column);
            cover[column] += sign * height * mean_covered(xa - at, xb - at);
        }
    }

    /// Each pixel's coverage, from 0 to 1.
    void finish() {
        double running = 0;
        for (std::size_t column = 0; column < cover.size(); ++column) {
            running += left[column];
            cover[column] = std::clamp(cover[column] + running, 0.0, 1.0);
        }
    }
};

/// An edge across a band: where it is at the band's top and bottom.
struct across_t {
    const edge_t* edge;
    double top;
    double bottom;
};

/// Sorts the edges by where they are in the middle of the band, as they were in the band before
/// but for the ones that crossed between, so mostly in order already.
void sort_by_middle(std::vector<across_t>& across) {
    for (std::size_t i = 1; i < across.size(); ++i) {
        for (std::size_t j = i;
             j > 0 && across[j - 1].top + across[j - 1].bottom > across[j].top + across[j].bottom;
             --j) {
            std::swap(across[j - 1], across[j]);
        }
    }
}

/// Adds what the edges fill of a band from `low` to `high`, where none crosses another, so each
/// stretch the winding fills between two of them is a trapezoid.
void fill_band(std::vector<across_t>& across, const double low, const double high, row_t& row) {
    for (across_t& edge : across) {
        edge.top = edge.edge->x_at(low);
        edge.bottom = edge.edge->x_at(high);
    }
    sort_by_middle(across);

    int winding = 0;
    const across_t* left = nullptr;
    for (const across_t& edge : across) {
        const int was = winding;
        winding += edge.edge->winding;
        if (was == 0 && winding != 0) {
            left = &edge;
        } else if (was != 0 && winding == 0 && left != nullptr) {
            row.add(1, edge.top, edge.bottom, high - low);
            row.add(-1, left->top, left->bottom, high - low);
        }
    }
}

/// The coverage of a row of pixels, from `top` to top + 1, by the edges that reach it: cut where
/// an edge starts or ends, then where two cross, which in a part no edge starts or ends in are
/// the pairs whose order at its bottom isn't their order at its top, each a swap of an
/// insertion sort from the one order to the other.
void cover_row(const std::span<const edge_t* const> edges, const double top, row_t& row) {
    const double bottom = top + 1;
    std::vector<double> ends{ top, bottom };
    for (const edge_t* edge : edges) {
        if (edge->y0 > top && edge->y0 < bottom) {
            ends.push_back(edge->y0);
        }
        if (edge->y1 > top && edge->y1 < bottom) {
            ends.push_back(edge->y1);
        }
    }
    std::ranges::sort(ends);
    ends.erase(std::ranges::unique(ends).begin(), ends.end());

    std::vector<across_t> across;
    std::vector<double> cuts;
    for (std::size_t part = 0; part + 1 < ends.size(); ++part) {
        const double low = ends[part];
        const double high = ends[part + 1];
        across.clear();
        for (const edge_t* edge : edges) {
            if (edge->y0 <= low && edge->y1 >= high) {
                across.push_back(
                    { .edge = edge, .top = edge->x_at(low), .bottom = edge->x_at(high) });
            }
        }
        std::ranges::sort(across, [](const across_t& a, const across_t& b) {
            return a.top != b.top ? a.top < b.top : a.bottom < b.bottom;
        });

        cuts.assign({ low, high });
        for (std::size_t i = 1; i < across.size(); ++i) {
            for (std::size_t j = i; j > 0 && across[j - 1].bottom > across[j].bottom; --j) {
                // The one on the left at the top is on the right at the bottom.
                const double above = across[j - 1].top - across[j].top;
                const double below = across[j - 1].bottom - across[j].bottom;
                if (above < 0) {
                    const double y = low + ((high - low) * above / (above - below));
                    if (y > low && y < high) {
                        cuts.push_back(y);
                    }
                }
                std::swap(across[j - 1], across[j]);
            }
        }
        std::ranges::sort(cuts);
        cuts.erase(std::ranges::unique(cuts).begin(), cuts.end());

        for (std::size_t band = 0; band + 1 < cuts.size(); ++band) {
            fill_band(across, cuts[band], cuts[band + 1], row);
        }
    }
    row.finish();
}

/// Without antialiasing, a row's pixels whose centers the edges' region holds, where a center on
/// an edge is in the region to its right, and one on its top end in it, but not on its bottom.
void center_row(const std::span<const edge_t* const> edges, const double top, row_t& row) {
    const double y = top + 0.5;
    std::vector<std::pair<double, int>> crossings;
    for (const edge_t* edge : edges) {
        if (edge->y0 <= y && y < edge->y1) {
            crossings.emplace_back(edge->x_at(y), edge->winding);
        }
    }
    std::ranges::sort(crossings, {}, &std::pair<double, int>::first);

    const auto width = static_cast<double>(row.cover.size());
    int winding = 0;
    double start = 0;
    for (const auto& [x, turn] : crossings) {
        const int was = winding;
        winding += turn;
        if (was == 0 && winding != 0) {
            start = x;
        } else if (was != 0 && winding == 0) {
            // The pixels from the first whose center is at or past start to the last before x.
            const double first = std::clamp(std::ceil(start - 0.5), 0.0, width);
            const double last = std::clamp(std::ceil(x - 0.5), 0.0, width);
            for (auto column = static_cast<std::size_t>(first);
                 column < static_cast<std::size_t>(last); ++column) {
                row.cover[column] = 1;
            }
        }
    }
}

/// Draws `rgba` over the pixels by the region the rings fill, by the nonzero rule.
void fill(const fb_array_t* pixels, const std::int64_t width, const std::int64_t height,
          const std::span<const ring_t> rings, const std::uint32_t rgba, const bool antialias) {
    std::vector<edge_t> edges = edges_of(rings);
    if (edges.empty() || width <= 0 || height <= 0) {
        return;
    }

    std::ranges::sort(edges, {}, &edge_t::y0);
    const std::array<std::uint8_t, 4> color{
        static_cast<std::uint8_t>(rgba >> 24U),
        static_cast<std::uint8_t>(rgba >> 16U),
        static_cast<std::uint8_t>(rgba >> 8U),
        static_cast<std::uint8_t>(rgba),
    };
    auto* bytes = static_cast<std::uint8_t*>(pixels->data);
    const auto columns = static_cast<std::size_t>(width);
    std::vector<const edge_t*> active;
    std::size_t next = 0;
    const double first_row = std::max(0.0, std::floor(edges.front().y0));
    for (auto y = static_cast<std::int64_t>(std::min(first_row, static_cast<double>(height)));
         y < height; ++y) {
        const auto top = static_cast<double>(y);
        const double bottom = top + 1;
        while (next < edges.size() && edges[next].y0 < bottom) {
            active.push_back(&edges[next]);
            ++next;
        }
        std::erase_if(active, [&](const edge_t* edge) { return edge->y1 <= top; });
        if (active.empty()) {
            if (next == edges.size()) {
                break;
            }
            continue;
        }

        row_t row(columns);
        if (antialias) {
            cover_row(active, top, row);
        } else {
            center_row(active, top, row);
        }

        std::uint8_t* pixel = bytes + (static_cast<std::size_t>(y) * columns * 4);
        for (std::size_t x = 0; x < columns; ++x, pixel += 4) {
            const double covered = row.cover[x];
            if (covered <= 0) {
                continue;
            }

            // Rounded to the nearest, a half up, and what's within a billionth below a half up
            // too, as an exact half can come out a hair below it.
            const auto alpha =
                static_cast<std::uint32_t>(std::floor((color[3] * covered) + 0.5 + 1e-9));
            if (alpha == 0) {
                continue;
            }

            const std::array<std::uint8_t, 4> drawn{
                color[0],
                color[1],
                color[2],
                static_cast<std::uint8_t>(alpha),
            };
            over(pixel, drawn.data());
        }
    }
}

/// A ring of `count` points from `coords`, x then y, from `at` on.
ring_t ring_of(const std::span<const double> coords, const std::size_t at,
               const std::size_t count) {
    ring_t ring;
    for (std::size_t i = 0; i < count; ++i) {
        ring.push_back({ .x = coords[(at + i) * 2], .y = coords[((at + i) * 2) + 1] });
    }
    return ring;
}

/// The rings in `coords`, `counts` of their points each.
std::vector<ring_t> rings_of(const fb_array_t* coords, const fb_array_t* counts) {
    const std::span<const double> values(static_cast<const double*>(coords->data),
                                         static_cast<std::size_t>(coords->count));
    const std::span<const std::int32_t> sizes(static_cast<const std::int32_t*>(counts->data),
                                              static_cast<std::size_t>(counts->count));
    std::vector<ring_t> rings;
    std::size_t at = 0;
    for (const std::int32_t size : sizes) {
        rings.push_back(ring_of(values, at, static_cast<std::size_t>(size)));
        at += static_cast<std::size_t>(size);
    }
    return rings;
}

/// A stroke's pieces: what a line of `points`, closed back to its first or not, covers `half`
/// a width either side, with its joins and caps.
class stroker_t {
public:
    stroker_t(const double half, const cap_t cap, const join_t join, const double miter_limit)
        : m_half(half), m_cap(cap), m_join(join), m_miter_limit(miter_limit) {}

    [[nodiscard]] std::vector<ring_t> stroke(ring_t points, const bool closed) {
        // Points the same as the one before go, and a closed line's last one the same as its
        // first.
        points.erase(
            std::ranges::unique(
                points, [](const point_t& a, const point_t& b) { return a.x == b.x && a.y == b.y; })
                .begin(),
            points.end());
        if (closed && points.size() > 1 && points.front().x == points.back().x &&
            points.front().y == points.back().y) {
            points.pop_back();
        }

        m_pieces.clear();
        if (points.empty() || m_half <= 0) {
            return m_pieces;
        }

        if (points.size() == 1) {
            dot(points[0]);
            return m_pieces;
        }

        const std::size_t count = points.size();
        const std::size_t segments = closed ? count : count - 1;
        for (std::size_t i = 0; i < segments; ++i) {
            segment(points[i], points[(i + 1) % count]);
        }

        for (std::size_t i = closed ? 0 : 1; i < (closed ? count : count - 1); ++i) {
            join(points[(i + count - 1) % count], points[i], points[(i + 1) % count]);
        }

        if (!closed) {
            cap(points[1], points[0]);
            cap(points[count - 2], points[count - 1]);
        }
        return m_pieces;
    }

private:
    double m_half;
    cap_t m_cap;
    join_t m_join;
    double m_miter_limit;
    std::vector<ring_t> m_pieces;

    /// The unit direction from a to b.
    static point_t direction(const point_t a, const point_t b) {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double length = std::sqrt((dx * dx) + (dy * dy));
        return { .x = dx / length, .y = dy / length };
    }

    void add(ring_t ring) {
        m_pieces.push_back(positive(std::move(ring)));
    }

    /// A rectangle along the segment, half the width either side.
    void segment(const point_t a, const point_t b) {
        const point_t d = direction(a, b);
        const point_t n{ .x = -d.y * m_half, .y = d.x * m_half };
        add({
            { .x = a.x + n.x, .y = a.y + n.y },
            { .x = b.x + n.x, .y = b.y + n.y },
            { .x = b.x - n.x, .y = b.y - n.y },
            { .x = a.x - n.x, .y = a.y - n.y },
        });
    }

    /// The join at `at`, between the segment from `before` and the one to `after`, on the
    /// outside of its turn.
    void join(const point_t before, const point_t at, const point_t after) {
        const point_t d0 = direction(before, at);
        const point_t d1 = direction(at, after);
        const double cross = (d0.x * d1.y) - (d0.y * d1.x);
        const double dot_product = (d0.x * d1.x) + (d0.y * d1.y);
        if (cross == 0 && dot_product > 0) {
            // Straight on: the rectangles meet.
            return;
        }

        if (m_join == JOIN_ROUND) {
            add(ellipse(at, m_half, m_half));
            return;
        }

        // The outside is to the left of both segments for a turn right, and to the right for a
        // turn left; a turn right back is either.
        const double side = cross > 0 ? -1 : 1;
        const point_t n0{ .x = -d0.y * side, .y = d0.x * side };
        const point_t n1{ .x = -d1.y * side, .y = d1.x * side };
        const point_t outer0{ .x = at.x + (n0.x * m_half), .y = at.y + (n0.y * m_half) };
        const point_t outer1{ .x = at.x + (n1.x * m_half), .y = at.y + (n1.y * m_half) };
        if (m_join == JOIN_MITER && cross != 0) {
            // The miter's tip is half the width over the cosine of half the turn from the
            // corner, and its length over the width that one over the cosine.
            const point_t sum{ .x = n0.x + n1.x, .y = n0.y + n1.y };
            const double length = std::sqrt((sum.x * sum.x) + (sum.y * sum.y));
            const point_t m{ .x = sum.x / length, .y = sum.y / length };
            const double cosine = (m.x * n0.x) + (m.y * n0.y);
            if (cosine * m_miter_limit >= 1) {
                const double reach = m_half / cosine;
                add({
                    at,
                    outer0,
                    { .x = at.x + (m.x * reach), .y = at.y + (m.y * reach) },
                    outer1,
                });
                return;
            }
        }

        add({ at, outer0, outer1 });
    }

    /// The cap at `end`, the end of the segment from `from`.
    void cap(const point_t from, const point_t end) {
        if (m_cap == CAP_ROUND) {
            add(ellipse(end, m_half, m_half));
        } else if (m_cap == CAP_SQUARE) {
            const point_t d = direction(from, end);
            const point_t n{ .x = -d.y * m_half, .y = d.x * m_half };
            const point_t past{ .x = end.x + (d.x * m_half), .y = end.y + (d.y * m_half) };
            add({
                { .x = end.x + n.x, .y = end.y + n.y },
                { .x = past.x + n.x, .y = past.y + n.y },
                { .x = past.x - n.x, .y = past.y - n.y },
                { .x = end.x - n.x, .y = end.y - n.y },
            });
        }
    }

    /// A line of one point: a round cap's dot, a square cap's square, or nothing.
    void dot(const point_t at) {
        if (m_cap == CAP_ROUND) {
            add(ellipse(at, m_half, m_half));
        } else if (m_cap == CAP_SQUARE) {
            add({
                { .x = at.x - m_half, .y = at.y - m_half },
                { .x = at.x + m_half, .y = at.y - m_half },
                { .x = at.x + m_half, .y = at.y + m_half },
                { .x = at.x - m_half, .y = at.y + m_half },
            });
        }
    }
};

} // namespace

extern "C" {

void fb_image_fill_rings(const fb_array_t* pixels, const std::int64_t width,
                         const std::int64_t height, const fb_array_t* coords,
                         const fb_array_t* counts, const std::uint32_t rgba,
                         const std::int32_t antialias) {
    fill(pixels, width, height, rings_of(coords, counts), rgba, antialias != 0);
}

void fb_image_fill_ellipse(const fb_array_t* pixels, const std::int64_t width,
                           const std::int64_t height, const double x, const double y,
                           const double rx, const double ry, const std::uint32_t rgba,
                           const std::int32_t antialias) {
    if (rx <= 0 || ry <= 0) {
        return;
    }

    const std::array<ring_t, 1> rings{ ellipse({ .x = x, .y = y }, rx, ry) };
    fill(pixels, width, height, rings, rgba, antialias != 0);
}

void fb_image_stroke(const fb_array_t* pixels, const std::int64_t width, const std::int64_t height,
                     const fb_array_t* coords, const fb_array_t* counts, const std::int32_t closed,
                     const double line_width, const std::int32_t cap, const std::int32_t join,
                     const double miter_limit, const std::uint32_t rgba,
                     const std::int32_t antialias) {
    stroker_t stroker(line_width / 2, static_cast<cap_t>(cap), static_cast<join_t>(join),
                      miter_limit);
    std::vector<ring_t> pieces;
    for (ring_t& line : rings_of(coords, counts)) {
        std::vector<ring_t> stroked = stroker.stroke(std::move(line), closed != 0);
        pieces.insert(pieces.end(), std::make_move_iterator(stroked.begin()),
                      std::make_move_iterator(stroked.end()));
    }
    fill(pixels, width, height, pieces, rgba, antialias != 0);
}

void fb_image_stroke_ellipse(const fb_array_t* pixels, const std::int64_t width,
                             const std::int64_t height, const double x, const double y,
                             const double rx, const double ry, const double line_width,
                             const std::uint32_t rgba, const std::int32_t antialias) {
    if (rx <= 0 || ry <= 0) {
        return;
    }

    // Its polygon turns a little at each corner, where a miter is the curve's offset within the
    // tolerance.
    stroker_t stroker(line_width / 2, CAP_BUTT, JOIN_MITER, 4);
    const std::vector<ring_t> pieces = stroker.stroke(ellipse({ .x = x, .y = y }, rx, ry), true);
    fill(pixels, width, height, pieces, rgba, antialias != 0);
}
}
