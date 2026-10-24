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
