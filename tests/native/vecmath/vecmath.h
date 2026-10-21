#ifndef VECMATH_H
#define VECMATH_H

// The C library the FFI tests call, as a package's [native] library: numbers, Bools, small
// integers that C's ABI extends, text, out-parameters, pointers, structs, enums, callbacks,
// constants, and an opaque handle.

#include <stdbool.h>
#include <stdint.h>

int32_t vm_add(int32_t a, int32_t b);
int64_t vm_add64(int64_t a, int64_t b);
double vm_scale(double value, double by);
float vm_halve(float value);

// A Bool is a byte to C, which FutureBlitz extends as clang does.
bool vm_is_even(int32_t value);
int32_t vm_count(bool a, bool b, bool c);

// A result narrower than 32 bits leaves the rest of its register as it is, on Windows, so
// these return what's left of their argument's.
int8_t vm_low8(int32_t value);
uint8_t vm_low_u8(int32_t value);
int16_t vm_low16(int32_t value);
int32_t vm_widen8(int8_t value);
uint32_t vm_widen_u16(uint16_t value);

int32_t vm_length(const char* text);
const char* vm_name(void);
const char* vm_nothing(void);
void vm_divide(int32_t a, int32_t b, int32_t* quotient, int32_t* remainder);

// One more each call: state of the library's own, which one copy of it keeps.
int32_t vm_counter(void);

// Pointers: values C reads and writes through, a struct, text the caller frees, and a pointer
// C sets.
typedef struct vm_pair {
    int32_t a;
    int64_t b;
    int8_t c;
} vm_pair;

int32_t vm_sum(const int32_t* values, int32_t count);
void vm_fill(int32_t* values, int32_t count, int32_t value);
int64_t vm_pair_total(const vm_pair* pair);
void vm_pair_set(vm_pair* pair, int32_t a, int64_t b, int8_t c);
int64_t vm_pair_size(void);
// A copy of `text` in capitals, which vm_free_text gives back, or NULL without the memory.
char* vm_upper_copy(const char* text);
void vm_free_text(char* text);
int32_t vm_numbers(const int32_t** numbers);

// Structs by value, which each C ABI passes its own way, by their size and what they hold.
typedef struct vm_tiny {
    int8_t a;
    int8_t b;
} vm_tiny;

typedef struct vm_point {
    int32_t x;
    int32_t y;
} vm_point;

typedef struct vm_vec3 {
    float x;
    float y;
    float z;
} vm_vec3;

typedef struct vm_span {
    int64_t start;
    double length;
} vm_span;

typedef struct vm_box {
    vm_point min;
    vm_point max;
    bool filled;
} vm_box;

typedef struct vm_odd {
    uint8_t bytes[3];
} vm_odd;

vm_tiny vm_tiny_swap(vm_tiny tiny);
vm_point vm_point_add(vm_point a, vm_point b);
vm_vec3 vm_vec3_scale(vm_vec3 v, float by);
vm_span vm_span_grow(vm_span span, int64_t by);
vm_box vm_box_make(int32_t x, int32_t y, int32_t width, int32_t height, bool filled);
int32_t vm_box_area(vm_box box);
vm_odd vm_odd_rotate(vm_odd odd);
// Enough of them that the last ones go on the stack: SysV has six registers for their
// integers, and Win64 four for the copies' addresses.
double vm_spans(vm_span a, vm_span b, vm_span c, vm_span d, vm_span e, vm_span f, vm_span g,
                int32_t h);

// Callbacks: C calls the program's code, with numbers, a Bool and small integers that the ABI
// may extend, text, and user data.
typedef int32_t (*vm_binary)(int32_t a, int32_t b);
typedef bool (*vm_test)(int8_t value);
typedef void (*vm_visit)(int32_t value, void* user);
typedef double (*vm_measure)(const char* name);

int32_t vm_apply(vm_binary f, int32_t a, int32_t b);
int32_t vm_count_if(const int8_t* values, int32_t count, vm_test test);
void vm_each(const int32_t* values, int32_t count, vm_visit visit, void* user);
double vm_measure_all(vm_measure measure);

// An enum in a struct that C gives by value and sets through a pointer. Its colors are what
// the caller says, which can be no color at all.
typedef enum vm_color { VM_RED = 1, VM_GREEN = 2, VM_BLUE = 3 } vm_color;

typedef struct vm_shade {
    vm_color color;
    int32_t alpha;
    vm_color edges[2];
} vm_shade;

vm_shade vm_shade_make(int32_t color, int32_t alpha);
void vm_shade_edge(vm_shade* shade, int32_t edge);

// A callback that takes an enum, which C passes as the caller says, color or not.
typedef void (*vm_painter)(vm_color color);

void vm_paint(vm_painter paint, int32_t color);

// A callback C gives back, which gives one of the colors, or none.
typedef vm_color (*vm_colorer)(void);

vm_colorer vm_colorer_of(bool valid);

// A callback in a struct passed by value.
typedef struct vm_handler {
    vm_binary f;
    int32_t value;
} vm_handler;

int32_t vm_handle(vm_handler handler, int32_t by);

// Whether a callback is vm_add itself, as C compares its own function pointers.
bool vm_is_add(vm_binary f);

// Constants a program can name: numbers, and an enum without a name.
#define VM_VERSION 3
#define VM_LIMIT (1 << 4)
#define VM_HALF 0.5
enum { VM_SLOTS = 4 };

// An opaque handle: C has the bag's fields, and a program only a pointer to it.
typedef struct vm_bag vm_bag;

// A new bag, which vm_bag_free gives back, or NULL without the memory. The others take a bag
// vm_bag_new gave.
vm_bag* vm_bag_new(void);
void vm_bag_add(vm_bag* bag, int32_t value);
int32_t vm_bag_total(const vm_bag* bag);
void vm_bag_free(vm_bag* bag);

#endif
