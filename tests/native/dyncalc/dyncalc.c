#include "dyncalc.h"

static int64_t calls = 0;

int32_t dc_add(int32_t a, int32_t b) {
    return a + b;
}

const char* dc_name(void) {
    return "dyncalc";
}

int64_t dc_count(void) {
    return ++calls;
}
