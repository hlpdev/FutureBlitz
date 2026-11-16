#include "callbacks.h"

// Each struct's double, which C passes as the last argument, and the two's combination, which
// its own function gives: the first field the sum plus the integers', the others the sum.

static vm_text twice_text(vm_text v) {
    v.size *= 2;
    return v;
}

static vm_text join_text(vm_text first, int64_t sum, vm_text last) {
    first.size += last.size + (size_t)sum;
    return first;
}

static vm_three twice_three(vm_three v) {
    v.a = (uint8_t)(v.a * 2);
    v.b = (uint8_t)(v.b * 2);
    v.c = (uint8_t)(v.c * 2);
    return v;
}

static vm_three join_three(vm_three first, int64_t sum, vm_three last) {
    first.a = (uint8_t)(first.a + last.a + sum);
    first.b = (uint8_t)(first.b + last.b);
    first.c = (uint8_t)(first.c + last.c);
    return first;
}

static vm_twelve twice_twelve(vm_twelve v) {
    v.a *= 2;
    v.b *= 2;
    v.c *= 2;
    return v;
}

static vm_twelve join_twelve(vm_twelve first, int64_t sum, vm_twelve last) {
    first.a = (int32_t)(first.a + last.a + sum);
    first.b += last.b;
    first.c += last.c;
    return first;
}

static vm_twenty_four twice_twenty_four(vm_twenty_four v) {
    v.a *= 2;
    v.b *= 2;
    v.c *= 2;
    return v;
}

static vm_twenty_four join_twenty_four(vm_twenty_four first, int64_t sum, vm_twenty_four last) {
    first.a += last.a + sum;
    first.b += last.b;
    first.c += last.c;
    return first;
}

static vm_two_floats twice_two_floats(vm_two_floats v) {
    v.x *= 2;
    v.y *= 2;
    return v;
}

static vm_two_floats join_two_floats(vm_two_floats first, int64_t sum, vm_two_floats last) {
    first.x += last.x + (float)sum;
    first.y += last.y;
    return first;
}

static vm_four_floats twice_four_floats(vm_four_floats v) {
    v.x *= 2;
    v.y *= 2;
    v.z *= 2;
    v.w *= 2;
    return v;
}

static vm_four_floats join_four_floats(vm_four_floats first, int64_t sum, vm_four_floats last) {
    first.x += last.x + (float)sum;
    first.y += last.y;
    first.z += last.z;
    first.w += last.w;
    return first;
}

static vm_packed_pair twice_packed_pair(vm_packed_pair v) {
    v.a *= 2;
    v.b *= 2;
    return v;
}

static vm_packed_pair join_packed_pair(vm_packed_pair first, int64_t sum, vm_packed_pair last) {
    first.a = (int32_t)(first.a + last.a + sum);
    first.b += last.b;
    return first;
}

static vm_double_int twice_double_int(vm_double_int v) {
    v.d *= 2;
    v.i *= 2;
    return v;
}

static vm_double_int join_double_int(vm_double_int first, int64_t sum, vm_double_int last) {
    first.d += last.d + (double)sum;
    first.i += last.i;
    return first;
}

static vm_int_double twice_int_double(vm_int_double v) {
    v.i *= 2;
    v.d *= 2;
    return v;
}

static vm_int_double join_int_double(vm_int_double first, int64_t sum, vm_int_double last) {
    first.i = (int32_t)(first.i + last.i + sum);
    first.d += last.d;
    return first;
}

static vm_either twice_either(vm_either v) {
    v.n *= 2;
    return v;
}

static vm_either join_either(vm_either first, int64_t sum, vm_either last) {
    first.n = (int32_t)(first.n + last.n + sum);
    return first;
}

// C calls the function it's given with the value and its double, and gives what that gives; and
// gives a pointer to its own, which does as the fb Function does.
#define VM_CALLBACK(name, type)                                                                    \
    type vm_call_##name(vm_##name##_fn fn, type value) {                                           \
        return fn(value, 1, 2, 3, 4, 5, 6, twice_##name(value));                                   \
    }                                                                                              \
                                                                                                   \
    static type own_##name(type first, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,      \
                           int64_t f, type last) {                                                 \
        return join_##name(first, a + b + c + d + e + f, last);                                    \
    }                                                                                              \
                                                                                                   \
    vm_##name##_fn vm_##name##_pointer(void) {                                                     \
        return own_##name;                                                                         \
    }

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

// The functions where registers run out: each gives its struct with the numbers before it added
// to its first field.

vm_two_floats vm_call_after_floats(vm_after_floats_fn fn, vm_two_floats value) {
    return fn(1, 2, 3, 4, 5, 6, 7, 8, value);
}

static vm_two_floats own_after_floats(float a, float b, float c, float d, float e, float f, float g,
                                      float h, vm_two_floats value) {
    value.x += a + b + c + d + e + f + g + h;
    return value;
}

vm_after_floats_fn vm_after_floats_pointer(void) {
    return own_after_floats;
}

vm_pair64 vm_call_no_room(vm_no_room_fn fn, vm_pair64 value) {
    return fn(1, 2, 3, 4, 5, value, 6);
}

static vm_pair64 own_no_room(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, vm_pair64 value,
                             int64_t f) {
    value.a += a + b + c + d + e;
    value.b += f;
    return value;
}

vm_no_room_fn vm_no_room_pointer(void) {
    return own_no_room;
}

vm_twenty_four vm_call_sret(vm_sret_fn fn, vm_twelve value) {
    return fn(1, 2, 3, 4, value);
}

static vm_twenty_four own_sret(int64_t a, int64_t b, int64_t c, int64_t d, vm_twelve value) {
    const vm_twenty_four made = { a + b + c + d + value.a, value.b, value.c };
    return made;
}

vm_sret_fn vm_sret_pointer(void) {
    return own_sret;
}

vm_three vm_call_narrow(vm_narrow_fn fn, vm_twelve value) {
    return fn(200, value, -300, true);
}

static vm_three own_narrow(uint8_t a, vm_twelve value, int16_t b, bool c) {
    const vm_three made = { a, (uint8_t)(value.a + (c ? 1 : 0)), (uint8_t)(b + 400) };
    return made;
}

vm_narrow_fn vm_narrow_pointer(void) {
    return own_narrow;
}

vm_four_floats vm_call_hfa_late(vm_hfa_late_fn fn, vm_four_floats value) {
    return fn(1, 2, 3, 4, 5, 6, 7, 8, value);
}

static vm_four_floats own_hfa_late(float a, float b, float c, float d, float e, float f, float g,
                                   float h, vm_four_floats value) {
    value.w += a + b + c + d + e + f + g + h;
    return value;
}

vm_hfa_late_fn vm_hfa_late_pointer(void) {
    return own_hfa_late;
}

vm_twelve vm_call_hued(vm_hued_fn fn, int32_t hue, vm_twelve value) {
    return fn((vm_hue)hue, value);
}

void vm_paint_tint(vm_tint_painter paint, int32_t hue, int32_t edge) {
    const vm_tint tint = { (vm_hue)hue, 50, { (vm_hue)hue, (vm_hue)edge } };
    paint(tint);
}

static vm_tint vm_green_tint(void) {
    const vm_tint tint = { VM_HUE_GREEN, 50, { VM_HUE_GREEN, VM_HUE_RED } };
    return tint;
}

static vm_tint vm_no_tint(void) {
    const vm_tint tint = { (vm_hue)9, 50, { VM_HUE_GREEN, VM_HUE_RED } };
    return tint;
}

vm_tinter vm_tinter_of(bool valid) {
    return valid ? vm_green_tint : vm_no_tint;
}

int32_t vm_read_bits(vm_bits_reader read, int32_t raw) {
    vm_hue_bits bits;
    bits.raw = raw;
    return read(bits);
}
