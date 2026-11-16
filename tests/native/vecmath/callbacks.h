#ifndef VECMATH_CALLBACKS_H
#define VECMATH_CALLBACKS_H

// Structs and unions by value through function pointers, each way: C calls a function it's
// given, and gives a pointer to one of its own. Each function takes the struct first, six
// integers, so the struct after them goes on the stack where registers run out, and the struct
// again, and gives one back: its first field the two's sum plus the integers', and each other
// field the two's sum.

#include <stdbool.h>
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

typedef struct vm_double_int {
    double d;
    int32_t i;
} vm_double_int;

typedef struct vm_int_double {
    int32_t i;
    double d;
} vm_int_double;

typedef struct vm_pair64 {
    int64_t a;
    int64_t b;
} vm_pair64;

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
VM_CALLBACK(double_int, vm_double_int)
VM_CALLBACK(int_double, vm_int_double)

#undef VM_CALLBACK

// Where registers run out, each way: a Struct after eight floats; one that needs two integer
// registers when one is left, after five integers, with one more integer after it; a Struct in
// memory for the result, with one after four integers; narrow integers and a Bool beside one;
// and four floats after eight, which AArch64 passes on the stack. C calls fb's function with
// what it's given and gives what that gives, and gives a pointer to its own, which does as fb's.
typedef vm_two_floats (*vm_after_floats_fn)(float a, float b, float c, float d, float e, float f,
                                            float g, float h, vm_two_floats value);
vm_two_floats vm_call_after_floats(vm_after_floats_fn fn, vm_two_floats value);
vm_after_floats_fn vm_after_floats_pointer(void);

typedef vm_pair64 (*vm_no_room_fn)(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                                   vm_pair64 value, int64_t f);
vm_pair64 vm_call_no_room(vm_no_room_fn fn, vm_pair64 value);
vm_no_room_fn vm_no_room_pointer(void);

typedef vm_twenty_four (*vm_sret_fn)(int64_t a, int64_t b, int64_t c, int64_t d, vm_twelve value);
vm_twenty_four vm_call_sret(vm_sret_fn fn, vm_twelve value);
vm_sret_fn vm_sret_pointer(void);

typedef vm_three (*vm_narrow_fn)(uint8_t a, vm_twelve value, int16_t b, bool c);
vm_three vm_call_narrow(vm_narrow_fn fn, vm_twelve value);
vm_narrow_fn vm_narrow_pointer(void);

typedef vm_four_floats (*vm_hfa_late_fn)(float a, float b, float c, float d, float e, float f,
                                         float g, float h, vm_four_floats value);
vm_four_floats vm_call_hfa_late(vm_hfa_late_fn fn, vm_four_floats value);
vm_hfa_late_fn vm_hfa_late_pointer(void);

// An enum in what crosses a function pointer, as C's caller says, a hue or not: beside a struct
// in registers, inside a struct both ways, and in a union, whose bytes may be the other field's.
typedef enum vm_hue { VM_HUE_RED = 1, VM_HUE_GREEN = 2 } vm_hue;

typedef struct vm_tint {
    vm_hue hue;
    int32_t alpha;
    vm_hue edges[2];
} vm_tint;

typedef union vm_hue_bits {
    vm_hue hue;
    int32_t raw;
} vm_hue_bits;

typedef vm_twelve (*vm_hued_fn)(vm_hue hue, vm_twelve value);
vm_twelve vm_call_hued(vm_hued_fn fn, int32_t hue, vm_twelve value);

typedef void (*vm_tint_painter)(vm_tint tint);
void vm_paint_tint(vm_tint_painter paint, int32_t hue, int32_t edge);

typedef vm_tint (*vm_tinter)(void);
vm_tinter vm_tinter_of(bool valid);

typedef int32_t (*vm_bits_reader)(vm_hue_bits bits);
int32_t vm_read_bits(vm_bits_reader read, int32_t raw);

#endif
