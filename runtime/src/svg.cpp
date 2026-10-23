// std.image's SVG over nanosvg: parsed from text into its shapes, and rasterized at a scale into
// RGBA, its edges antialiased and its alpha straight.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <nanosvg.h>
#include <nanosvgrast.h>
#include <string>
#include <string_view>

using namespace fb::image;

extern "C" {

void* fb_svg_parse(const fb_string_t* text) {
    // nanosvg parses in place, so it gets a copy, which ends in a 0.
    std::string copy(text_of(text));
    NSVGimage* svg = nsvgParse(copy.data(), "px", 96.0F);
    if (svg == nullptr) {
        fb_panic("out of memory: std.image's SVG couldn't be parsed", nullptr);
    }

    if (svg->width <= 0 || svg->height <= 0) {
        nsvgDelete(svg);
        return nullptr;
    }
    return svg;
}

double fb_svg_width(const void* svg) {
    return static_cast<const NSVGimage*>(svg)->width;
}

double fb_svg_height(const void* svg) {
    return static_cast<const NSVGimage*>(svg)->height;
}

void* fb_svg_rasterize(const void* svg, const std::int64_t width, const std::int64_t height,
                       const double scale, const double x, const double y) {
    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    if (rasterizer == nullptr) {
        fb_panic("out of memory: std.image's SVG couldn't be rasterized", nullptr);
    }

    auto* image = new image_t{ .width = width, .height = height, .delays = { 0 }, .plays = 1 };
    std::vector<std::uint8_t>& out =
        image->frames.emplace_back(static_cast<std::size_t>(pixels_of(width, height)) * 4, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): nanosvg reads the image only.
    nsvgRasterize(rasterizer, const_cast<NSVGimage*>(static_cast<const NSVGimage*>(svg)),
                  static_cast<float>(x), static_cast<float>(y), static_cast<float>(scale),
                  out.data(), static_cast<int>(width), static_cast<int>(height),
                  static_cast<int>(width * 4));
    nsvgDeleteRasterizer(rasterizer);
    return image;
}

void fb_svg_free(void* svg) {
    nsvgDelete(static_cast<NSVGimage*>(svg));
}
}
