"""Writes data.fb, of the bytes of the images in files/, which make_files.py had common tools make,
and their pixels, as array literals that image_tools.fb reads, since a program can't read a file
until std.io's files. Each image's pixels, RGBA by rows from the top, its frames' one after
another, are what Pillow 12.3 decodes, a GIF's transparent ones black, but an HDR's, which
Pillow doesn't read: those are its
RGBE as stb_image reads it, each channel its byte times 2 to the power of the exponent less 136,
which a Float32 holds exactly, and a 16-bit gray PNG's, whose bytes are each gray's top 8 bits,
as stb_image keeps them.

    python make_data.py
"""

from pathlib import Path

from PIL import Image, ImageSequence

HERE = Path(__file__).resolve().parent
FILES = [
    ("RgbaPng", "rgba.png"), ("InterlacedPng", "interlaced.png"), ("PalettePng", "palette.png"),
    ("Gray16Png", "gray16.png"), ("AnimatedPng", "animated.png"), ("DefaultPng", "default.png"),
    ("BaselineJpeg", "baseline.jpg"), ("ProgressiveJpeg", "progressive.jpg"),
    ("GrayJpeg", "gray.jpg"), ("Rgb24Bmp", "rgb24.bmp"), ("Palette8Bmp", "palette8.bmp"),
    ("MonoBmp", "mono.bmp"), ("Rgba32Bmp", "rgba32.bmp"), ("Rle8Bmp", "rle8.bmp"),
    ("RawTga", "raw.tga"), ("RleTga", "rle.tga"), ("PaletteTga", "palette.tga"),
    ("StillGif", "still.gif"), ("AnimatedGif", "animated.gif"), ("TwiceGif", "twice.gif"),
    ("InterlacedGif", "interlaced.gif"), ("WideHdr", "wide.hdr"), ("NarrowHdr", "narrow.hdr"),
    ("RgbaQoi", "rgba.qoi"), ("RgbQoi", "rgb.qoi"), ("LosslessWebP", "lossless.webp"),
    ("LossyWebP", "lossy.webp"), ("OpaqueWebP", "opaque.webp"), ("AnimatedWebP", "animated.webp"),
]
# The files that aren't std.image's to read, which have no pixels here.
UNREAD = {"rle8.bmp", "animated.webp"}


def pixels(path):
    """The image's frames' RGBA bytes, one after another, as Pillow decodes them."""
    image = Image.open(path)
    if image.mode == "I;16":
        return bytes(value for gray in image.get_flattened_data() for value in (gray >> 8,) * 3 + (255,))

    out = bytearray()
    for frame in ImageSequence.Iterator(image):
        out += frame.convert("RGBA").tobytes()
    if image.format == "GIF":
        # A GIF's transparent pixels have no color, and Pillow gives them their index's.
        for at in range(0, len(out), 4):
            if out[at + 3] == 0:
                out[at:at + 3] = bytes(3)
    return bytes(out)


def hdr_floats(data):
    """An HDR's pixels as stb_image reads them: each channel's byte times 2 ** (exponent - 136),
    and an alpha of 1, from its rows, flat or run-length encoded."""
    header, _, rest = data.partition(b"\n\n")
    size, _, body = rest.partition(b"\n")
    _, height, _, width = size.split()
    width, height = int(width), int(height)
    rows = []
    at = 0
    for _ in range(height):
        if width >= 8 and body[at] == 2 and body[at + 1] == 2:
            at += 4
            channels = []
            for _ in range(4):
                channel = []
                while len(channel) < width:
                    count = body[at]
                    at += 1
                    if count > 128:
                        channel += [body[at]] * (count - 128)
                        at += 1
                    else:
                        channel += list(body[at:at + count])
                        at += count
                channels.append(channel)
            rows.append(list(zip(*channels)))
        else:
            rows.append([tuple(body[at + i * 4:at + i * 4 + 4]) for i in range(width)])
            at += width * 4
    values = []
    for row in rows:
        for r, g, b, e in row:
            scale = 2.0 ** (e - 136) if e != 0 else 0.0
            values += [r * scale, g * scale, b * scale, 1.0]
    return values


def literal(name, comment, kind, values):
    lines = [f"// {comment}", f"Public Function {name}(): {kind}[]", "    Return ["]
    step = 14 if kind == "UInt8" else 6
    for at in range(0, len(values), step):
        part = values[at:at + step]
        if kind == "UInt8":
            row = ", ".join(f"0x{value:02x}" for value in part)
        else:
            row = ", ".join(repr(value) for value in part)
        last = at + len(part) >= len(values)
        lines.append(f"        {row}{'' if last else ','}")
    lines += ["    ]", "End Function", ""]
    return lines


def main():
    lines = [
        "// The bytes of the images in files/, which common tools made, and their pixels, as array",
        "// literals, since a program can't read a file until std.io's files; make_files.py says how",
        "// each was made, and make_data.py how its pixels were found.",
        "",
        "Import std.core",
        "",
    ]
    for name, file in FILES:
        data = (HERE / "files" / file).read_bytes()
        lines += literal(name, f"{file}, {len(data)} bytes.", "UInt8", list(data))
        if file in UNREAD:
            continue

        if file.endswith(".hdr"):
            floats = hdr_floats(data)
            lines += literal(f"{name}Pixels", f"{file}'s pixels, as Float32s.", "Float32", floats)
        else:
            lines += literal(f"{name}Pixels", f"{file}'s pixels.", "UInt8",
                             list(pixels(HERE / "files" / file)))
    (HERE / "data.fb").write_text("\n".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
