/* stb_image and stb_image_write, built once for std.image. stb_image reads PNG, JPEG, BMP, TGA,
 * and HDR from memory, with its messages for people, and stb_image_write writes JPEG, BMP, TGA,
 * and HDR to it. std.image reads and writes no files through them, and stb_image's other formats
 * aren't built.
 * stb_image_write keeps its file functions, which nothing calls: without them, its HDR writer
 * isn't built either. */

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
