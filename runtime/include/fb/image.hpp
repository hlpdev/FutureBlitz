#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.image's functions in the runtime. Those over a bundled C library have its prefix, and a
// program links the library only when it calls one: fb_stb_ over stb_image and stb_image_write,
// fb_webp_ over libwebp, fb_svg_ over nanosvg, and fb_zlib_png_ and fb_zlib_apng_, whose PNGs are
// the runtime's own, over zlib-ng. The fb_image_ ones, QOI's and GIF's encoder among them, need
// no library. Arrays and Strings are only borrowed. What gives encoded bytes puts them in
// `output`, an array the caller made, whose old elements they replace. Pixels are RGBA, 8 bits a
// channel, the alpha straight, by rows from the top, width * height * 4 bytes at a pointer into an
// Image's Buffer<UInt8>, and what gives pixels writes them where `output` points, as many as the
// size it's given or the image's says. std.image checks sizes, levels, and qualities first.
//
// A decoder gives an image, which fb_image_'s functions read and free: its result, 0, or 1 when
// the bytes aren't the format or are broken, 2 when they end too soon, 3 when its pixels, all its
// frames' counted, are more than the decoder's `limit`, or 4 when the format's use isn't supported,
// like an animated WebP; and once it's 0, its size and its frames, each with its delay in
// milliseconds, and how many times they play, 0 for forever.

extern "C" {

std::int32_t fb_image_result(const void* image);
fb_string_t* fb_image_message(const void* image);
std::int64_t fb_image_width(const void* image);
std::int64_t fb_image_height(const void* image);
std::int64_t fb_image_frames(const void* image);
std::int32_t fb_image_delay(const void* image, std::int64_t frame);
std::int32_t fb_image_plays(const void* image);
/// The frame's pixels, where `output` points.
void fb_image_pixels(const void* image, std::int64_t frame, std::uint8_t* output);
/// An HDR image's pixels, where `output` points, width * height * 4 floats: red, green, blue, and
/// alpha, which is 1, by rows from the top, as Radiance's RGBE gives them, linear.
void fb_image_hdr_pixels(const void* image, float* output);
void fb_image_free(void* image);

/// Which format the bytes say they are, by how they start: 1 PNG, 2 JPEG, 3 BMP, 4 TGA, 5 GIF, 6
/// HDR, 7 QOI, or 8 WebP, or 0 for none of those. TGA has no signature, so it's what starts as a
/// TGA's header does once the others don't match.
std::int32_t fb_image_format(const fb_array_t* data);

/// QOI, read and written by the runtime itself: a decoder of its 3 and 4 channels, and an
/// encoder of 4, sRGB.
void* fb_image_qoi_decode(const fb_array_t* data, std::int64_t limit);
void fb_image_qoi_encode(const std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                         fb_array_t* output);

/// GIF, read and written by the runtime itself. Its decoder gives each frame as it's drawn on the
/// last, by the frame before's disposal, on a canvas that starts transparent, as browsers draw
/// them, or the first frame alone when `first_only` isn't 0. Its encoder makes each frame the
/// whole of the GIF's width and height, with a palette of its own, its pixels' colors when there
/// are 256 or fewer, counting one for those whose alpha is below 128, which are transparent, or
/// else colors median cut picks, with Floyd-Steinberg's dithering. A GIF of one frame has no
/// delay or plays; one of more is drawn on a transparent canvas each frame, and plays `plays`
/// times, 0 for forever. Delays are in milliseconds, kept in hundredths of a second.
void* fb_image_gif_decode(const fb_array_t* data, std::int32_t first_only, std::int64_t limit);
void* fb_image_gif_encoder_new(std::int64_t width, std::int64_t height, std::int32_t plays);
void fb_image_gif_encoder_add(void* encoder, const std::uint8_t* pixels, std::int32_t delay);
void fb_image_gif_encoder_finish(void* encoder, fb_array_t* output);
void fb_image_gif_encoder_free(void* encoder);

/// The transforms of an image's pixels, `width` by `height` of them, which give new ones in
/// `output`. Resizing gives `new_width` by `new_height` pixels by `filter`: 0 the nearest one's, 1
/// a box's, 2 a triangle's, 3 Keys's cubic, or 4 Lanczos's of 3 lobes, each widened as the image
/// shrinks, its colors weighted by their alpha, as Pillow resamples.
void fb_image_resize(const std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                     std::int64_t new_width, std::int64_t new_height, std::int32_t filter,
                     std::uint8_t* output);
/// The `crop_width` by `crop_height` pixels from (`x`, `y`), which are all in the image.
void fb_image_crop(const std::uint8_t* pixels, std::int64_t width, std::int64_t x, std::int64_t y,
                   std::int64_t crop_width, std::int64_t crop_height, std::uint8_t* output);
/// The pixels flipped, `how` 0 across or 1 down, or turned, 2 a quarter right, 3 a quarter left,
/// or 4 a half.
void fb_image_orient(const std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                     std::int32_t how, std::uint8_t* output);
/// Draws `source`'s pixels on `target`'s, its top-left one at (`x`, `y`), over them by its alpha
/// when `blend` isn't 0, else in their place; what falls outside `target` is left out. A source
/// whose memory is any of the target's is drawn from a copy of it.
void fb_image_blit(std::uint8_t* target, std::int64_t width, std::int64_t height,
                   const std::uint8_t* source, std::int64_t source_width,
                   std::int64_t source_height, std::int64_t x, std::int64_t y, std::int32_t blend);
/// Gives each of `target`'s pixels the color `rgba`, whose top byte is its red.
void fb_image_fill(std::uint8_t* target, std::int64_t width, std::int64_t height,
                   std::uint32_t rgba);

/// Drawing on `pixels`, `width` by `height` of them, the color `rgba`, whose top byte is its
/// red, over each pixel by the region drawn: a pixel is the square from its (x, y) to (x + 1,
/// y + 1), and with `antialias` it's drawn by the part of it the region covers, exactly, its
/// color's alpha times that, rounded to the nearest, a half and what's within a billionth below
/// one up, or else wholly when the region holds its center, which on the region's edge is in it
/// on the edge's right, and on its top but not its bottom; a pixel more than 32 edges pass
/// through is measured along 16 lines across it. Rings are `coords`' points, a
/// Float64[] of x then y, `counts`' of them each, an Int32[]. A region is filled by the nonzero
/// rule. An ellipse is a polygon within a hundredth of a pixel of it, from its rightmost point,
/// of 4 times the fewest sides from 2 up that keep it so, at most 32768, by sines of the
/// runtime's own. A stroke is what its pieces cover together: a rectangle along each segment,
/// half `line_width` either side, a join at each corner on its outside, by `join`, 0 a miter,
/// past `miter_limit` times the width a bevel, 1 the line width's circle, or 2 a bevel, and,
/// unless it's `closed`, a cap at each end, by `cap`, 0 none, 1 the width's circle, or 2 a
/// square half the width past the end.
void fb_image_fill_rings(std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                         const fb_array_t* coords, const fb_array_t* counts, std::uint32_t rgba,
                         std::int32_t antialias);
void fb_image_fill_ellipse(std::uint8_t* pixels, std::int64_t width, std::int64_t height, double x,
                           double y, double rx, double ry, std::uint32_t rgba,
                           std::int32_t antialias);
void fb_image_stroke(std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                     const fb_array_t* coords, const fb_array_t* counts, std::int32_t closed,
                     double line_width, std::int32_t cap, std::int32_t join, double miter_limit,
                     std::uint32_t rgba, std::int32_t antialias);
/// An ellipse's stroke: its polygon's, closed, with miter joins, whose corners turn so little
/// they're within the tolerance of its curve's offset.
void fb_image_stroke_ellipse(std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                             double x, double y, double rx, double ry, double line_width,
                             std::uint32_t rgba, std::int32_t antialias);

/// TrueType and OpenType fonts, read by the runtime itself (font.cpp, and cff.cpp for CFF's
/// outlines), each offset checked first: a font of `data`, which it copies, and whose error is
/// "" unless it isn't one, or is one of a kind it doesn't read, which fb_font_unsupported says,
/// like CFF2's outlines. Its metrics are in font units, its descent 0 or below, as hhea has
/// them.
void* fb_font_parse(const fb_array_t* data);
fb_string_t* fb_font_error(const void* font);
std::int32_t fb_font_unsupported(const void* font);
std::int32_t fb_font_units(const void* font);
std::int32_t fb_font_ascent(const void* font);
std::int32_t fb_font_descent(const void* font);
std::int32_t fb_font_line_gap(const void* font);
/// Whether cmap gives the code point a glyph.
std::int32_t fb_font_has(const void* font, std::uint32_t code_point);
void fb_font_free(void* font);
/// The widest line of `text`'s width, in pixels, at `size` pixels an em, as fb_image_fill_text
/// lays it out.
double fb_font_width(const void* font, const fb_string_t* text, double size);
/// Fills the glyphs of `text` in `font` at `size` pixels an em, its first line's baseline from
/// (`x`, `y`), as one region: each glyph after the last's advance and the kerning between them,
/// unrounded, each line feed a line's height, ascent less descent and the line gap, down, and
/// a code point the font has no glyph for its .notdef; each glyph's quadratics and cubics cut
/// into the chords that keep them within the tolerance, a quadratic's second difference over 4
/// times the count squared, and three quarters of the larger of a cubic's two over the count
/// squared, at most 1024, but a curve wholly outside the image, which is one chord; a glyph of
/// more than 65536 points so cut isn't drawn.
void fb_image_fill_text(std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                        const void* font, const fb_string_t* text, double size, double x, double y,
                        std::uint32_t rgba, std::int32_t antialias);

/// stb_image's decoding: `format` is one of fb_image_format's 1 PNG, 2 JPEG, 3 BMP, 4 TGA, or 6
/// HDR, and the bytes must be that format. An HDR gives fb_image_hdr_pixels.
void* fb_stb_decode(const fb_array_t* data, std::int32_t format, std::int64_t limit);
/// An APNG's frames, as each is drawn on the last by its blend and disposal, or a PNG's one.
void* fb_stb_decode_apng(const fb_array_t* data, std::int64_t limit);
/// stb_image_write's encoding: `format` 2 JPEG at `quality`, from 1 to 100, which leaves alpha
/// out, 3 BMP, with alpha, or 4 TGA, with alpha, run-length encoded.
void fb_stb_encode(const std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                   std::int32_t format, std::int32_t quality, fb_array_t* output);
/// Radiance's RGBE, run-length encoded, of `pixels`, a Float32[] as fb_image_hdr_pixels gives
/// them, whose alpha it leaves out.
void fb_stb_encode_hdr(const float* pixels, std::int64_t width, std::int64_t height,
                       fb_array_t* output);

/// PNG's encoder, the runtime's own, over zlib-ng at `level`, from 0 to 9: 8 bits a channel, RGB
/// when every pixel is opaque, else RGBA, each row filtered by the filter that gives the least
/// sum of its bytes' sizes, as libpng picks.
void fb_zlib_png_encode(const std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                        std::int32_t level, fb_array_t* output);
/// APNG's encoder: each frame is the whole of its width and height, which replaces the one
/// before, the first one also the image a PNG decoder shows, and plays `plays` times, 0 for
/// forever.
void* fb_zlib_apng_encoder_new(std::int64_t width, std::int64_t height, std::int32_t plays,
                               std::int32_t level);
void fb_zlib_apng_encoder_add(void* encoder, const std::uint8_t* pixels, std::int32_t delay);
void fb_zlib_apng_encoder_finish(void* encoder, fb_array_t* output);
void fb_zlib_apng_encoder_free(void* encoder);

/// libwebp's decoding of a WebP, lossy or lossless, which isn't animated.
void* fb_webp_decode(const fb_array_t* data, std::int64_t limit);
/// libwebp's encoding: lossy at `quality`, from 0 to 100, or, when `lossless` isn't 0, lossless at
/// `quality` as an effort, from 0 to 9, keeping the colors of pixels with no alpha.
void fb_webp_encode(const std::uint8_t* pixels, std::int64_t width, std::int64_t height,
                    std::int32_t quality, std::int32_t lossless, fb_array_t* output);

/// nanosvg's SVG, parsed from `text` at 96 dots an inch: null when it has no size, which is what
/// nanosvg gives for what isn't an SVG. Its size is in pixels.
void* fb_svg_parse(const fb_string_t* text);
double fb_svg_width(const void* svg);
double fb_svg_height(const void* svg);
/// The SVG drawn at `scale`, moved by `x` and `y` after, into an image of `width` by `height`.
void* fb_svg_rasterize(const void* svg, std::int64_t width, std::int64_t height, double scale,
                       double x, double y);
void fb_svg_free(void* svg);
}
