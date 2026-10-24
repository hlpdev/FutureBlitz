"""image_text's font, made here rather than taken from anywhere, so it's under no license but the
project's: a few glyphs on a 1000-unit em, as TrueType has them, its outlines' points on or off
the curve, and its bytes as a TrueType file, with cmap's formats 4 and 12, a composite glyph, and
a kern table. make_data.py writes its bytes and what text in it draws.
"""

import struct

UNITS_PER_EM = 1000
ASCENT = 800
DESCENT = -200
LINE_GAP = 90

ON = True
OFF = False

# Each glyph: its name, advance, left side bearing, and contours of (x, y, on the curve), y up;
# a composite's are its parts', in COMPOSITES.
GLYPHS = [
    (".notdef", 600, 100, [
        [(100, 0, ON), (100, 700, ON), (500, 700, ON), (500, 0, ON)],
        [(150, 50, ON), (450, 50, ON), (450, 650, ON), (150, 650, ON)],
    ]),
    ("space", 300, 0, []),
    ("A", 700, 0, [
        [(0, 0, ON), (280, 750, ON), (420, 750, ON), (700, 0, ON), (520, 0, ON), (440, 250, ON),
         (260, 250, ON), (180, 0, ON)],
        [(300, 400, ON), (400, 400, ON), (350, 580, ON)],
    ]),
    ("V", 700, 0, [
        [(0, 750, ON), (180, 750, ON), (350, 180, ON), (520, 750, ON), (700, 750, ON),
         (440, 0, ON), (260, 0, ON)],
    ]),
    ("T", 650, 0, [
        [(0, 750, ON), (650, 750, ON), (650, 600, ON), (400, 600, ON), (400, 0, ON),
         (250, 0, ON), (250, 600, ON), (0, 600, ON)],
    ]),
    # Only points off the curve: TrueType puts one on it halfway between each two.
    ("o", 600, 50, [
        [(50, 0, OFF), (50, 520, OFF), (550, 520, OFF), (550, 0, OFF)],
        [(150, 100, OFF), (450, 100, OFF), (450, 420, OFF), (150, 420, OFF)],
    ]),
    ("period", 220, 50, [
        [(50, 0, ON), (50, 120, ON), (170, 120, ON), (170, 0, ON)],
    ]),
    ("i", 240, 60, [
        [(60, 0, ON), (60, 520, ON), (180, 520, ON), (180, 0, ON)],
        [(60, 620, ON), (60, 740, ON), (180, 740, ON), (180, 620, ON)],
    ]),
    ("odieresis", 600, 50, None),
    # Points on and off the curve: a straight side and a round one.
    ("D", 650, 0, [
        [(0, 0, ON), (0, 750, ON), (300, 750, ON), (600, 750, OFF), (600, 375, ON),
         (600, 0, OFF), (300, 0, ON)],
    ]),
]

CMAP = {0x20: 1, 0x41: 2, 0x56: 3, 0x54: 4, 0x6F: 5, 0x2E: 6, 0x69: 7, 0xF6: 8, 0x44: 9,
        # Past the BMP, only in format 12.
        0x1F642: 5}

# A composite's parts: (glyph index, x offset, y offset). odieresis is o with two periods over it.
COMPOSITES = {8: [(5, 0, 0), (6, 80, 600), (6, 300, 600)]}

KERNING = {(2, 3): -80, (3, 2): -80, (4, 5): -60}


def contours_of(index):
    """A glyph's contours, a composite's its parts' moved by their offsets."""
    if index in COMPOSITES:
        contours = []
        for part, dx, dy in COMPOSITES[index]:
            for contour in contours_of(part):
                contours.append([(x + dx, y + dy, on) for x, y, on in contour])
        return contours
    return GLYPHS[index][3]


def path_of(index):
    """The glyph's path, as cff_font.py's are: ("move", x, y), ("line", x, y), and ("quadratic",
    cx, cy, x, y), each contour from a point on its curve, its first, its last, or else halfway
    between them, then a line to each point on it and a quadratic through each point off it, to
    the next point on it or halfway to the next off it, and back to its start."""
    path = []
    for contour in contours_of(index):
        first = contour[0]
        last = contour[-1]
        if first[2]:
            start, rest = first, contour[1:]
        elif last[2]:
            start, rest = last, contour[:-1]
        else:
            start, rest = ((last[0] + first[0]) / 2, (last[1] + first[1]) / 2, ON), contour

        path.append(("move", start[0], start[1]))
        off = None
        for p in rest + [start]:
            if p[2]:
                if off is None:
                    path.append(("line", p[0], p[1]))
                else:
                    path.append(("quadratic", off[0], off[1], p[0], p[1]))
                    off = None
            else:
                if off is not None:
                    path.append(("quadratic", off[0], off[1], (off[0] + p[0]) / 2,
                                 (off[1] + p[1]) / 2))
                off = p
    return path


def bounds(contours):
    xs = [p[0] for c in contours for p in c] or [0]
    ys = [p[1] for c in contours for p in c] or [0]
    return min(xs), min(ys), max(xs), max(ys)


def glyph_bytes(index):
    contours = contours_of(index)
    if not contours:
        return b""
    x_min, y_min, x_max, y_max = bounds(contours)
    if index in COMPOSITES:
        out = struct.pack(">hhhhh", -1, x_min, y_min, x_max, y_max)
        parts = COMPOSITES[index]
        for i, (part, dx, dy) in enumerate(parts):
            # Words for its arguments, which are an offset, and more after it but for the last.
            flags = 0x0001 | 0x0002 | (0x0020 if i + 1 < len(parts) else 0)
            out += struct.pack(">HHhh", flags, part, dx, dy)
        return out

    out = struct.pack(">hhhhh", len(contours), x_min, y_min, x_max, y_max)
    end = -1
    for contour in contours:
        end += len(contour)
        out += struct.pack(">H", end)
    out += struct.pack(">H", 0)
    points = [p for c in contours for p in c]
    out += bytes(1 if on else 0 for _, _, on in points)
    last = 0
    for x, _, _ in points:
        out += struct.pack(">h", x - last)
        last = x
    last = 0
    for _, y, _ in points:
        out += struct.pack(">h", y - last)
        last = y
    return out


def cmap_bytes(cmap):
    bmp = sorted(c for c in cmap if c <= 0xFFFF)
    # Format 4: a segment for each code point, then the last one that ends it.
    ends = bmp + [0xFFFF]
    starts = bmp + [0xFFFF]
    deltas = [(cmap[c] - c) % 65536 for c in bmp] + [1]
    count = len(ends)
    power = 1
    while power * 2 <= count:
        power *= 2
    body = struct.pack(">HHHH", count * 2, power * 2, power.bit_length() - 1,
                       count * 2 - power * 2)
    body += b"".join(struct.pack(">H", e) for e in ends) + struct.pack(">H", 0)
    body += b"".join(struct.pack(">H", s) for s in starts)
    body += b"".join(struct.pack(">H", d) for d in deltas)
    body += b"".join(struct.pack(">H", 0) for _ in range(count))
    format4 = struct.pack(">HHH", 4, 6 + len(body), 0) + body

    groups = sorted(cmap.items())
    format12 = struct.pack(">HHIII", 12, 0, 16 + 12 * len(groups), 0, len(groups))
    format12 += b"".join(struct.pack(">III", c, c, g) for c, g in groups)

    header = struct.pack(">HH", 0, 2)
    first = 4 + 2 * 8
    header += struct.pack(">HHI", 3, 1, first) + struct.pack(">HHI", 3, 10, first + len(format4))
    return header + format4 + format12


def kern_bytes(kerning):
    pairs = sorted(kerning.items())
    count = len(pairs)
    search = 1
    while search * 2 <= count:
        search *= 2
    body = struct.pack(">HHHH", count, search * 6, search.bit_length() - 1, (count - search) * 6)
    body += b"".join(struct.pack(">HHh", left, right, value) for (left, right), value in pairs)
    subtable = struct.pack(">HHH", 0, 6 + len(body), 0x0001) + body
    return struct.pack(">HH", 0, 1) + subtable


def checksum(data):
    data += b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(data) // 4}I", data)) & 0xFFFFFFFF


def font_bytes():
    glyphs = [glyph_bytes(i) for i in range(len(GLYPHS))]
    glyf = b""
    offsets = []
    for data in glyphs:
        offsets.append(len(glyf))
        glyf += data + b"\0" * (-len(data) % 4)
    offsets.append(len(glyf))
    loca = b"".join(struct.pack(">I", o) for o in offsets)

    every = [contours_of(i) for i in range(len(GLYPHS))]
    x_min, y_min, x_max, y_max = bounds([c for contours in every for c in contours])
    head = struct.pack(">IIIIHHQQhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x000B,
                       UNITS_PER_EM, 0, 0, x_min, y_min, x_max, y_max, 0, 8, 2, 1, 0)
    hhea = struct.pack(">IhhhHhhhhhhhhhhhH", 0x00010000, ASCENT, DESCENT, LINE_GAP,
                       max(g[1] for g in GLYPHS), 0, 0, x_max, 1, 0, 0, 0, 0, 0, 0, 0,
                       len(GLYPHS))
    points = max(sum(len(c) for c in contours) for contours in every)
    contours = max(len(contours) for contours in every)
    maxp = struct.pack(">IHHHHHHHHHHHHHH", 0x00010000, len(GLYPHS), points, contours, points,
                       contours, 2, 0, 0, 0, 0, 0, 0, 3, 1)
    hmtx = b"".join(struct.pack(">Hh", g[1], g[2]) for g in GLYPHS)
    tables = {b"cmap": cmap_bytes(CMAP), b"glyf": glyf, b"head": head, b"hhea": hhea,
              b"hmtx": hmtx, b"kern": kern_bytes(KERNING), b"loca": loca, b"maxp": maxp,
              b"name": NAME, b"post": POST}
    return sfnt_bytes(0x00010000, tables)


NAME = struct.pack(">HHH", 0, 0, 6)
POST = struct.pack(">IIhhIIIII", 0x00030000, 0, -100, 50, 0, 0, 0, 0, 0)


def sfnt_bytes(version, tables):
    """A font file of the tables, its version 0x00010000 for TrueType's outlines or OTTO's for
    CFF's, with their directory, checksums, and head's adjustment."""
    count = len(tables)
    search = 1
    while search * 2 <= count:
        search *= 2
    out = struct.pack(">IHHHH", version, count, search * 16, search.bit_length() - 1,
                      count * 16 - search * 16)
    offset = 12 + 16 * count
    directory = b""
    body = b""
    for tag in sorted(tables):
        data = tables[tag]
        directory += struct.pack(">4sIII", tag, checksum(data), offset + len(body), len(data))
        body += data + b"\0" * (-len(data) % 4)
    font = bytearray(out + directory + body)
    # head's checkSumAdjustment makes the whole file's sum 0xB1B0AFBA.
    head_at = offset + sum(len(tables[t]) + (-len(tables[t]) % 4) for t in sorted(tables)
                           if t < b"head")
    adjustment = (0xB1B0AFBA - checksum(bytes(font))) & 0xFFFFFFFF
    font[head_at + 8:head_at + 12] = struct.pack(">I", adjustment)
    return bytes(font)
