/* stb_image and stb_image_write, built once for std.image. stb_image reads PNG, JPEG, BMP, TGA,
 * and HDR from memory, with its messages for people, and stb_image_write writes JPEG, BMP, TGA,
 * and HDR to it. std.image reads and writes no files through them, and stb_image's other formats
 * aren't built.
 * stb_image_write keeps its file functions, which nothing calls: without them, its HDR writer
 * isn't built either. */

#include <stdlib.h>

/* What stb_image allocates starts zeroed: a JPEG whose markers break after its frame header
 * decodes without an error as far as it got, and what it didn't get to would otherwise be what
 * the memory held before, which can be another part of the program's. */
#define STBI_MALLOC(size)           calloc(1, size)
#define STBI_REALLOC(pointer, size) realloc(pointer, size)
#define STBI_FREE(pointer)          free(pointer)

/* Any side an int holds: by default stb_image reads none past 1 << 24, which std.image writes.
 * std.image checks an image's pixels against its limit first, and stb_image keeps its own checks
 * of its sizes. */
#define STBI_MAX_DIMENSIONS 0x7fffffff

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* stbi_info_from_memory for one format, which std.image has already told by how the data starts:
 * `format` is fb_image_format's 1 PNG, 2 JPEG, 3 BMP, 4 TGA, or 6 HDR. stbi_info_from_memory
 * tries every format, so why it failed is the last one's, as an image that's too large is
 * an image of no known type. */
int fb_stbi_info_of(int format, const stbi_uc* data, int size, int* width, int* height,
                    int* channels) {
    stbi__context context;
    stbi__start_mem(&context, data, size);
    switch (format) {
    case 1:
        return stbi__png_info(&context, width, height, channels);
    case 2:
        return stbi__jpeg_info(&context, width, height, channels);
    case 3:
        return stbi__bmp_info(&context, width, height, channels);
    case 4:
        return stbi__tga_info(&context, width, height, channels);
    case 6:
        return stbi__hdr_info(&context, width, height, channels);
    default:
        return stbi__err("unknown format", "Image not of any known type, or corrupt");
    }
}
