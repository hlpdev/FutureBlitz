#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.image's functions in the runtime. Those over a bundled C library have its prefix, and a
// program links the library only when it calls one: fb_stb_ over stb_image and stb_image_write,
// fb_webp_ over libwebp, fb_svg_ over nanosvg, and fb_zlib_png_ and fb_zlib_apng_, whose PNGs are
// the runtime's own, over zlib-ng. The fb_image_ ones, QOI's and GIF's encoder among them, need
// no library. Arrays and Strings are only borrowed. What gives bytes puts them in `output`, an
// array the caller made, whose old elements they replace. Pixels are RGBA, 8 bits a channel, the
// alpha straight, by rows from the top, as a UInt8[] of width * height * 4. std.image checks sizes,
// levels, and qualities first.
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
/// The frame's pixels, into `output`, a UInt8[].
void fb_image_pixels(const void* image, std::int64_t frame, fb_array_t* output);
/// An HDR image's pixels, into `output`, a Float32[] of width * height * 4: red, green, blue, and
/// alpha, which is 1, by rows from the top, as Radiance's RGBE gives them, linear.
void fb_image_hdr_pixels(const void* image, fb_array_t* output);
void fb_image_free(void* image);
/// Puts `count` zero bytes in `output`, a UInt8[], for a new image's pixels.
void fb_image_blank(fb_array_t* output, std::int64_t count);
/// Puts `count` zero Floats in `output`, a Float32[], for a new HDR image's pixels.
void fb_image_blank_hdr(fb_array_t* output, std::int64_t count);

/// Which format the bytes say they are, by how they start: 1 PNG, 2 JPEG, 3 BMP, 4 TGA, 5 GIF, 6
/// HDR, 7 QOI, or 8 WebP, or 0 for none of those. TGA has no signature, so it's what starts as a
/// TGA's header does once the others don't match.
std::int32_t fb_image_format(const fb_array_t* data);

/// QOI, read and written by the runtime itself: a decoder of its 3 and 4 channels, and an
/// encoder of 4, sRGB.
void* fb_image_qoi_decode(const fb_array_t* data, std::int64_t limit);
void fb_image_qoi_encode(const fb_array_t* pixels, std::int64_t width, std::int64_t height,
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
void fb_image_gif_encoder_add(void* encoder, const fb_array_t* pixels, std::int32_t delay);
void fb_image_gif_encoder_finish(void* encoder, fb_array_t* output);
void fb_image_gif_encoder_free(void* encoder);

/// stb_image's decoding: `format` is one of fb_image_format's 1 PNG, 2 JPEG, 3 BMP, 4 TGA, or 6
/// HDR, and the bytes must be that format. An HDR gives fb_image_hdr_pixels.
void* fb_stb_decode(const fb_array_t* data, std::int32_t format, std::int64_t limit);
/// An APNG's frames, as each is drawn on the last by its blend and disposal, or a PNG's one.
void* fb_stb_decode_apng(const fb_array_t* data, std::int64_t limit);
/// stb_image_write's encoding: `format` 2 JPEG at `quality`, from 1 to 100, which leaves alpha
/// out, 3 BMP, with alpha, or 4 TGA, with alpha, run-length encoded.
void fb_stb_encode(const fb_array_t* pixels, std::int64_t width, std::int64_t height,
                   std::int32_t format, std::int32_t quality, fb_array_t* output);
/// Radiance's RGBE, run-length encoded, of `pixels`, a Float32[] as fb_image_hdr_pixels gives
/// them, whose alpha it leaves out.
void fb_stb_encode_hdr(const fb_array_t* pixels, std::int64_t width, std::int64_t height,
                       fb_array_t* output);

/// PNG's encoder, the runtime's own, over zlib-ng at `level`, from 0 to 9: 8 bits a channel, RGB
/// when every pixel is opaque, else RGBA, each row filtered by the filter that gives the least
/// sum of its bytes' sizes, as libpng picks.
void fb_zlib_png_encode(const fb_array_t* pixels, std::int64_t width, std::int64_t height,
                        std::int32_t level, fb_array_t* output);
/// APNG's encoder: each frame is the whole of its width and height, which replaces the one
/// before, the first one also the image a PNG decoder shows, and plays `plays` times, 0 for
/// forever.
void* fb_zlib_apng_encoder_new(std::int64_t width, std::int64_t height, std::int32_t plays,
                               std::int32_t level);
void fb_zlib_apng_encoder_add(void* encoder, const fb_array_t* pixels, std::int32_t delay);
void fb_zlib_apng_encoder_finish(void* encoder, fb_array_t* output);
void fb_zlib_apng_encoder_free(void* encoder);

/// libwebp's decoding of a WebP, lossy or lossless, which isn't animated.
void* fb_webp_decode(const fb_array_t* data, std::int64_t limit);
/// libwebp's encoding: lossy at `quality`, from 0 to 100, or, when `lossless` isn't 0, lossless at
/// `quality` as an effort, from 0 to 9, keeping the colors of pixels with no alpha.
void fb_webp_encode(const fb_array_t* pixels, std::int64_t width, std::int64_t height,
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
