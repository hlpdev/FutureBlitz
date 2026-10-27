"""Writes font.fb, the bytes of font.py's TrueType font and of cff_font.py's OpenType ones, its
CID font and the other with a charset of each of CFF's forms, some of which it writes as files
too, and data.fb, the pixels image_text.fb's text should have, worked out from their glyphs, not
from the fonts' bytes, so a parser that reads them wrong draws something else: each glyph's
path, its points scaled from the em to the size and put on the line, each quadratic or cubic cut
into the chords that keep it within a hundredth of a pixel, as the runtime cuts them, then
filled together as image_draw's reference fills a region.

    python make_data.py

It needs nothing but Python 3.
"""

import math
import pathlib
import sys

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "image_draw"))
import cff_font  # noqa: E402
import font  # noqa: E402
from reference import draw, f32  # noqa: E402

TOLERANCE = 0.01
MOST_CHORDS = 1024


def quadratic(p0, c, p1):
    """The chords from p0 to p1 of the quadratic through c, but p0: as many as keep them within
    the tolerance, as a quadratic's second difference over 4 times their count squared."""
    dx = p0[0] - 2 * c[0] + p1[0]
    dy = p0[1] - 2 * c[1] + p1[1]
    bend = math.sqrt(dx * dx + dy * dy)
    count = max(1, min(MOST_CHORDS, math.ceil(math.sqrt(bend / (4 * TOLERANCE)))))
    points = []
    for i in range(1, count):
        t = i / count
        u = 1 - t
        points.append((u * u * p0[0] + 2 * u * t * c[0] + t * t * p1[0],
                       u * u * p0[1] + 2 * u * t * c[1] + t * t * p1[1]))
    points.append(p1)
    return points


def cubic(p0, p1, p2, p3):
    """The chords from p0 to p3 of the cubic through p1 and p2, but p0: as many as keep them
    within the tolerance, three quarters of the larger of its two second differences over their
    count squared."""
    ax = p0[0] - 2 * p1[0] + p2[0]
    ay = p0[1] - 2 * p1[1] + p2[1]
    bx = p1[0] - 2 * p2[0] + p3[0]
    by = p1[1] - 2 * p2[1] + p3[1]
    bend = max(math.sqrt(ax * ax + ay * ay), math.sqrt(bx * bx + by * by))
    count = max(1, min(MOST_CHORDS, math.ceil(math.sqrt(0.75 * bend / TOLERANCE))))
    points = []
    for i in range(1, count):
        t = i / count
        u = 1 - t
        points.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] +
                       t * t * t * p3[0],
                       u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] +
                       t * t * t * p3[1]))
    points.append(p3)
    return points


def rings_of(path, place):
    """A glyph path's rings in pixels, `place` putting each of its points there: a ring from each
    move, without its last point when that's back at its first, as each ring closes itself."""
    rings = []

    def close():
        if rings and len(rings[-1]) > 1 and rings[-1][0] == rings[-1][-1]:
            rings[-1].pop()

    current = None
    for kind, *values in path:
        end = place(values[-2], values[-1])
        if kind == "move":
            close()
            rings.append([end])
        elif kind == "line":
            rings[-1].append(end)
        elif kind == "quadratic":
            rings[-1].extend(quadratic(current, place(values[0], values[1]), end))
        else:
            rings[-1].extend(cubic(current, place(values[0], values[1]),
                                   place(values[2], values[3]), end))
        current = end
    close()
    return rings


def text_rings(face, text, size, x, y):
    """The rings of the text's glyphs in the face, font or cff_font, its first line's baseline
    from (x, y)."""
    scale = size / face.UNITS_PER_EM
    line_height = (font.ASCENT - font.DESCENT + font.LINE_GAP) * scale
    pen_x = x
    pen_y = y
    before = None
    rings = []
    for character in text:
        if character == "\n":
            pen_x = x
            pen_y += line_height
            before = None
            continue
        glyph = face.CMAP.get(ord(character), 0)
        if before is not None:
            pen_x += face.KERNING.get((before, glyph), 0) * scale
        at_x = pen_x
        at_y = pen_y
        rings += rings_of(face.path_of(glyph),
                          lambda unit_x, unit_y: (at_x + unit_x * scale, at_y - unit_y * scale))
        pen_x += face.GLYPHS[glyph][1] * scale
        before = glyph
    return rings


# The text image_text.fb draws: (name, width, height, [(face, text, size, x, y, color,
# antialias)]).
BLACK = (0, 0, 0, 255)
CASES = [
    ("Kerned", 96, 32, [(font, "AVTo", 24.0, 2.0, 24.0, BLACK, True)]),
    ("Between", 80, 28, [(font, "iD.ö", f32(21.3), 1.25, f32(21.7), BLACK, True)]),
    ("Missing", 40, 24, [(font, "x\U0001F642", 16.0, 1.0, 18.0, BLACK, True)]),
    ("Lines", 48, 40, [(font, "To\nAV", 14.0, 2.0, 13.0, BLACK, True)]),
    ("Aliased", 56, 24, [(font, "AVo", 18.0, 1.5, 18.5, BLACK, False)]),
    ("Colored", 64, 28, [(font, "AoV", 22.0, 3.0, 21.0, (200, 40, 10, 255), True),
                          (font, "iT", 26.0, 20.0, 24.0, (20, 60, 220, 140), True)]),
    ("Cff", 120, 26, [(cff_font, "LOcf~HZNÖ", 20.0, 2.0, 20.0, BLACK, True)]),
    ("CffMissing", 64, 24, [(cff_font, "Hx Z", f32(16.6), 1.5, 18.5, BLACK, False)]),
]


def literal(name, pixels):
    lines = [f"Public Function {name}(): UInt8[]"]
    line = "    Return ["
    for value in pixels:
        piece = f"{value}, "
        if len(line) + len(piece) > 99:
            lines.append(line.rstrip())
            line = "            "
        line += piece
    lines.append(line.rstrip().rstrip(",") + "]")
    lines.append("End Function")
    return lines


def main():
    # Each font's function, its file, which tests/runtime/font_tests.cpp breaks, or None, what it
    # is, and its bytes.
    fonts = [("FontBytes", "font.ttf", "font.py's font, as TrueType", font.font_bytes()),
             ("CffBytes", "cff.otf", "cff_font.py's font, as OpenType of CFF outlines",
              cff_font.font_bytes()),
             ("CffRangesBytes", None, "The same, its charset of ranges, each's count a byte",
              cff_font.font_bytes(charset=1)),
             ("CffWideRangesBytes", None, "The same, each's count two bytes",
              cff_font.font_bytes(charset=2)),
             ("CidBytes", "cid.otf", "cff_font.py's font, as a CID font's",
              cff_font.font_bytes(cid=True))]
    lines = ["// The fonts image_text.fb draws, from make_data.py."]
    for name, file, what, data in fonts:
        lines += ["", f"// {what}."] + literal(name, data)
        if file is not None:
            (HERE / file).write_bytes(data)
    (HERE / "font.fb").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    # The other programs that need a font each have the TrueType one in a folder of their own,
    # as a program builds with only its own folder's modules.
    for program in ("image_files", "panic_image_text_size", "panic_image_font_size"):
        folder = HERE.parent / program
        folder.mkdir(exist_ok=True)
        own = ["// font.py's font, as TrueType, from image_text/make_data.py.", ""]
        own += literal("FontBytes", fonts[0][3])
        (folder / "font.fb").write_text("\n".join(own) + "\n", encoding="utf-8", newline="\n")

    lines = ["// What image_text.fb's text gives, from make_data.py, which works it out from",
             "// font.py's and cff_font.py's glyphs apart from the runtime. Each is an image's",
             "// RGBA bytes.", ""]
    for name, width, height, texts in CASES:
        pixels = [0] * (width * height * 4)
        for face, text, size, x, y, color, antialias in texts:
            draw(pixels, width, height, text_rings(face, text, size, x, y), color, antialias)
        lines += literal(name, pixels)
        lines.append("")
    (HERE / "data.fb").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    sizes = ", ".join(str(len(data)) for _, _, _, data in fonts)
    print(f"wrote the fonts, of {sizes} bytes, and {len(CASES)} texts")


main()
