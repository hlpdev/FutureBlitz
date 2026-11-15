#include "packed.h"

#include <stdalign.h>
#include <stddef.h>

int64_t vm_packed_size(const int32_t which) {
    const size_t sizes[] = {
        sizeof(vm_status), sizeof(vm_late),    sizeof(vm_tight),        sizeof(vm_half),
        sizeof(vm_eight),  sizeof(vm_address), sizeof(vm_identity),     sizeof(vm_nested),
        sizeof(vm_number), sizeof(vm_floats),  sizeof(vm_packed_union),
    };
    return which >= 0 && which < 11 ? (int64_t)sizes[which] : -1;
}

int64_t vm_packed_align(const int32_t which) {
    const size_t aligns[] = {
        alignof(vm_status), alignof(vm_late),    alignof(vm_tight),        alignof(vm_half),
        alignof(vm_eight),  alignof(vm_address), alignof(vm_identity),     alignof(vm_nested),
        alignof(vm_number), alignof(vm_floats),  alignof(vm_packed_union),
    };
    return which >= 0 && which < 11 ? (int64_t)aligns[which] : -1;
}

vm_status vm_status_make(const uint32_t handle, const int64_t id, const int32_t state) {
    const vm_status made = { handle, id, state };
    return made;
}

vm_status vm_status_next(vm_status status) {
    status.handle += 1;
    status.id *= 2;
    status.state -= 1;
    return status;
}

int64_t vm_status_sum(const vm_status* const status) {
    return (int64_t)status->handle + status->id * 3 + (int64_t)status->state * 7;
}

vm_late vm_late_next(vm_late late) {
    late.a += 1;
    late.b *= 2;
    return late;
}

vm_tight vm_tight_next(vm_tight tight) {
    tight.a = (uint8_t)(tight.a + 1);
    tight.b *= 2;
    tight.c /= 2.0;
    tight.d = (uint16_t)(tight.d + 3);
    return tight;
}

double vm_tight_sum(const vm_tight* const tight) {
    return (double)tight->a + (double)tight->b * 3.0 + tight->c * 5.0 + (double)tight->d * 7.0;
}

vm_half vm_half_next(vm_half half) {
    half.a = (uint8_t)(half.a + 1);
    half.b *= 2;
    half.c = (int8_t)(half.c - 1);
    half.d *= 3;
    return half;
}

vm_eight vm_eight_next(vm_eight eight) {
    eight.a += 1;
    eight.b *= 2;
    return eight;
}

vm_address vm_address_next(vm_address address) {
    address.ip[0] = (uint8_t)(address.ip[0] + 1);
    address.ip[15] = 0xAB;
    return address;
}

vm_identity vm_identity_next(vm_identity identity) {
    identity.kind += 1;
    identity.value.id += 1;
    identity.steam *= 2;
    return identity;
}

int64_t vm_identity_sum(const vm_identity* const identity) {
    return (int64_t)identity->kind + (int64_t)identity->value.id * 3 + identity->steam * 5;
}

vm_nested vm_nested_next(vm_nested nested) {
    nested.flag = (uint8_t)(nested.flag + 1);
    nested.inner = vm_eight_next(nested.inner);
    nested.many[0] = vm_tight_next(nested.many[0]);
    nested.many[1] = vm_tight_next(nested.many[1]);
    return nested;
}

double vm_nested_sum(const vm_nested* const nested) {
    return (double)nested->flag + (double)nested->inner.a * 3.0 + (double)nested->inner.b * 5.0 +
           vm_tight_sum(&nested->many[0]) + vm_tight_sum(&nested->many[1]) * 11.0;
}

vm_number vm_number_double(vm_number number) {
    number.d *= 2.0;
    return number;
}

vm_floats vm_floats_swap(vm_floats floats) {
    const float x = floats.pair.x;
    floats.pair.x = floats.pair.y;
    floats.pair.y = x;
    return floats;
}

vm_packed_union vm_packed_union_next(vm_packed_union packed) {
    packed.value += 1;
    packed.bytes[4] = (uint8_t)(packed.bytes[4] + 2);
    return packed;
}

void vm_status_fill(vm_status* const status) {
    status->handle = 0xDEADBEEFu;
    status->id = -1234567890123LL;
    status->state = 42;
}
