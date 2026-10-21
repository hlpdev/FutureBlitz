#include "vecmath.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int32_t vm_add(int32_t a, int32_t b) {
    return a + b;
}

int64_t vm_add64(int64_t a, int64_t b) {
    return a + b;
}

double vm_scale(double value, double by) {
    return value * by;
}

float vm_halve(float value) {
    return value / 2.0f;
}

bool vm_is_even(int32_t value) {
    return value % 2 == 0;
}

int32_t vm_count(bool a, bool b, bool c) {
    return (int32_t)a + (int32_t)b + (int32_t)c;
}

int8_t vm_low8(int32_t value) {
    return (int8_t)value;
}

uint8_t vm_low_u8(int32_t value) {
    return (uint8_t)value;
}

int16_t vm_low16(int32_t value) {
    return (int16_t)value;
}

int32_t vm_widen8(int8_t value) {
    return value;
}

uint32_t vm_widen_u16(uint16_t value) {
    return value;
}

int32_t vm_length(const char* text) {
    return (int32_t)strlen(text);
}

const char* vm_name(void) {
    return "vecmath";
}

const char* vm_nothing(void) {
    return NULL;
}

void vm_divide(int32_t a, int32_t b, int32_t* quotient, int32_t* remainder) {
    *quotient = a / b;
    *remainder = a % b;
}

int32_t vm_counter(void) {
    static int32_t count = 0;
    return ++count;
}

int32_t vm_sum(const int32_t* values, int32_t count) {
    int32_t total = 0;
    for (int32_t i = 0; i < count; ++i) {
        total += values[i];
    }

    return total;
}

void vm_fill(int32_t* values, int32_t count, int32_t value) {
    for (int32_t i = 0; i < count; ++i) {
        values[i] = value;
    }
}

int64_t vm_pair_total(const vm_pair* pair) {
    return pair->a + pair->b + pair->c;
}

void vm_pair_set(vm_pair* pair, int32_t a, int64_t b, int8_t c) {
    pair->a = a;
    pair->b = b;
    pair->c = c;
}

int64_t vm_pair_size(void) {
    return (int64_t)sizeof(vm_pair);
}

char* vm_upper_copy(const char* text) {
    const size_t length = strlen(text);
    char* copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }

    for (size_t i = 0; i <= length; ++i) {
        copy[i] = (char)toupper((unsigned char)text[i]);
    }

    return copy;
}

void vm_free_text(char* text) {
    free(text);
}

int32_t vm_numbers(const int32_t** numbers) {
    static const int32_t values[] = { 3, 1, 4, 1, 5 };
    *numbers = values;
    return 5;
}

vm_tiny vm_tiny_swap(vm_tiny tiny) {
    const vm_tiny swapped = { tiny.b, tiny.a };
    return swapped;
}

vm_point vm_point_add(vm_point a, vm_point b) {
    const vm_point sum = { a.x + b.x, a.y + b.y };
    return sum;
}

vm_vec3 vm_vec3_scale(vm_vec3 v, float by) {
    const vm_vec3 scaled = { v.x * by, v.y * by, v.z * by };
    return scaled;
}

vm_span vm_span_grow(vm_span span, int64_t by) {
    const vm_span grown = { span.start + by, span.length + (double)by };
    return grown;
}

vm_box vm_box_make(int32_t x, int32_t y, int32_t width, int32_t height, bool filled) {
    const vm_box box = { { x, y }, { x + width, y + height }, filled };
    return box;
}

int32_t vm_box_area(vm_box box) {
    const int32_t area = (box.max.x - box.min.x) * (box.max.y - box.min.y);
    return box.filled ? area : -area;
}

vm_odd vm_odd_rotate(vm_odd odd) {
    const vm_odd rotated = { { odd.bytes[1], odd.bytes[2], odd.bytes[0] } };
    return rotated;
}

double vm_spans(vm_span a, vm_span b, vm_span c, vm_span d, vm_span e, vm_span f, vm_span g,
                int32_t h) {
    return (double)(a.start + b.start + c.start + d.start + e.start + f.start + g.start) +
           a.length + b.length + c.length + d.length + e.length + f.length + g.length + h;
}

int32_t vm_apply(vm_binary f, int32_t a, int32_t b) {
    return f(a, b);
}

int32_t vm_count_if(const int8_t* values, int32_t count, vm_test test) {
    int32_t found = 0;
    for (int32_t i = 0; i < count; ++i) {
        found += test(values[i]) ? 1 : 0;
    }

    return found;
}

void vm_each(const int32_t* values, int32_t count, vm_visit visit, void* user) {
    for (int32_t i = 0; i < count; ++i) {
        visit(values[i], user);
    }
}

double vm_measure_all(vm_measure measure) {
    return measure("a") + measure("bb") + measure("ccc");
}

vm_shade vm_shade_make(int32_t color, int32_t alpha) {
    const vm_shade shade = { (vm_color)color, alpha, { (vm_color)color, (vm_color)color } };
    return shade;
}

void vm_shade_edge(vm_shade* shade, int32_t edge) {
    shade->edges[1] = (vm_color)edge;
}

void vm_paint(vm_painter paint, int32_t color) {
    paint((vm_color)color);
}

static vm_color vm_green(void) {
    return VM_GREEN;
}

static vm_color vm_no_color(void) {
    return (vm_color)9;
}

vm_colorer vm_colorer_of(bool valid) {
    return valid ? vm_green : vm_no_color;
}

int32_t vm_handle(vm_handler handler, int32_t by) {
    return handler.f(handler.value, by);
}

bool vm_is_add(vm_binary f) {
    return f == vm_add;
}

struct vm_bag {
    int32_t total;
};

vm_bag* vm_bag_new(void) {
    return calloc(1, sizeof(vm_bag));
}

void vm_bag_add(vm_bag* bag, int32_t value) {
    bag->total += value;
}

int32_t vm_bag_total(const vm_bag* bag) {
    return bag->total;
}

void vm_bag_free(vm_bag* bag) {
    free(bag);
}
