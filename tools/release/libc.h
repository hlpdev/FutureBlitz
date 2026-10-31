/* A header of the C library's headers, which release.yml has the installed release's fb-bindgen
 * read, so the target's C headers it ships have every header theirs include. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct libc_t {
    uint32_t id;
    bool on;
    size_t size;
} libc_t;

size_t libc_length(const char* text);
