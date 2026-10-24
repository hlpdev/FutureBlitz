"""Writes data.fb, an image of 23 by 17 pixels that image_transforms.fb resizes, and what Pillow
12.3 resizes it to, by each filter, as array literals. The image is opaque, so Pillow's alpha,
which it premultiplies in bytes, doesn't round its colors, and its alpha is a second image of
its own, an L image's bytes, which Pillow resizes as any channel.

    python make_data.py
"""

from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
WIDTH, HEIGHT = 23, 17
SIZES = [(9, 7), (37, 26), (40, 5)]
FILTERS = [
    ("NEAREST", Image.Resampling.NEAREST), ("BOX", Image.Resampling.BOX),
    ("BILINEAR", Image.Resampling.BILINEAR), ("BICUBIC", Image.Resampling.BICUBIC),
    ("LANCZOS", Image.Resampling.LANCZOS),
]


def source():
    """Gradients across and down, a checkerboard's sharp edges, and a bright square."""
    image = Image.new("RGBA", (WIDTH, HEIGHT))
    for y in range(HEIGHT):
        for x in range(WIDTH):
            checker = 255 if (x // 3 + y // 3) % 2 == 0 else 0
            square = 6 <= x < 12 and 4 <= y < 10
            red = 255 if square else x * 11
            green = 255 if square else y * 15
            image.putpixel((x, y), (red, green, checker, 255))
    return image


def alpha():
    """An alpha that falls from the top-left corner, with a hole."""
    image = Image.new("L", (WIDTH, HEIGHT))
    for y in range(HEIGHT):
        for x in range(WIDTH):
            hole = 14 <= x < 18 and 8 <= y < 12
            image.putpixel((x, y), 0 if hole else max(0, 255 - x * 7 - y * 9))
    return image


def literal(name, comment, values):
    lines = [f"// {comment}", f"Public Function {name}(): UInt8[]", "    Return ["]
    for at in range(0, len(values), 14):
        part = values[at:at + 14]
        row = ", ".join(f"0x{value:02x}" for value in part)
        last = at + len(part) >= len(values)
        lines.append(f"        {row}{'' if last else ','}")
    lines += ["    ]", "End Function", ""]
    return lines


def main():
    lines = [
        "// An image that image_transforms.fb resizes, and what Pillow 12.3 resizes it to, as array",
        "// literals; make_data.py says how they were made.",
        "",
        "Import std.core",
        "",
    ]
    image = source()
    mask = alpha()
    lines += literal("Source", f"The image, {WIDTH} by {HEIGHT}, opaque.", list(image.tobytes()))
    lines += literal("Mask", "An alpha for it, an L image's bytes.", list(mask.tobytes()))
    for name, method in FILTERS:
        for width, height in SIZES:
            resized = image.resize((width, height), method)
            lines += literal(f"{name.title()}{width}x{height}",
                             f"The image resized to {width} by {height}, by {name}.",
                             list(resized.tobytes()))
            resized = mask.resize((width, height), method)
            lines += literal(f"{name.title()}{width}x{height}Mask",
                             f"Its alpha resized to {width} by {height}, by {name}.",
                             list(resized.tobytes()))
    (HERE / "data.fb").write_text("\n".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
