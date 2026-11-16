#ifndef VECMATH_CALLBACKS_H
#define VECMATH_CALLBACKS_H

// Structs and unions by value through function pointers, each way: C calls a function it's
// given, and gives a pointer to one of its own. Each function takes the struct first, six
// integers, so the struct after them goes on the stack where registers run out, and the struct
// again, and gives one back: its first field the two's sum plus the integers', and each other
// field the two's sum.

#include <stddef.h>
#include <stdint.h>

// Discord's string: a pointer and a size_t.
typedef struct vm_text {
    const uint8_t* bytes;
    size_t size;
} vm_text;

typedef struct vm_three {
    uint8_t a;
    uint8_t b;
    uint8_t c;
} vm_three;

typedef struct vm_twelve {
    int32_t a;
    int32_t b;
    int32_t c;
} vm_twelve;

typedef struct vm_twenty_four {
    int64_t a;
    int64_t b;
    int64_t c;
} vm_twenty_four;

typedef struct vm_two_floats {
    float x;
    float y;
} vm_two_floats;

typedef struct vm_four_floats {
    float x;
    float y;
    float z;
    float w;
} vm_four_floats;

#pragma pack(push, 4)
typedef struct vm_packed_pair {
    int32_t a;
    int64_t b;
} vm_packed_pair;
#pragma pack(pop)

typedef union vm_either {
    int32_t n;
    float f;
} vm_either;

#define VM_CALLBACK(name, type)                                                                    \
    typedef type (*vm_##name##_fn)(type first, int64_t a, int64_t b, int64_t c, int64_t d,         \
                                   int64_t e, int64_t f, type last);                               \
    type vm_call_##name(vm_##name##_fn fn, type value);                                            \
    vm_##name##_fn vm_##name##_pointer(void);

VM_CALLBACK(text, vm_text)
VM_CALLBACK(three, vm_three)
VM_CALLBACK(twelve, vm_twelve)
VM_CALLBACK(twenty_four, vm_twenty_four)
VM_CALLBACK(two_floats, vm_two_floats)
VM_CALLBACK(four_floats, vm_four_floats)
VM_CALLBACK(packed_pair, vm_packed_pair)
VM_CALLBACK(either, vm_either)

#undef VM_CALLBACK

#endif
