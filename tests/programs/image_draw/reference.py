"""What image_draw's and image_text's make_data.py share: drawings' regions as the same polygons,
made by the same arithmetic in doubles as runtime/src/draw.cpp makes them (its own sines, its
ellipses' sides, its strokes' pieces), each pixel's coverage of a region exactly, in Fractions,
by the nonzero rule, and a color drawn over pixels by the same integer "over" as the runtime's.
It needs nothing but Python 3.
"""

import math
import struct
from fractions import Fraction


def f32(value):
    """The value as a Float, a Vec2's, has it."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


# Ellipses and strokes, as draw.cpp makes them.

TOLERANCE = 0.01
MOST_SIDES = 1024
HALF_PI = math.pi / 2


def sine(angle):
    squared = angle * angle
    total = 0.0
    for power in range(19, 1, -2):
        total = (1 - total) * squared / (power * (power - 1))
    return angle * (1 - total)


def quarter_sides(radius):
    sides = 2
    while sides < MOST_SIDES:
        half = HALF_PI / (2.0 * sides)
        if radius * (1 - sine(HALF_PI - half)) <= TOLERANCE:
            break
        sides += 1
    return sides


def ellipse(cx, cy, rx, ry):
    sides = quarter_sides(max(rx, ry))
    sines = [sine(HALF_PI * k / sides) for k in range(sides + 1)]
    ring = []
    for quarter in range(4):
        for k in range(sides):
            turned = (sines[sides - k], sines[k])
            for _ in range(quarter):
                turned = (-turned[1], turned[0])
            ring.append((cx + rx * turned[0], cy + ry * turned[1]))
    return ring


def twice_area(ring):
    total = 0.0
    for i, a in enumerate(ring):
        b = ring[(i + 1) % len(ring)]
        total += a[0] * b[1] - b[0] * a[1]
    return total


def positive(ring):
    return list(reversed(ring)) if twice_area(ring) < 0 else ring


def direction(a, b):
    dx = b[0] - a[0]
    dy = b[1] - a[1]
    length = math.sqrt(dx * dx + dy * dy)
    return (dx / length, dy / length)


def stroke(points, closed, width, cap, join, miter_limit):
    """A stroke's pieces, as draw.cpp's stroker_t makes them."""
    half = width / 2
    kept = []
    for p in points:
        if not kept or kept[-1] != p:
            kept.append(p)
    if closed and len(kept) > 1 and kept[0] == kept[-1]:
        kept.pop()
    pieces = []
    if not kept or half <= 0:
        return pieces

    def add(ring):
        pieces.append(positive(ring))

    def circle(at):
        add(ellipse(at[0], at[1], half, half))

    if len(kept) == 1:
        at = kept[0]
        if cap == "ROUND":
            circle(at)
        elif cap == "SQUARE":
            add([(at[0] - half, at[1] - half), (at[0] + half, at[1] - half),
                 (at[0] + half, at[1] + half), (at[0] - half, at[1] + half)])
        return pieces

    count = len(kept)
    for i in range(count if closed else count - 1):
        a = kept[i]
        b = kept[(i + 1) % count]
        d = direction(a, b)
        n = (-d[1] * half, d[0] * half)
        add([(a[0] + n[0], a[1] + n[1]), (b[0] + n[0], b[1] + n[1]),
             (b[0] - n[0], b[1] - n[1]), (a[0] - n[0], a[1] - n[1])])

    for i in range(0 if closed else 1, count if closed else count - 1):
        before = kept[(i + count - 1) % count]
        at = kept[i]
        after = kept[(i + 1) % count]
        d0 = direction(before, at)
        d1 = direction(at, after)
        cross = d0[0] * d1[1] - d0[1] * d1[0]
        dot = d0[0] * d1[0] + d0[1] * d1[1]
        if cross == 0 and dot > 0:
            continue
        if join == "ROUND":
            circle(at)
            continue
        side = -1 if cross > 0 else 1
        n0 = (-d0[1] * side, d0[0] * side)
        n1 = (-d1[1] * side, d1[0] * side)
        outer0 = (at[0] + n0[0] * half, at[1] + n0[1] * half)
        outer1 = (at[0] + n1[0] * half, at[1] + n1[1] * half)
        if join == "MITER" and cross != 0:
            total = (n0[0] + n1[0], n0[1] + n1[1])
            length = math.sqrt(total[0] * total[0] + total[1] * total[1])
            m = (total[0] / length, total[1] / length)
            cosine = m[0] * n0[0] + m[1] * n0[1]
            if cosine * miter_limit >= 1:
                reach = half / cosine
                add([at, outer0, (at[0] + m[0] * reach, at[1] + m[1] * reach), outer1])
                continue
        add([at, outer0, outer1])

    if not closed:
        for start, end in ((kept[1], kept[0]), (kept[count - 2], kept[count - 1])):
            if cap == "ROUND":
                circle(end)
            elif cap == "SQUARE":
                d = direction(start, end)
                n = (-d[1] * half, d[0] * half)
                past = (end[0] + d[0] * half, end[1] + d[1] * half)
                add([(end[0] + n[0], end[1] + n[1]), (past[0] + n[0], past[1] + n[1]),
                     (past[0] - n[0], past[1] - n[1]), (end[0] - n[0], end[1] - n[1])])
    return pieces


# Coverage, exactly.


def edges_of(rings):
    edges = []
    for ring in rings:
        exact = [(Fraction(x), Fraction(y)) for x, y in ring]
        for i, a in enumerate(exact):
            b = exact[(i + 1) % len(exact)]
            if a[1] < b[1]:
                edges.append((a, b, 1))
            elif a[1] > b[1]:
                edges.append((b, a, -1))
    return edges


def x_at(edge, y):
    (x0, y0), (x1, y1), _ = edge
    return x0 + (y - y0) * (x1 - x0) / (y1 - y0)


def clipped_area(polygon, left, right):
    """The area of the convex polygon between x = left and x = right."""
    def clip(points, keep, cross):
        out = []
        for i, p in enumerate(points):
            q = points[(i + 1) % len(points)]
            if keep(p):
                out.append(p)
            if keep(p) != keep(q):
                out.append(cross(p, q))
        return out

    def at_x(p, q, x):
        return (x, p[1] + (q[1] - p[1]) * (x - p[0]) / (q[0] - p[0]))

    points = clip(polygon, lambda p: p[0] >= left, lambda p, q: at_x(p, q, left))
    if points:
        points = clip(points, lambda p: p[0] <= right, lambda p, q: at_x(p, q, right))
    total = Fraction(0)
    for i, p in enumerate(points):
        q = points[(i + 1) % len(points)]
        total += p[0] * q[1] - q[0] * p[1]
    return abs(total) / 2


def coverage(rings, antialias, width, height):
    """Each pixel's coverage of the rings' region by the nonzero rule, as a Fraction, or, without
    antialiasing, 1 where the region holds the pixel's center."""
    edges = edges_of(rings)
    cover = [[Fraction(0)] * width for _ in range(height)]
    for row in range(height):
        top, bottom = Fraction(row), Fraction(row + 1)
        reaching = [e for e in edges if e[0][1] < bottom and e[1][1] > top]
        if not antialias:
            y = top + Fraction(1, 2)
            crossings = sorted((x_at(e, y), e[2]) for e in reaching if e[0][1] <= y < e[1][1])
            winding = 0
            start = None
            for x, turn in crossings:
                was = winding
                winding += turn
                if was == 0 and winding != 0:
                    start = x
                elif was != 0 and winding == 0:
                    for column in range(width):
                        center = column + Fraction(1, 2)
                        if start <= center < x:
                            cover[row][column] = Fraction(1)
            continue

        ys = {top, bottom}
        for (x0, y0), (x1, y1), _ in reaching:
            for y in (y0, y1):
                if top < y < bottom:
                    ys.add(y)
        for i, a in enumerate(reaching):
            for b in reaching[i + 1:]:
                low = max(top, a[0][1], b[0][1])
                high = min(bottom, a[1][1], b[1][1])
                if high <= low:
                    continue
                above = x_at(a, low) - x_at(b, low)
                below = x_at(a, high) - x_at(b, high)
                if above * below < 0:
                    ys.add(low + (high - low) * above / (above - below))
        ys = sorted(ys)
        for low, high in zip(ys, ys[1:]):
            middle = (low + high) / 2
            across = sorted((x_at(e, middle), x_at(e, low), x_at(e, high), e[2])
                            for e in reaching if e[0][1] <= low and e[1][1] >= high)
            winding = 0
            left = None
            for edge in across:
                was = winding
                winding += edge[3]
                if was == 0 and winding != 0:
                    left = edge
                elif was != 0 and winding == 0:
                    trapezoid = [(left[1], low), (edge[1], low), (edge[2], high), (left[2], high)]
                    xs = [p[0] for p in trapezoid]
                    first = max(0, math.floor(min(xs)))
                    last = min(width, math.ceil(max(xs)))
                    for column in range(first, last):
                        cover[row][column] += clipped_area(trapezoid, Fraction(column),
                                                           Fraction(column + 1))
    return cover


def draw(pixels, width, height, rings, color, antialias=True):
    """Draws the color over the pixels, RGBA bytes, by the rings' coverage, as draw.cpp does."""
    cover = coverage(rings, antialias, width, height)
    for y in range(height):
        for x in range(width):
            covered = min(cover[y][x], 1)
            if covered <= 0:
                continue
            # Rounded to the nearest, a half up, and what's within a billionth below a half up
            # too, as draw.cpp rounds it.
            alpha = math.floor(color[3] * covered + Fraction(1, 2) + Fraction(1, 10**9))
            if alpha == 0:
                continue
            over(pixels, (y * width + x) * 4, (color[0], color[1], color[2], alpha))


def over(pixels, at, color):
    """image_internal.hpp's over, in 255ths."""
    alpha = color[3]
    if alpha == 255:
        pixels[at:at + 4] = list(color)
        return
    under = pixels[at + 3] * (255 - alpha)
    total = alpha * 255 + under
    for c in range(3):
        mixed = color[c] * alpha * 255 + pixels[at + c] * under
        pixels[at + c] = (mixed + total // 2) // total
    pixels[at + 3] = (total + 127) // 255
