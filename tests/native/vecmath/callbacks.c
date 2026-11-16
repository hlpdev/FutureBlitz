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

vm_twelve vm_call_hued(vm_hued_fn fn, int32_t hue, vm_twelve value) {
    return fn((vm_hue)hue, value);
}
