#ifndef VECMATH_PACKED_H
#define VECMATH_PACKED_H

// Packed structs and unions, shaped like an SDK's: a 32-bit handle before a 64-bit id, a struct
// with a union and a 64-bit id, and a plain struct inside a packed one. Each comes back from C
// by value and goes to it by value and through a pointer, and C says how big it is, so a test
// sees the layout C has.

#include <stdint.h>

#pragma pack(push, 4)
// The id is at 4, off its own alignment.
typedef struct vm_status {
    uint32_t handle;
    int64_t id;
    int32_t state;
} vm_status;

typedef struct vm_late {
    int32_t a;
    int64_t b;
} vm_late;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct vm_tight {
    uint8_t a;
    int32_t b;
    double c;
    uint16_t d;
} vm_tight;
#pragma pack(pop)

#pragma pack(push, 2)
typedef struct vm_half {
    uint8_t a;
    int32_t b;
    int8_t c;
    int64_t d;
} vm_half;
#pragma pack(pop)

// Packed to 8, the most any of its fields has, it's as it would be anyway.
#pragma pack(push, 8)
typedef struct vm_eight {
    int32_t a;
    int64_t b;
} vm_eight;
#pragma pack(pop)

typedef union vm_address {
    uint64_t id;
    uint8_t ip[16];
} vm_address;

#pragma pack(push, 4)
// A union at 4, and an id after it at 20.
typedef struct vm_identity {
    int32_t kind;
    vm_address value;
    int64_t steam;
} vm_identity;

// A plain struct at 4, its inside as it is, and packed ones in a fixed array.
typedef struct vm_nested {
    uint8_t flag;
    vm_eight inner;
    vm_tight many[2];
} vm_nested;
#pragma pack(pop)

typedef union vm_number {
    float f;
    double d;
    int32_t i;
} vm_number;

typedef union vm_floats {
    float f;
    struct {
        float x;
        float y;
    } pair;
} vm_floats;

#pragma pack(push, 2)
typedef union vm_packed_union {
    uint8_t bytes[5];
    int32_t value;
} vm_packed_union;
#pragma pack(pop)

// sizeof and alignof of each, in the order above: 0 for vm_status to 10 for vm_packed_union.
int64_t vm_packed_size(int32_t which);
int64_t vm_packed_align(int32_t which);

// Each made from its numbers, given back changed, and summed through a pointer.
vm_status vm_status_make(uint32_t handle, int64_t id, int32_t state);
vm_status vm_status_next(vm_status status);
int64_t vm_status_sum(const vm_status* status);
vm_late vm_late_next(vm_late late);
vm_tight vm_tight_next(vm_tight tight);
double vm_tight_sum(const vm_tight* tight);
vm_half vm_half_next(vm_half half);
vm_eight vm_eight_next(vm_eight eight);
vm_address vm_address_next(vm_address address);
vm_identity vm_identity_next(vm_identity identity);
int64_t vm_identity_sum(const vm_identity* identity);
vm_nested vm_nested_next(vm_nested nested);
double vm_nested_sum(const vm_nested* nested);
vm_number vm_number_double(vm_number number);
vm_floats vm_floats_swap(vm_floats floats);
vm_packed_union vm_packed_union_next(vm_packed_union packed);
// Sets each field through a pointer, as a callback's out-parameter does.
void vm_status_fill(vm_status* status);

// A union whose enum isn't the member C set: its other member holds what isn't one of the enum's
// values, by value, in a struct, and set through a pointer.
enum vm_kind { VM_KIND_ONE = 1 };
typedef union vm_with_kind {
    enum vm_kind kind;
    int32_t value;
} vm_with_kind;

typedef struct vm_holds_kind {
    int32_t tag;
    vm_with_kind held;
} vm_holds_kind;

vm_with_kind vm_with_kind_give(void);
vm_holds_kind vm_holds_kind_give(void);
void vm_with_kind_set(vm_with_kind* with);

#endif
