"""image_text's OpenType fonts of CFF outlines, made here as font.py's TrueType one is: glyphs of
absolute moves, lines, and cubics, y up, on a 1000-unit em, each written as a Type 2 charstring
by the operators its runs name, so the fonts use each of Type 2's operators: its path operators
and flexes, hints and a hintmask, widths, 16.16 numbers, local and global subroutines, its
arithmetic and storage, dotsection, and endchar's accented character. One font is a CID font,
its glyphs in two Font DICTs by an FDSelect of format 3, each with its own local subroutines,
and its accented character spelled out, as a CID font's glyphs have no names; the other's
charset is of any of CFF's three formats. make_data.py draws their text from these glyphs, not
from the fonts' bytes.
"""

import struct

import font

UNITS_PER_EM = 1000

# Each glyph: its name, advance, and runs, each an operator and the absolute points it goes
# through: ("move", (x, y)), lines to points, or curves of (c1, c2, end).
GLYPHS = [
    (".notdef", 600, [
        ("move", (100, 0)),
        ("rlineto", [(500, 0), (500, 700), (100, 700)]),
        ("move", (150, 50)),
        ("vlineto", [(150, 650), (450, 650), (450, 50)]),
    ]),
    ("space", 300, []),
    # N: lines whose deltas each of Type 2's arithmetic and storage operators work out.
    ("N", 620, [
        ("move", (60, 0)),
        ("computed", [(120, 0), (180, 0), (180, 520), (440, 0), (560, 0), (560, 750),
                      (440, 750), (440, 230), (180, 750), (120, 750), (60, 750)]),
    ]),
    # O: a ring of two contours the other way around, by hv and vh curves.
    ("O", 700, [
        ("move", (350, 0)),
        ("hvcurveto", [((157, 0), (0, 163), (0, 375)), ((0, 587), (157, 750), (350, 750))]),
        ("hvcurveto", [((543, 750), (700, 587), (700, 375)),
                       ((700, 163), (543, 0), (350, 0))]),
        ("move", (350, 120)),
        ("hvcurveto", [((480, 120), (580, 230), (580, 375))]),
        ("vhcurveto", [((580, 520), (480, 630), (350, 630)),
                       ((220, 630), (120, 520), (120, 375))]),
        ("vhcurveto", [((120, 230), (220, 120), (350, 120))]),
    ]),
    # L: lines across and down by turns, in a local subroutine, which in the CID font is its
    # Font DICT's first, as H's is in the other's.
    ("L", 560, [
        ("move", (60, 0)),
        ("subr", [
            ("hlineto", [(520, 0), (520, 140), (210, 140), (210, 750), (60, 750)]),
        ]),
    ]),
    # c: an open ring by rrcurveto, rcurveline, and rlinecurve.
    ("c", 560, [
        ("move", (520, 120)),
        ("rcurveline", [((460, 40), (390, 0), (300, 0)), ((140, 0), (40, 110), (40, 260)),
                        (40, 270)]),
        ("rrcurveto", [((40, 420), (150, 530), (300, 530))]),
        ("rlinecurve", [(330, 530), ((410, 530), (470, 500), (520, 440))]),
        ("rlineto", [(420, 380)]),
        ("rrcurveto", [((390, 410), (350, 420), (310, 420)),
                       ((230, 420), (180, 360), (180, 265))]),
        ("rrcurveto", [((180, 170), (230, 110), (310, 110)),
                       ((360, 110), (400, 125), (420, 150))]),
    ]),
    # f: vv and hh curves, the first of each starting off its axis.
    ("f", 400, [
        ("move", (100, 0)),
        ("vlineto", [(100, 450), (40, 450), (40, 540), (100, 540)]),
        ("vvcurveto", [((110, 640), (180, 720), (180, 760))]),
        ("hhcurveto", [((200, 770), (260, 770), (300, 770))]),
        ("vlineto", [(300, 670)]),
        ("hhcurveto", [((270, 670), (240, 660), (220, 660))]),
        ("vvcurveto", [((220, 620), (230, 580), (230, 540))]),
        ("hlineto", [(330, 540), (330, 450), (230, 450), (230, 0)]),
    ]),
    # ~: a wave of flex, hflex, hflex1, and flex1, then back.
    ("asciitilde", 900, [
        ("move", (50, 300)),
        ("flex", [((80, 380), (140, 400), (200, 400)), ((260, 400), (300, 360), (330, 330))]),
        ("hflex", [((360, 330), (390, 280), (430, 280)), ((470, 280), (500, 330), (530, 330))]),
        ("hflex1", [((570, 340), (600, 380), (640, 380)), ((680, 380), (700, 350), (720, 330))]),
        ("flex1", [((750, 300), (790, 290), (820, 300)), ((840, 307), (850, 310), (850, 330))]),
        ("rlineto", [(850, 230), (50, 230)]),
    ]),
    # H, in a local subroutine, after hints and a hintmask, and with its width.
    ("H", 700, [
        ("hints", None),
        ("move", (60, 0)),
        ("subr", [
            ("rlineto", [(210, 0), (210, 320), (490, 320), (490, 0), (640, 0), (640, 750),
                         (490, 750), (490, 450), (210, 450), (210, 750), (60, 750)]),
        ]),
    ]),
    # Z, in a global subroutine, with 16.16 numbers, and a width.
    ("Z", 640, [
        ("move", (40, 0)),
        ("gsubr", [
            ("fixed", [(600, 0), (600, 120), (237.5, 120), (600, 640), (600, 750), (52.5, 750),
                       (52.5, 630), (412.5, 630), (40, 110)]),
        ]),
    ]),
    ("dieresis", 400, [
        ("move", (0, 0)),
        ("rlineto", [(100, 0), (100, 100), (0, 100)]),
        ("move", (240, 0)),
        ("rlineto", [(340, 0), (340, 100), (240, 100)]),
    ]),
    # Ö: endchar's O and dieresis, the dieresis at (180, 800), with a width.
    ("Odieresis", 700, [
        ("seac", (180, 800, "O", "dieresis")),
    ]),
    # |: a point past what a glyph's may be, which its arithmetic puts there, so it draws
    # nothing.
    ("bar", 300, [
        ("move", (100, 0)),
        ("far", None),
    ]),
    # ?: a subroutine's number past any there is, so it draws nothing.
    ("question", 500, [
        ("move", (0, 0)),
        ("far call", None),
    ]),
]

# The glyphs with a width in their charstrings, past nominalWidthX's 0.
WIDTHS = {".notdef", "H", "Z", "Odieresis"}

# The SIDs of the glyphs' names, CFF's standard strings', but Odieresis's, the font's own first.
SIDS = {".notdef": 0, "space": 1, "N": 47, "O": 48, "L": 45, "c": 68, "f": 71,
        "asciitilde": 95, "H": 41, "Z": 59, "dieresis": 131, "Odieresis": 391, "bar": 93,
        "question": 32}

# StandardEncoding's codes for the glyphs an accented character is of.
STANDARD_CODES = {"O": 79, "dieresis": 200}


def glyph(name):
    return next(i for i, g in enumerate(GLYPHS) if g[0] == name)


CMAP = {0x20: glyph("space"), 0x4E: glyph("N"), 0x4F: glyph("O"), 0x4C: glyph("L"),
        0x63: glyph("c"), 0x66: glyph("f"), 0x7E: glyph("asciitilde"), 0x48: glyph("H"),
        0x5A: glyph("Z"), 0xA8: glyph("dieresis"), 0xD6: glyph("Odieresis"), 0x7C: glyph("bar"),
        0x3F: glyph("question")}

KERNING = {(glyph("L"), glyph("O")): -40}


def shifted(runs, dx, dy):
    """The runs, their points moved by (dx, dy)."""
    def move(point):
        if isinstance(point[0], tuple):
            return tuple((p[0] + dx, p[1] + dy) for p in point)
        return (point[0] + dx, point[1] + dy)

    out = []
    for name, points in runs:
        if name == "move":
            out.append((name, move(points)))
        elif name in ("subr", "gsubr"):
            out.append((name, shifted(points, dx, dy)))
        elif name == "hints":
            out.append((name, points))
        else:
            out.append((name, [move(p) for p in points]))
    return out


def spelled_out(runs):
    """The runs, an accented character's its base's and its accent's, moved, as a CID font has
    it."""
    out = []
    for name, points in runs:
        if name == "seac":
            dx, dy, base, accent = points
            out += GLYPHS[glyph(base)][2] + shifted(GLYPHS[glyph(accent)][2], dx, dy)
        else:
            out.append((name, points))
    return out


def path_of(index):
    """The glyph's path, as font.py's are: ("move", x, y), ("line", x, y), and ("cubic", x1, y1,
    x2, y2, x, y)."""
    path = []

    def walk(runs):
        for name, points in runs:
            if name == "hints":
                continue
            if name == "move":
                path.append(("move",) + points)
            elif name in ("subr", "gsubr"):
                walk(points)
            else:
                for point in points:
                    if isinstance(point[0], tuple):
                        c1, c2, end = point
                        path.append(("cubic",) + c1 + c2 + end)
                    else:
                        path.append(("line",) + point)

    runs = spelled_out(GLYPHS[index][2])
    if any(name in ("far", "far call") for name, _ in runs):
        return []
    walk(runs)
    return path


# Type 2 charstrings.


def number(value):
    """A charstring's encoding of a number: an integer in its shortest form, or else 16.16."""
    if value != int(value):
        return bytes([255]) + struct.pack(">i", int(round(value * 65536)))
    value = int(value)
    if -107 <= value <= 107:
        return bytes([value + 139])
    if 108 <= value <= 1131:
        value -= 108
        return bytes([(value >> 8) + 247, value & 0xFF])
    if -1131 <= value <= -108:
        value = -value - 108
        return bytes([(value >> 8) + 251, value & 0xFF])
    return bytes([28]) + struct.pack(">h", value)


def numbers(values):
    return b"".join(number(v) for v in values)


OPERATORS = {"hstem": [1], "vstem": [3], "vmoveto": [4], "rlineto": [5], "hlineto": [6],
             "vlineto": [7], "rrcurveto": [8], "callsubr": [10], "return": [11], "endchar": [14],
             "hintmask": [19], "rmoveto": [21], "hmoveto": [22], "rcurveline": [24],
             "rlinecurve": [25], "vvcurveto": [26], "hhcurveto": [27], "callgsubr": [29],
             "vhcurveto": [30], "hvcurveto": [31], "dotsection": [12, 0], "and": [12, 3],
             "or": [12, 4], "not": [12, 5], "abs": [12, 9], "add": [12, 10], "sub": [12, 11],
             "div": [12, 12], "neg": [12, 14], "eq": [12, 15], "drop": [12, 18], "put": [12, 20],
             "get": [12, 21], "ifelse": [12, 22], "random": [12, 23], "mul": [12, 24],
             "sqrt": [12, 26], "dup": [12, 27], "exch": [12, 28], "index": [12, 29],
             "roll": [12, 30], "hflex": [12, 34], "flex": [12, 35], "hflex1": [12, 36],
             "flex1": [12, 37]}


def op(*names):
    return b"".join(bytes(OPERATORS[name]) for name in names)


def by_square_root(d):
    """d as a square root, times the least whole number that keeps the square a charstring's
    number."""
    k = next(k for k in range(1, abs(d) + 2) if d % k == 0 and (d // k) ** 2 <= 32767)
    out = numbers([(d // k) ** 2]) + op("sqrt") + (numbers([k]) + op("mul") if k > 1 else b"")
    return out + (op("neg") if d < 0 else b"")


# Ways to leave a delta, d, on the stack's top, each by others of Type 2's arithmetic and
# storage operators, leaving what's below as it was.
EXPRESSIONS = [
    lambda d: numbers([d - 7, 7]) + op("add"),
    lambda d: numbers([d + 5, 5]) + op("sub"),
    lambda d: numbers([2 * d, 2]) + op("div"),
    lambda d: numbers([-d]) + op("neg"),
    lambda d: numbers([-abs(d)]) + op("abs") + (op("neg") if d < 0 else b""),
    lambda d: numbers([d, 3]) + op("mul") + numbers([3]) + op("div"),
    by_square_root,
    lambda d: numbers([99, d]) + op("exch", "drop", "dup", "drop"),
    # index of 0, of 1, and below 0, the top's next.
    lambda d: numbers([d, 0]) + op("index", "drop"),
    lambda d: numbers([d, 42, 1]) + op("index", "exch", "drop", "exch", "drop"),
    lambda d: numbers([d, -1]) + op("index", "drop"),
    # roll of three up by one, and down by one.
    lambda d: numbers([d, 7, 9, 3, 1]) + op("roll", "drop", "exch", "drop"),
    lambda d: numbers([7, 9, d, 3, -1]) + op("roll", "drop", "exch", "drop"),
    lambda d: numbers([d, 5]) + op("put") + numbers([5]) + op("get"),
    lambda d: numbers([d, d + 1, 1, 2]) + op("ifelse"),
    lambda d: numbers([d + 1, d, 2, 1]) + op("ifelse"),
    # eq, or, not, and and, each 1, then each 0.
    lambda d: (numbers([d - 4, 4, 4]) + op("eq") + numbers([3, 0]) + op("or", "add") +
               numbers([0]) + op("not", "add") + numbers([2, 5]) + op("and", "add", "add")),
    lambda d: (numbers([d, 4, 5]) + op("eq") + numbers([0, 0]) + op("or", "add") +
               numbers([7]) + op("not", "add") + numbers([0, 5]) + op("and", "add", "add")),
    # random's number is above 0 and at most 1.
    lambda d: numbers([d, d + 1]) + op("random") + numbers([1]) + op("ifelse"),
    lambda d: numbers([d + 1, d]) + op("random") + numbers([0]) + op("ifelse"),
]


class Encoder:
    """Writes a glyph's runs as charstrings, relative to where each run starts, checking that
    each run's points are what its operator can say."""

    def __init__(self, local, global_subrs, width):
        self.x = 0
        self.y = 0
        self.local = local
        self.global_subrs = global_subrs
        self.width = width
        self.ended = False

    def delta(self, point):
        dx, dy = point[0] - self.x, point[1] - self.y
        self.x, self.y = point
        return dx, dy

    def curve(self, c1, c2, end):
        d1 = self.delta(c1)
        d2 = self.delta(c2)
        d3 = self.delta(end)
        return d1 + d2 + d3

    def width_first(self):
        """The width, before the first operator that clears the stack, then never again."""
        if self.width is None:
            return []
        width, self.width = self.width, None
        return [width]

    def runs(self, runs):
        out = b""
        for name, points in runs:
            out += self.run(name, points)
        return out

    def run(self, name, points):
        if name == "hints":
            # Two horizontal stems and a vertical one, then a hintmask of the three.
            return (numbers(self.width_first() + [0, 140, 310, 130]) + op("hstem") +
                    numbers([60, 150]) + op("vstem") + op("hintmask") + bytes([0b11100000]))
        if name == "move":
            dx, dy = self.delta(points)
            if dy == 0:
                return numbers(self.width_first() + [dx]) + op("hmoveto")
            if dx == 0:
                return numbers(self.width_first() + [dy]) + op("vmoveto")
            return numbers(self.width_first() + [dx, dy]) + op("rmoveto")
        if name == "seac":
            dx, dy, base, accent = points
            self.ended = True
            return (numbers(self.width_first() + [dx, dy, STANDARD_CODES[base],
                                                  STANDARD_CODES[accent]]) + op("endchar"))
        if name in ("far", "far call"):
            # A number past what a point or a subroutine's number may be, 30000 squared, and
            # squared again five times, about 3.4e286: a line by it each way, then back across,
            # or a call of the local subroutine of that number.
            huge = numbers([30000, 30000]) + op("mul") + op("dup", "mul") * 5
            if name == "far call":
                return huge + op("callsubr")
            return huge + op("dup", "rlineto") + huge + op("neg") + numbers([0]) + op("rlineto")
        if name in ("subr", "gsubr"):
            # The run's charstring as a subroutine of its own, which returns.
            body = self.runs(points) + op("return")
            subrs = self.local if name == "subr" else self.global_subrs
            subrs.append(body)
            bias = 107
            return numbers([len(subrs) - 1 - bias]) + op("callsubr" if name == "subr" else
                                                          "callgsubr")
        if name in ("rlineto", "fixed"):
            values = []
            for point in points:
                values += self.delta(point)
            return numbers(values) + op("rlineto")
        if name == "computed":
            # A dotsection, which does nothing, then each delta by the next of EXPRESSIONS.
            out = op("dotsection")
            deltas = []
            for point in points:
                deltas += self.delta(point)
            assert len(deltas) >= len(EXPRESSIONS)
            for i, d in enumerate(deltas):
                out += EXPRESSIONS[i % len(EXPRESSIONS)](d)
            return out + op("rlineto")
        if name in ("hlineto", "vlineto"):
            # Lines across and down by turns, starting as the operator says.
            across = name == "hlineto"
            values = []
            for point in points:
                dx, dy = self.delta(point)
                assert (dy if across else dx) == 0, (name, point)
                values.append(dx if across else dy)
                across = not across
            return numbers(values) + op(name)
        if name == "rrcurveto":
            values = []
            for c1, c2, end in points:
                values += self.curve(c1, c2, end)
            return numbers(values) + op("rrcurveto")
        if name == "rcurveline":
            values = []
            for curve in points[:-1]:
                values += self.curve(*curve)
            values += self.delta(points[-1])
            return numbers(values) + op("rcurveline")
        if name == "rlinecurve":
            values = []
            for line in points[:-1]:
                values += self.delta(line)
            values += self.curve(*points[-1])
            return numbers(values) + op("rlinecurve")
        if name in ("hvcurveto", "vhcurveto"):
            across = name == "hvcurveto"
            values = []
            for i, (c1, c2, end) in enumerate(points):
                d = self.curve(c1, c2, end)
                last = i + 1 == len(points)
                if across:
                    assert d[1] == 0 and (d[4] == 0 or last), (name, c1)
                    values += [d[0], d[2], d[3], d[5]] + ([d[4]] if last and d[4] else [])
                else:
                    assert d[0] == 0 and (d[5] == 0 or last), (name, c1)
                    values += [d[1], d[2], d[3], d[4]] + ([d[5]] if last and d[5] else [])
                across = not across
            return numbers(values) + op(name)
        if name in ("hhcurveto", "vvcurveto"):
            values = []
            for i, (c1, c2, end) in enumerate(points):
                d = self.curve(c1, c2, end)
                if name == "hhcurveto":
                    assert d[5] == 0 and (d[1] == 0 or i == 0), (name, c1)
                    values += ([d[1]] if d[1] else []) + [d[0], d[2], d[3], d[4]]
                else:
                    assert d[4] == 0 and (d[0] == 0 or i == 0), (name, c1)
                    values += ([d[0]] if d[0] else []) + [d[1], d[2], d[3], d[5]]
            return numbers(values) + op(name)
        if name == "flex":
            values = self.curve(*points[0]) + self.curve(*points[1])
            return numbers(list(values) + [50]) + op("flex")
        if name == "hflex":
            a = self.curve(*points[0])
            b = self.curve(*points[1])
            assert a[1] == 0 and a[5] == 0 and b[1] == 0 and b[3] == -a[3] and b[5] == 0
            return numbers([a[0], a[2], a[3], a[4], b[0], b[2], b[4]]) + op("hflex")
        if name == "hflex1":
            a = self.curve(*points[0])
            b = self.curve(*points[1])
            assert a[5] == 0 and b[1] == 0 and b[5] == -(a[1] + a[3] + b[3])
            return numbers([a[0], a[1], a[2], a[3], a[4], b[0], b[2], b[3], b[4]]) + op("hflex1")
        if name == "flex1":
            a = self.curve(*points[0])
            b = self.curve(*points[1])
            dx = a[0] + a[2] + a[4] + b[0] + b[2]
            dy = a[1] + a[3] + a[5] + b[1] + b[3]
            across = abs(dx) > abs(dy)
            assert (b[5] == -dy) if across else (b[4] == -dx)
            return numbers(list(a) + [b[0], b[1], b[2], b[3], b[4] if across else b[5]]) + \
                op("flex1")
        raise ValueError(name)


# CFF's tables.


def index(entries):
    """An INDEX of the entries, with offsets of 4 bytes."""
    if not entries:
        return struct.pack(">H", 0)
    offsets = [1]
    for entry in entries:
        offsets.append(offsets[-1] + len(entry))
    return (struct.pack(">HB", len(entries), 4) + b"".join(struct.pack(">I", o) for o in offsets)
            + b"".join(entries))


def dict_bytes(entries):
    """A DICT of (operator, operands), each operand an integer of 5 bytes, so its size doesn't
    depend on its values."""
    out = b""
    for operator, operands in entries:
        for value in operands:
            out += bytes([29]) + struct.pack(">i", value)
        out += bytes(operator)
    return out


def charset_bytes(names, form):
    """A charset of what names the glyphs from the second, as the first is .notdef: each's, of
    form 0, or ranges of names one after another, each its first and how many more, in a byte, of
    form 1, or two, of 2."""
    if form == 0:
        return bytes([0]) + b"".join(struct.pack(">H", n) for n in names)
    ranges = []
    for name in names:
        if ranges and ranges[-1][0] + ranges[-1][1] + 1 == name:
            ranges[-1][1] += 1
        else:
            ranges.append([name, 0])
    layout = ">HB" if form == 1 else ">HH"
    return bytes([form]) + b"".join(struct.pack(layout, first, more) for first, more in ranges)


def cff_bytes(cid, charset):
    """A CFF table of GLYPHS, as a CID font of two Font DICTs, whose charset gives each glyph
    its number as its CID, or not, with the charset of that form."""
    global_subrs = []
    charstrings = []
    locals_ = [[], []]
    for i, (name, advance, runs) in enumerate(GLYPHS):
        group = 1 if cid and i >= 5 else 0
        encoder = Encoder(locals_[group], global_subrs, advance if name in WIDTHS else None)
        body = encoder.runs(spelled_out(runs) if cid else runs)
        if encoder.width is not None:
            body = numbers([encoder.width]) + body
        charstrings.append(body if encoder.ended else body + op("endchar"))

    header = bytes([1, 0, 4, 4])
    names = index([b"FBTest"])
    strings = index([b"Adobe", b"Identity"] if cid else [b"Odieresis"])
    gsubrs = index(global_subrs)
    charstring_index = index(charstrings)
    if cid:
        charset_data = charset_bytes(range(1, len(GLYPHS)), 2)
    else:
        charset_data = charset_bytes([SIDS[g[0]] for g in GLYPHS[1:]], charset)

    groups = 2 if cid else 1
    subr_indexes = [index(locals_[g]) for g in range(groups)]
    # Each Private DICT has only Subrs, which counts from the DICT, so the INDEX just past it is
    # as far as the DICT's size.
    private_size = len(dict_bytes([([19], [0])]))
    privates = [dict_bytes([([19], [private_size])]) for _ in range(groups)]

    def top_dict(at_charstrings, at_charset, at_private, at_fd_array, at_fd_select):
        entries = [([17], [at_charstrings]), ([15], [at_charset])]
        if cid:
            entries = [([12, 30], [391, 392, 0])] + entries + [([12, 36], [at_fd_array]),
                                                               ([12, 37], [at_fd_select])]
        else:
            entries.append(([18], [len(privates[0]), at_private]))
        return dict_bytes(entries)

    top_size = len(index([top_dict(0, 0, 0, 0, 0)]))
    at = len(header) + len(names) + top_size + len(strings) + len(gsubrs)
    at_charstrings = at
    at += len(charstring_index)
    at_charset = at
    at += len(charset_data)
    fd_select = b""
    fd_array = b""
    at_fd_select = at_fd_array = 0
    if cid:
        # Format 3: the first five glyphs in the first Font DICT, the rest in the second.
        fd_select = struct.pack(">BHHBHBH", 3, 2, 0, 0, 5, 1, len(GLYPHS))
        at_fd_select = at
        at += len(fd_select)
        fonts_size = len(index([dict_bytes([([18], [0, 0])]) for _ in range(groups)]))
        at_fd_array = at
        at += fonts_size
    private_ats = []
    for g in range(groups):
        private_ats.append(at)
        at += len(privates[g]) + len(subr_indexes[g])
    if cid:
        fd_array = index([dict_bytes([([18], [len(privates[g]), private_ats[g]])])
                          for g in range(groups)])
    top = index([top_dict(at_charstrings, at_charset, private_ats[0], at_fd_array,
                          at_fd_select)])
    assert len(top) == top_size
    out = (header + names + top + strings + gsubrs + charstring_index + charset_data + fd_select +
           fd_array)
    for g in range(groups):
        assert len(out) == private_ats[g]
        out += privates[g] + subr_indexes[g]
    return out


def font_bytes(cid=False, charset=0):
    """The OpenType font of GLYPHS' CFF outlines, a CID font or not, and if not, with a charset
    of that form."""
    advances = [g[1] for g in GLYPHS]
    head = struct.pack(">IIIIHHQQhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x000B,
                       UNITS_PER_EM, 0, 0, 0, 0, 900, 900, 0, 8, 2, 0, 0)
    hhea = struct.pack(">IhhhHhhhhhhhhhhhH", 0x00010000, font.ASCENT, font.DESCENT,
                       font.LINE_GAP, max(advances), 0, 0, 900, 1, 0, 0, 0, 0, 0, 0, 0,
                       len(GLYPHS))
    maxp = struct.pack(">IH", 0x00005000, len(GLYPHS))
    hmtx = b"".join(struct.pack(">Hh", a, 0) for a in advances)
    tables = {b"CFF ": cff_bytes(cid, charset), b"cmap": font.cmap_bytes(CMAP), b"head": head,
              b"hhea": hhea, b"hmtx": hmtx, b"kern": font.kern_bytes(KERNING), b"maxp": maxp,
              b"name": font.NAME, b"post": font.POST}
    return font.sfnt_bytes(0x4F54544F, tables)
