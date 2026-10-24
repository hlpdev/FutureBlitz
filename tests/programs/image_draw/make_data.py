"""Writes data.fb, the pixels image_draw.fb's drawings should have, worked out apart from the
runtime: each drawing's region as the same polygons, made by the same arithmetic in doubles as
runtime/src/draw.cpp makes them (its own sines, its ellipses' sides, its strokes' pieces), then
each pixel's coverage of it exactly, in Fractions, by the nonzero rule, and its color drawn over
by the same integer "over" as the runtime's.

    python make_data.py

It needs nothing but Python 3.
"""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from reference import draw, ellipse, f32, stroke  # noqa: E402

WIDTH = 24
HEIGHT = 16

# The drawings, as image_draw.fb draws them.

BLACK = (0, 0, 0, 255)


def paint(pixels, rings, color, antialias=True):
    draw(pixels, WIDTH, HEIGHT, rings, color, antialias)


def rect(x, y, width, height):
    return [(x, y), (x + width, y), (x + width, y + height), (x, y + height)]


# A pentagram, its points joined every other, so it crosses itself, and its middle winds twice.
STAR = [(12.0, 1.5), (15.75, 13.0), (5.875, 5.875), (18.125, 5.875), (8.25, 13.0)]

CASES = {
    "AlignedRect": lambda p: paint(p, [rect(2, 3, 10, 5)], BLACK),
    "FractionRect": lambda p: paint(p, [rect(1.25, 2.5, 13.375, 7.75)], BLACK),
    "Triangle": lambda p: paint(p, [[(2.5, 1.25), (21.75, 6.5), (7.125, 14.875)]], BLACK),
    "Star": lambda p: paint(p, [STAR], BLACK),
    "StarAliased": lambda p: paint(p, [STAR], BLACK, antialias=False),
    "Hole": lambda p: paint(p, [rect(2, 2, 20, 12), list(reversed(rect(6.5, 5.25, 9, 5)))], BLACK),
    "Overlapping": lambda p: paint(p, [rect(2, 2, 12, 8), rect(8.5, 5.5, 13, 9.25)], BLACK),
    "Circle": lambda p: paint(p, [ellipse(f32(11.7), f32(7.2), f32(5.3), f32(5.3))], BLACK),
    "CircleAliased": lambda p: paint(p, [ellipse(f32(11.7), f32(7.2), f32(5.3), f32(5.3))], BLACK,
                                    antialias=False),
    "Ellipse": lambda p: paint(p, [ellipse(12, 8, 10.5, 4.25)], BLACK),
    "LineButt": lambda p: paint(p, stroke([(3, 4), (20, 11)], False, 2.5, "BUTT", "MITER", 4),
                                BLACK),
    "LineRound": lambda p: paint(p, stroke([(3, 4), (20, 11)], False, 2.5, "ROUND", "MITER", 4),
                                BLACK),
    "LineSquare": lambda p: paint(p, stroke([(3, 4), (20, 11)], False, 2.5, "SQUARE", "MITER", 4),
                                 BLACK),
    "Miter": lambda p: paint(p, stroke([(2, 13), (8, 3), (14, 13), (20, 4)], False, 2, "BUTT",
                                      "MITER", 4), BLACK),
    "MiterLimited": lambda p: paint(p, stroke([(2, 13), (12, 2), (22, 13)], False, 3, "BUTT",
                                             "MITER", 1.5), BLACK),
    "RoundJoin": lambda p: paint(p, stroke([(2, 13), (8, 3), (14, 13), (20, 4)], False, 2, "BUTT",
                                          "ROUND", 4), BLACK),
    "BevelJoin": lambda p: paint(p, stroke([(2, 13), (8, 3), (14, 13), (20, 4)], False, 2, "BUTT",
                                          "BEVEL", 4), BLACK),
    "ClosedPolygon": lambda p: paint(p, stroke([(3, 2), (21, 4), (17, 14), (5, 11)], True, 1.5,
                                              "BUTT", "MITER", 4), BLACK),
    "StrokeRect": lambda p: paint(p, stroke(rect(3.5, 2.5, 16, 10), True, 1, "BUTT", "MITER", 4),
                                 BLACK),
    "StrokeCircle": lambda p: paint(p, stroke(ellipse(12, 8, 6, 6), True, 1.5, "BUTT", "MITER", 4),
                                   BLACK),
    "StrokeEllipse": lambda p: paint(p, stroke(ellipse(12, 8, 10, 5), True, 2, "BUTT", "MITER", 4),
                                    BLACK),
    "Dot": lambda p: paint(p, stroke([(12.5, 8.5)], False, 5, "ROUND", "MITER", 4), BLACK),
    "Translucent": lambda p: (paint(p, [rect(2, 2, 14, 9)], (255, 0, 0, 255)),
                              paint(p, [ellipse(14, 9, 7, 5)], (0, 0, 255, 128)),
                              paint(p, stroke([(1, 14), (23, 1)], False, 3, "ROUND", "MITER", 4),
                                   (0, 200, 0, 90))),
}


def main():
    lines = ["// What image_draw.fb's drawings give, from make_data.py, which works them out apart",
             "// from the runtime. Each is an image's RGBA bytes, " + f"{WIDTH} by {HEIGHT}.", ""]
    for name, case in CASES.items():
        pixels = [0] * (WIDTH * HEIGHT * 4)
        case(pixels)
        lines.append(f"Public Function {name}(): UInt8[]")
        values = [str(v) for v in pixels]
        line = "    Return ["
        chunks = []
        for value in values:
            piece = value + ", "
            if len(line) + len(piece) > 99:
                chunks.append(line.rstrip())
                line = "            "
            line += piece
        chunks.append(line.rstrip().rstrip(",") + "]")
        lines.extend(chunks)
        lines.append("End Function")
        lines.append("")
    path = pathlib.Path(__file__).with_name("data.fb")
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {len(CASES)} drawings to {path}")


main()
