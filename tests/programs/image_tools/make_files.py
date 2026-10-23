"""Makes files/, images that common tools wrote, which image_tools.fb decodes: Pillow 12.3 (with
libjpeg-turbo 3.1.4.1 and libwebp 1.6.0) and ImageMagick 7.1.2-29 wrote them, from the patterns
below. make_data.py then writes data.fb of their bytes and their pixels as Pillow decodes them.

    python make_files.py
"""

import subprocess
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
FILES = HERE / "files"
WIDTH = 13
HEIGHT = 9


def color(x, y):
    """A pattern's color at x, y, which differs from pixel to pixel."""
    return (x * 19 % 256, y * 28 % 256, (x * y * 7 + 40) % 256)


def rgba():
    """The pattern, its alpha falling to the bottom right."""
    image = Image.new("RGBA", (WIDTH, HEIGHT))
    for y in range(HEIGHT):
        for x in range(WIDTH):
            image.putpixel((x, y), color(x, y) + (255 - (x + y) * 12,))
    return image


def few_colors(offset=0, transparent=True):
    """Stripes of 6 colors, and a transparent corner, for palettes."""
    colors = [(255, 0, 0), (0, 160, 0), (0, 0, 255), (250, 200, 0), (90, 30, 140), (20, 20, 20)]
    image = Image.new("RGBA", (WIDTH, HEIGHT))
    for y in range(HEIGHT):
        for x in range(WIDTH):
            alpha = 0 if transparent and x + y < 3 else 255
            image.putpixel((x, y), colors[(x + y + offset) // 2 % len(colors)] + (alpha,))
    return image


def magick(*arguments):
    subprocess.run(["magick", *arguments], check=True, cwd=FILES)


def main():
    FILES.mkdir(exist_ok=True)
    rgba().save(FILES / "rgba.png")
    rgba().convert("RGB").save(FILES / "source.ppm")
    few_colors().save(FILES / "few.png")
    magick("source.ppm", "-interlace", "PNG", "interlaced.png")
    magick("few.png", "PNG8:palette.png")
    magick("-size", f"{WIDTH}x{HEIGHT}", "gradient:white-black", "-rotate", "0", "-depth", "16",
           "-colorspace", "Gray", "-define", "png:color-type=0", "gray16.png")

    photo = Image.new("RGB", (16, 16))
    for y in range(16):
        for x in range(16):
            photo.putpixel((x, y), (x * 16, y * 16, 128 + (x - y) * 4))
    photo.save(FILES / "baseline.jpg", quality=90)
    photo.save(FILES / "photo.ppm")
    photo.convert("L").save(FILES / "gray.jpg", quality=90)
    magick("photo.ppm", "-quality", "85", "-interlace", "JPEG", "progressive.jpg")

    rgba().convert("RGB").save(FILES / "rgb24.bmp")
    few_colors(transparent=False).convert("P").save(FILES / "palette8.bmp")
    rgba().convert("1").save(FILES / "mono.bmp")
    magick("rgba.png", "BMP:rgba32.bmp")
    magick("few.png", "-alpha", "off", "-type", "Palette", "-compress", "RLE", "BMP3:rle8.bmp")

    rgba().save(FILES / "raw.tga")
    rgba().save(FILES / "rle.tga", compression="tga_rle")
    few_colors(transparent=False).convert("P").save(FILES / "palette.tga")

    few_colors().save(FILES / "still.gif")
    frames = [few_colors(offset) for offset in range(4)]
    # Frames that change only in part, which Pillow writes as the part that changed.
    frames[2] = frames[1].copy()
    frames[2].paste((0, 0, 0, 0), (4, 2, 9, 6))
    frames[0].save(FILES / "animated.gif", save_all=True, append_images=frames[1:],
                   duration=[100, 50, 200, 70], loop=0, disposal=[1, 2, 1, 2])
    frames[0].save(FILES / "twice.gif", save_all=True, append_images=frames[1:2],
                   duration=40, loop=1)
    magick("source.ppm", "-interlace", "GIF", "interlaced.gif")

    frames[0].save(FILES / "animated.png", save_all=True, append_images=frames[1:],
                   duration=[100, 50, 200, 70], loop=3, disposal=[0, 1, 2, 0], blend=[0, 1, 1, 0])
    frames[0].save(FILES / "default.png", save_all=True, default_image=True,
                   append_images=frames[1:3], duration=80, loop=0)

    magick("source.ppm", "-depth", "32", "-define", "quantum:format=floating-point",
           "-evaluate", "multiply", "3", "wide.hdr")
    magick("source.ppm", "-crop", "6x9+0+0", "+repage", "narrow.hdr")

    rgba().save(FILES / "rgba.qoi")
    magick("source.ppm", "rgb.qoi")

    rgba().save(FILES / "lossless.webp", lossless=True, exact=True)
    rgba().save(FILES / "lossy.webp", quality=80)
    rgba().convert("RGB").save(FILES / "opaque.webp", quality=60)
    frames[0].save(FILES / "animated.webp", save_all=True, append_images=frames[1:2], duration=90)

    for scratch in ["source.ppm", "photo.ppm", "few.png"]:
        (FILES / scratch).unlink()


if __name__ == "__main__":
    main()
