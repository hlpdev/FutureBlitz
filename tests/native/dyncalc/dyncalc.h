#ifndef DYNCALC_H
#define DYNCALC_H

// The C library the tests load as a package's [native] dynamic library, from beside the
// program: a function, text, and state the library keeps between calls.

#include <stdint.h>

#ifdef _WIN32
#define DYNCALC_API __declspec(dllexport)
#else
#define DYNCALC_API __attribute__((visibility("default")))
#endif

DYNCALC_API int32_t dc_add(int32_t a, int32_t b);
DYNCALC_API const char* dc_name(void);

// How many times it's been called, counting this one.
DYNCALC_API int64_t dc_count(void);

#endif
