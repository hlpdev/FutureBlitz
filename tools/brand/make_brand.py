"""Draws FutureBlitz's mark, an F whose upright is an amber lightning bolt, with white arms, and
its wordmark, and writes the sizes the project uses into assets/brand/:

    python tools/brand/make_brand.py [SpaceGrotesk-Bold.ttf]

The wordmark needs Space Grotesk Bold (SIL OFL 1.1, github.com/floriankarsten/space-grotesk);
without the font, only the mark's files are written.

The mark is three polygons on a 100 by 100 grid, its edges all leaning at one angle (0.3, about
17 degrees): the bolt, and the two arms, whose left ends sit on the bolt's edge and the middle
one's bottom on the bolt's kink.
"""

import pathlib

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "assets" / "brand"

AMBER = "#F5A623"
WHITE = "#F4F6FB"
NIGHT = "#0B0D14"

BOLT = [(35.2, 12), (56, 12), (45.8, 46), (60, 46), (26, 92), (36, 56), (22, 56)]
TOP_ARM = [(54, 12), (86, 12), (81.8, 26), (51.8, 26)]
MIDDLE_ARM = [(49, 34), (74, 34), (70.4, 46), (45.8, 46)]


def path(points: list[tuple[float, float]]) -> str:
    return "M" + " L".join(f"{x:g} {y:g}" for x, y in points) + " Z"


def mark_svg(background: bool) -> str:
    tile = f'<rect width="100" height="100" rx="22" fill="{NIGHT}"/>' if background else ""
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">' + tile +
            f'<path d="{path(TOP_ARM)} {path(MIDDLE_ARM)}" fill="{WHITE}"/>'
            f'<path d="{path(BOLT)}" fill="{AMBER}"/></svg>\n')


def rgb(color: str) -> tuple[int, int, int, int]:
    return (int(color[1:3], 16), int(color[3:5], 16), int(color[5:7], 16), 255)


def mark_png(size: int, tile: str, inset: float = 1.0) -> Image.Image:
    """The mark at `size` pixels, drawn 8 times over and scaled down, so its edges are smooth.
    `tile` is "rounded" (an app icon), "square" (Discord crops it to a circle), or "none"; an
    `inset` below 1 draws the mark that much smaller, about its middle."""
    scale = size * 8 / 100
    big = Image.new("RGBA", (size * 8, size * 8), (0, 0, 0, 0))
    draw = ImageDraw.Draw(big)
    if tile == "rounded":
        draw.rounded_rectangle([0, 0, big.width - 1, big.height - 1], radius=22 * scale,
                               fill=rgb(NIGHT))
    elif tile == "square":
        draw.rectangle([0, 0, big.width - 1, big.height - 1], fill=rgb(NIGHT))
    # The mark's own middle, which an inset shrinks it toward, so it stays centered.
    middle_x, middle_y = 54, 52
    for points, color in ((TOP_ARM, WHITE), (MIDDLE_ARM, WHITE), (BOLT, AMBER)):
        draw.polygon([((50 + (x - middle_x) * inset) * scale, (50 + (y - middle_y) * inset) * scale)
                      for x, y in points], fill=rgb(color))
    return big.resize((size, size), Image.Resampling.LANCZOS)


# The wordmark: the mark as its F, then "uture" and "Blitz" in Space Grotesk Bold, leaning at the
# mark's angle, its cap height the mark's, on the bolt's baseline.
WORD_SIZE = 104
WORD_BASELINE = 88
WORD_START = 80
WORD_SPACING = -3
LEAN = 0.3


def wordmark_layout(font_path: pathlib.Path) -> list[tuple[str, float]]:
    """Each letter after the F, and where its pen starts, in the mark's units."""
    from fontTools.ttLib import TTFont

    font = TTFont(font_path)
    scale = WORD_SIZE / font["head"].unitsPerEm
    cmap = font.getBestCmap()
    placed = []
    x = WORD_START
    for letter in "utureBlitz":
        placed.append((letter, x))
        x += font["hmtx"][cmap[ord(letter)]][0] * scale + WORD_SPACING
    return placed


def wordmark_svg(font_path: pathlib.Path, blitz: str) -> str:
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.ttLib import TTFont

    font = TTFont(font_path)
    glyphs = font.getGlyphSet()
    cmap = font.getBestCmap()
    scale = WORD_SIZE / font["head"].unitsPerEm
    letters = []
    placed = wordmark_layout(font_path)
    for i, (letter, x) in enumerate(placed):
        pen = SVGPathPen(glyphs)
        glyphs[cmap[ord(letter)]].draw(pen)
        # Font units up, SVG's down, and the lean: x moves 0.3 for each unit of height.
        offset = x - LEAN * WORD_BASELINE
        color = blitz if i >= len("uture") else WHITE
        letters.append(f'<path transform="matrix({scale:g} 0 {LEAN * scale:g} {-scale:g} '
                       f'{offset:g} {WORD_BASELINE})" d="{pen.getCommands()}" fill="{color}"/>')
    left = 22 - 8 - 6
    right = placed[-1][1] + 60
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{left} 0 {right - left:g} 100">'
            f'<g transform="translate(-8 0)"><path d="{path(TOP_ARM)} {path(MIDDLE_ARM)}" '
            f'fill="{WHITE}"/><path d="{path(BOLT)}" fill="{AMBER}"/></g>' + "".join(letters) +
            "</svg>\n")


def wordmark_png(font_path: pathlib.Path, height: int, blitz: str) -> Image.Image:
    """The wordmark `height` pixels tall, on a transparent background."""
    from PIL import ImageFont

    unit = height * 2 / 100
    placed = wordmark_layout(font_path)
    left, right = 22 - 8 - 6, placed[-1][1] + 60
    big = Image.new("RGBA", (round((right - left) * unit), height * 2), (0, 0, 0, 0))
    draw = ImageDraw.Draw(big)
    for points, color in ((TOP_ARM, WHITE), (MIDDLE_ARM, WHITE), (BOLT, AMBER)):
        draw.polygon([((x - 8 - left) * unit, y * unit) for x, y in points], fill=rgb(color))
    # The letters upright, then leaned as a layer, as the SVG leans them: x moves 0.3 for each unit
    # down from the top.
    letters = Image.new("RGBA", big.size, (0, 0, 0, 0))
    text = ImageDraw.Draw(letters)
    font = ImageFont.truetype(str(font_path), round(WORD_SIZE * unit))
    for i, (letter, x) in enumerate(placed):
        color = blitz if i >= len("uture") else WHITE
        text.text(((x - left) * unit, WORD_BASELINE * unit), letter, font=font, fill=rgb(color),
                  anchor="ls")
    leaned = letters.transform(letters.size, Image.Transform.AFFINE, (1, LEAN, 0, 0, 1, 0),
                               Image.Resampling.BICUBIC)
    big.alpha_composite(leaned)
    small = big.resize((big.width // 2, height), Image.Resampling.LANCZOS)
    # As wide as what's drawn, so a header can place it by its edges.
    left_edge, _, right_edge, _ = small.getbbox()
    return small.crop((left_edge, 0, right_edge, height))


def main() -> None:
    import sys

    OUT.mkdir(parents=True, exist_ok=True)
    for stale in OUT.iterdir():
        if stale.suffix in (".png", ".svg", ".ico"):
            stale.unlink()
    if len(sys.argv) > 1:
        font_path = pathlib.Path(sys.argv[1])
        (OUT / "wordmark.svg").write_text(wordmark_svg(font_path, AMBER), encoding="utf-8")
        (OUT / "wordmark-white.svg").write_text(wordmark_svg(font_path, WHITE), encoding="utf-8")
        for height in (64, 128, 256):
            wordmark_png(font_path, height, AMBER).save(OUT / f"wordmark-{height}.png")
    (OUT / "mark.svg").write_text(mark_svg(False), encoding="utf-8")
    (OUT / "mark-tile.svg").write_text(mark_svg(True), encoding="utf-8")
    for size in (16, 32, 48, 64, 128, 180, 192, 256, 512):
        mark_png(size, "rounded").save(OUT / f"mark-tile-{size}.png")
    for size in (64, 256, 1024):
        mark_png(size, "none").save(OUT / f"mark-{size}.png")
    # Discord shows a server's icon as a circle cut from a square of at least 512.
    # Inset, so the circle doesn't cut the top arm's corner.
    mark_png(512, "square", inset=0.82).save(OUT / "discord-icon-512.png")
    mark_png(256, "rounded").save(OUT / "favicon.ico", sizes=[(16, 16), (32, 32), (48, 48)])
    print("wrote", sorted(p.name for p in OUT.iterdir()))


if __name__ == "__main__":
    main()
