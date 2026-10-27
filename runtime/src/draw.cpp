// std.image's drawing: regions of straight edges filled by the nonzero rule, each pixel by the
// exact part of its square the region covers, or, without antialiasing, by whether its center is
// in it; ellipses as polygons within a hundredth of a pixel of them; and strokes as the region
// their pieces cover together, a rectangle along each segment, a join at each corner, and a cap
// at each end. It computes in doubles, without fused multiply-adds, with sines of its own, so
// every target gives the same pixels.
//
// A row of pixels is swept across, a pixel at a time, with the winding of what's wholly to the
// pixel's left kept as it changes down the row. In the pixel, its edges' pieces and those changes
// cut it into bands where none starts, ends, or crosses another, so in each band they go
// straight across in an order of their own; going across a band, the winding says which
// stretches between them are filled, trapezoids that don't overlap, whose areas add up to the
// exact part of the pixel the region covers. So a row costs what its pixels' pieces do, not what
// all its edges do together. A pixel that more than a few dozen pieces pass through, which only
// a font made to be slow has, is measured along 16 lines across it instead.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "font_internal.hpp"
#include "image_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <numbers>
#include <span>
#include <string_view>
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

/// The winding down a row of what's wholly to a pixel's left: how it changes at each height, the
/// row's top's for what it is there. A change and its opposite at one height, as two edges of a
/// ring that meet there give, are none.
class turns_t {
public:
    void add(const double y, const int turn) {
        auto [at, added] = m_turns.try_emplace(y, turn);
        if (!added) {
            at->second += turn;
            if (at->second == 0) {
                m_turns.erase(at);
            }
        }
    }

    [[nodiscard]] const std::map<double, int>& all() const {
        return m_turns;
    }

    /// The winding at `y`.
    [[nodiscard]] int at(const double y) const {
        int winding = 0;
        for (auto it = m_turns.begin(); it != m_turns.end() && it->first <= y; ++it) {
            winding += it->second;
        }
        return winding;
    }

private:
    std::map<double, int> m_turns;
};

/// More pieces than a pixel's exact coverage is worked out for: past them, it's measured along
/// lines across it, as a pixel's pieces can each cross every other.
constexpr std::size_t most_exact_pieces = 32;
/// The lines across a pixel of too many pieces it's measured along.
constexpr int sampled_lines = 16;

/// The part of the pixel from `left` to left + 1 across and `top` to `bottom` down that the
/// region covers, where the winding of `turns`, from what's wholly to its left, and its
/// `pieces` isn't 0: cut where a piece starts or ends or the turns change, then where two
/// pieces cross, which in a part nothing starts or ends in are the pairs whose order at its
/// bottom isn't their order at its top, each a swap of an insertion sort from the one order to
/// the other.
double exactly_covered(const std::span<const edge_t> pieces, const turns_t& turns,
                       const double left, const double top, const double bottom) {
    std::vector<double> ends{ top, bottom };
    for (const auto& [y, turn] : turns.all()) {
        if (y > top && y < bottom) {
            ends.push_back(y);
        }
    }
    for (const edge_t& piece : pieces) {
        for (const double y : { piece.y0, piece.y1 }) {
            if (y > top && y < bottom) {
                ends.push_back(y);
            }
        }
    }
    std::ranges::sort(ends);
    ends.erase(std::ranges::unique(ends).begin(), ends.end());

    double area = 0;
    std::vector<across_t> across;
    std::vector<double> cuts;
    for (std::size_t part = 0; part + 1 < ends.size(); ++part) {
        const double low = ends[part];
        const double high = ends[part + 1];
        const int base = turns.at(low);
        across.clear();
        for (const edge_t& piece : pieces) {
            if (piece.y0 <= low && piece.y1 >= high) {
                across.push_back(
                    { .edge = &piece, .top = piece.x_at(low), .bottom = piece.x_at(high) });
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

        // Each band's stretches the winding fills, from the pixel's left side to its right.
        for (std::size_t band = 0; band + 1 < cuts.size(); ++band) {
            const double a = cuts[band];
            const double b = cuts[band + 1];
            for (across_t& edge : across) {
                edge.top = edge.edge->x_at(a);
                edge.bottom = edge.edge->x_at(b);
            }
            sort_by_middle(across);

            int winding = base;
            double from_top = left;
            double from_bottom = left;
            for (const across_t& edge : across) {
                const int was = winding;
                winding += edge.edge->winding;
                if (was == 0 && winding != 0) {
                    from_top = edge.top;
                    from_bottom = edge.bottom;
                } else if (was != 0 && winding == 0) {
                    area += (b - a) * ((edge.top + edge.bottom) - (from_top + from_bottom)) / 2;
                }
            }
            if (winding != 0) {
                area += (b - a) * ((2 * (left + 1)) - (from_top + from_bottom)) / 2;
            }
        }
    }
    return std::clamp(area, 0.0, 1.0);
}

/// The same, measured along lines across the pixel, at the middles of `sampled_lines` equal
/// parts of it: what's filled of each, exactly, averaged.
double sampled_covered(const std::span<const edge_t> pieces, const turns_t& turns,
                       const double left, const double top, const double bottom) {
    double covered = 0;
    std::vector<std::pair<double, int>> crossings;
    for (int line = 0; line < sampled_lines; ++line) {
        const double y = top + ((bottom - top) * (line + 0.5) / sampled_lines);
        crossings.clear();
        for (const edge_t& piece : pieces) {
            if (piece.y0 <= y && y < piece.y1) {
                crossings.emplace_back(piece.x_at(y), piece.winding);
            }
        }
        std::ranges::sort(crossings, {}, &std::pair<double, int>::first);

        int winding = turns.at(y);
        double from = left;
        for (const auto& [x, turn] : crossings) {
            const int was = winding;
            winding += turn;
            if (was == 0 && winding != 0) {
                from = x;
            } else if (was != 0 && winding == 0) {
                covered += x - from;
            }
        }
        if (winding != 0) {
            covered += left + 1 - from;
        }
    }
    return std::clamp(covered * (bottom - top) / sampled_lines, 0.0, 1.0);
}

/// The part of a row's pixel, from `top` to `bottom`, the winding of `turns` alone fills: where
/// it isn't 0.
double turns_covered(const turns_t& turns, const double top, const double bottom) {
    double covered = 0;
    int winding = 0;
    double from = top;
    for (const auto& [y, turn] : turns.all()) {
        if (y >= bottom) {
            break;
        }
        if (winding != 0) {
            covered += y - from;
        }
        winding += turn;
        from = y;
    }
    if (winding != 0) {
        covered += bottom - from;
    }
    return covered;
}

/// The row's pieces of an edge from (xa, ya) down to (xb, yb), its part in the row, each in a
/// pixel's column, which it gives with the column; its part left of the row's first pixel is in
/// `turns`, and its part right of its last is left out.
void add_pieces(const double xa, const double ya, const double xb, const double yb,
                const int winding, const double width, turns_t& turns,
                std::vector<std::pair<std::size_t, edge_t>>& pieces) {
    const auto give = [&](const double x0, const double y0, const double x1, const double y1) {
        if (!(y0 < y1)) {
            return;
        }
        const double low = std::min(x0, x1);
        if (std::max(x0, x1) <= 0) {
            turns.add(y0, winding);
            turns.add(y1, -winding);
        } else if (low < width) {
            pieces.emplace_back(
                static_cast<std::size_t>(std::floor(low)),
                edge_t{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .winding = winding });
        }
    };

    const double low = std::min(xa, xb);
    const double high = std::max(xa, xb);
    if (low == high) {
        give(xa, ya, xb, yb);
        return;
    }

    // Cut where it crosses each side of a pixel's column, from 0 to the width, the way it goes.
    const auto first = static_cast<std::int64_t>(std::max(std::floor(low) + 1, 0.0));
    const auto last = static_cast<std::int64_t>(std::min(std::ceil(high) - 1, width));
    double x = xa;
    double y = ya;
    const auto cut_at = [&](const std::int64_t column) {
        const auto side = static_cast<double>(column);
        const double at = std::clamp(ya + ((side - xa) * (yb - ya) / (xb - xa)), ya, yb);
        give(x, y, side, at);
        x = side;
        y = at;
    };
    if (xa < xb) {
        for (std::int64_t column = first; column <= last; ++column) {
            cut_at(column);
        }
    } else {
        for (std::int64_t column = last; column >= first; --column) {
            cut_at(column);
        }
    }
    give(x, y, xb, yb);
}

/// Each pixel's coverage in a row of `cover.size()` from `top` to top + 1, by the edges that
/// reach it, swept across: a pixel's pieces and the turns of what's left of it give its exact
/// coverage, and a stretch of pixels with no pieces is what the turns alone give.
void cover_row(const std::span<const edge_t* const> edges, const double top,
               std::vector<double>& cover) {
    const double bottom = top + 1;
    const auto width = static_cast<double>(cover.size());
    turns_t turns;
    std::vector<std::pair<std::size_t, edge_t>> pieces;
    for (const edge_t* edge : edges) {
        const double ya = std::max(edge->y0, top);
        const double yb = std::min(edge->y1, bottom);
        if (ya < yb) {
            add_pieces(edge->x_at(ya), ya, edge->x_at(yb), yb, edge->winding, width, turns, pieces);
        }
    }
    std::ranges::stable_sort(pieces, {}, &std::pair<std::size_t, edge_t>::first);

    std::vector<edge_t> here;
    std::size_t next = 0;
    std::size_t column = 0;
    while (column < cover.size()) {
        if (next == pieces.size() || pieces[next].first > column) {
            // Up to the next pixel with pieces, the turns alone, which don't change.
            const std::size_t until = next == pieces.size() ? cover.size() : pieces[next].first;
            const double covered = std::clamp(turns_covered(turns, top, bottom), 0.0, 1.0);
            std::fill(cover.begin() + static_cast<std::ptrdiff_t>(column),
                      cover.begin() + static_cast<std::ptrdiff_t>(until), covered);
            column = until;
            continue;
        }

        here.clear();
        while (next < pieces.size() && pieces[next].first == column) {
            here.push_back(pieces[next].second);
            ++next;
        }
        const auto left = static_cast<double>(column);
        cover[column] = here.size() > most_exact_pieces
                            ? sampled_covered(here, turns, left, top, bottom)
                            : exactly_covered(here, turns, left, top, bottom);
        for (const edge_t& piece : here) {
            turns.add(piece.y0, piece.winding);
            turns.add(piece.y1, -piece.winding);
        }
        ++column;
    }
}

/// Without antialiasing, a row's pixels whose centers the edges' region holds, where a center on
/// an edge is in the region to its right, and one on its top end in it, but not on its bottom.
void center_row(const std::span<const edge_t* const> edges, const double top,
                std::vector<double>& cover) {
    const double y = top + 0.5;
    std::vector<std::pair<double, int>> crossings;
    for (const edge_t* edge : edges) {
        if (edge->y0 <= y && y < edge->y1) {
            crossings.emplace_back(edge->x_at(y), edge->winding);
        }
    }
    std::ranges::sort(crossings, {}, &std::pair<double, int>::first);

    const auto width = static_cast<double>(cover.size());
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
                cover[column] = 1;
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

        std::vector<double> cover(columns, 0);
        if (antialias) {
            cover_row(active, top, cover);
        } else {
            center_row(active, top, cover);
        }

        std::uint8_t* pixel = bytes + (static_cast<std::size_t>(y) * columns * 4);
        for (std::size_t x = 0; x < columns; ++x, pixel += 4) {
            const double covered = cover[x];
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

/// The most chords a quadratic or a cubic of a glyph's is cut into.
constexpr int most_chords = 1024;
/// The most points a glyph's rings have, its curves cut into chords: a glyph of more, which only
/// a font made to be slow to draw has, isn't drawn.
constexpr std::size_t most_glyph_points = std::size_t{ 1 } << 16U;

/// The image's pixels and one more around them. A curve whose control points are all on one side
/// of it, and so the curve too, is one chord, which covers nothing of the image the curve doesn't.
struct box_t {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;

    [[nodiscard]] bool misses(const std::initializer_list<point_t> points) const {
        const auto all = [&](const auto& outside) { return std::ranges::all_of(points, outside); };
        return all([&](const point_t point) { return point.x < left; }) ||
               all([&](const point_t point) { return point.x > right; }) ||
               all([&](const point_t point) { return point.y < top; }) ||
               all([&](const point_t point) { return point.y > bottom; });
    }
};

/// Adds the chords from p0 to p1 of the quadratic through c, but p0's point: as many as keep
/// them within the tolerance, which its second difference over 4 times their count squared is.
void add_quadratic(const point_t p0, const point_t c, const point_t p1, ring_t& ring) {
    const double dx = p0.x - (2 * c.x) + p1.x;
    const double dy = p0.y - (2 * c.y) + p1.y;
    const double bend = std::sqrt((dx * dx) + (dy * dy));
    const int count =
        std::clamp(static_cast<int>(std::ceil(std::sqrt(bend / (4 * tolerance)))), 1, most_chords);
    for (int i = 1; i < count; ++i) {
        const double t = static_cast<double>(i) / count;
        const double u = 1 - t;
        ring.push_back({
            .x = (u * u * p0.x) + (2 * u * t * c.x) + (t * t * p1.x),
            .y = (u * u * p0.y) + (2 * u * t * c.y) + (t * t * p1.y),
        });
    }
    ring.push_back(p1);
}

/// Adds the chords from p0 to p3 of the cubic through p1 and p2, but p0's point: as many as keep
/// them within the tolerance, three quarters of the larger of its two second differences over
/// their count squared.
void add_cubic(const point_t p0, const point_t p1, const point_t p2, const point_t p3,
               ring_t& ring) {
    const double ax = p0.x - (2 * p1.x) + p2.x;
    const double ay = p0.y - (2 * p1.y) + p2.y;
    const double bx = p1.x - (2 * p2.x) + p3.x;
    const double by = p1.y - (2 * p2.y) + p3.y;
    const double bend =
        std::max(std::sqrt((ax * ax) + (ay * ay)), std::sqrt((bx * bx) + (by * by)));
    const int count =
        std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75 * bend / tolerance))), 1, most_chords);
    for (int i = 1; i < count; ++i) {
        const double t = static_cast<double>(i) / count;
        const double u = 1 - t;
        ring.push_back({
            .x = (u * u * u * p0.x) + (3 * u * u * t * p1.x) + (3 * u * t * t * p2.x) +
                 (t * t * t * p3.x),
            .y = (u * u * u * p0.y) + (3 * u * u * t * p1.y) + (3 * u * t * t * p2.y) +
                 (t * t * t * p3.y),
        });
    }
    ring.push_back(p3);
}

/// A glyph's outline as rings, `place` putting each of its points in the image: a ring from each
/// start, its lines, and its curves as chords, but one for a curve `box` misses, without its last
/// point when that's back at its first, as each ring closes itself. None when they'd have more
/// than most_glyph_points.
template <typename Place>
void add_glyph_rings(const fb::image::font_path_t& path, const Place& place, const box_t& box,
                     std::vector<ring_t>& rings) {
    using fb::image::font_segment_t;
    std::vector<ring_t> glyph;
    const auto close = [&] {
        if (!glyph.empty() && glyph.back().size() > 1 &&
            glyph.back().front().x == glyph.back().back().x &&
            glyph.back().front().y == glyph.back().back().y) {
            glyph.back().pop_back();
        }
    };

    point_t current{};
    bool open = false;
    std::size_t points = 0;
    for (const font_segment_t& segment : path) {
        const point_t end = place(segment.x, segment.y);
        if (segment.kind == font_segment_t::MOVE) {
            if (open) {
                close();
            }
            glyph.push_back({ end });
            open = true;
        } else if (!open) {
            continue;
        } else if (segment.kind == font_segment_t::LINE) {
            glyph.back().push_back(end);
        } else if (segment.kind == font_segment_t::QUADRATIC) {
            const point_t control = place(segment.x1, segment.y1);
            if (box.misses({ current, control, end })) {
                glyph.back().push_back(end);
            } else {
                const std::size_t before = glyph.back().size();
                add_quadratic(current, control, end, glyph.back());
                points += glyph.back().size() - before - 1;
            }
        } else {
            const point_t first = place(segment.x1, segment.y1);
            const point_t second = place(segment.x2, segment.y2);
            if (box.misses({ current, first, second, end })) {
                glyph.back().push_back(end);
            } else {
                const std::size_t before = glyph.back().size();
                add_cubic(current, first, second, end, glyph.back());
                points += glyph.back().size() - before - 1;
            }
        }
        current = end;
        if (++points > most_glyph_points) {
            return;
        }
    }
    if (open) {
        close();
    }
    rings.insert(rings.end(), std::make_move_iterator(glyph.begin()),
                 std::make_move_iterator(glyph.end()));
}

/// The next code point of the text's UTF-8, which a String's always is, from `at`, which it
/// moves past it.
std::uint32_t next_code_point(const std::string_view text, std::size_t& at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = 1;
    std::uint32_t point = lead;
    if (lead >= 0xF0) {
        length = 4;
        point = lead & 0x07U;
    } else if (lead >= 0xE0) {
        length = 3;
        point = lead & 0x0FU;
    } else if (lead >= 0xC0) {
        length = 2;
        point = lead & 0x1FU;
    }
    for (std::size_t i = 1; i < length && at + i < text.size(); ++i) {
        point = (point << 6U) | (static_cast<unsigned char>(text[at + i]) & 0x3FU);
    }
    at += length;
    return point;
}

/// Lays the text out in the font at `size` pixels an em, its first line's baseline from (x,
/// y): each glyph after the last's advance and the kerning between them, unrounded, and each
/// line feed a line's height down at x again. Gives each glyph's place to `glyph`, and the
/// widest line's width.
template <typename Glyph>
double lay_out(const fb::image::font_t& font, const std::string_view text, const double size,
               const double x, const double y, const Glyph& glyph) {
    const double scale = size / font.units_per_em();
    const double line_height = (font.ascent() - font.descent() + font.line_gap()) * scale;
    double pen_x = x;
    double pen_y = y;
    double widest = 0;
    bool first = true;
    std::uint32_t before = 0;
    for (std::size_t at = 0; at < text.size();) {
        const std::uint32_t point = next_code_point(text, at);
        if (point == '\n') {
            widest = std::max(widest, pen_x - x);
            pen_x = x;
            pen_y += line_height;
            first = true;
            continue;
        }

        const std::uint32_t index = font.glyph_of(point);
        if (!first) {
            pen_x += font.kerning(before, index) * scale;
        }
        glyph(index, pen_x, pen_y, scale);
        pen_x += font.advance_of(index) * scale;
        before = index;
        first = false;
    }
    return std::max(widest, pen_x - x);
}

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

void fb_image_fill_text(const fb_array_t* pixels, const std::int64_t width,
                        const std::int64_t height, const void* font, const fb_string_t* text,
                        const double size, const double x, const double y, const std::uint32_t rgba,
                        const std::int32_t antialias) {
    const auto& face = *static_cast<const fb::image::font_t*>(font);
    const box_t box{
        .left = -1,
        .top = -1,
        .right = static_cast<double>(width) + 1,
        .bottom = static_cast<double>(height) + 1,
    };
    std::vector<ring_t> rings;
    lay_out(
        face, text_of(text), size, x, y,
        [&](const std::uint32_t glyph, const double at_x, const double at_y, const double scale) {
            // A point of the glyph's, in font units, y up, in the image.
            const auto place = [&](const double unit_x, const double unit_y) {
                return point_t{ .x = at_x + (unit_x * scale), .y = at_y - (unit_y * scale) };
            };
            add_glyph_rings(face.outline(glyph), place, box, rings);
        });
    fill(pixels, width, height, rings, rgba, antialias != 0);
}

double fb_font_width(const void* font, const fb_string_t* text, const double size) {
    const auto& face = *static_cast<const fb::image::font_t*>(font);
    return lay_out(face, text_of(text), size, 0, 0,
                   [](std::uint32_t /*glyph*/, double /*x*/, double /*y*/, double /*scale*/) {});
}
}
