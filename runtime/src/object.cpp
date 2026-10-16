#include "fb/runtime.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

// Objects of Classes: allocating them, counting their owners, and destroying them.

namespace {

/// The last owner let go: the Class's destroy runs Destroy() and releases the fields, then the
/// memory is freed. During Destroy() the object counts one owner, so Self can be used normally.
/// If it has more afterwards, Destroy() stored Self somewhere, and that reference would dangle.
void destroy(fb_object_t* object) {
    object->refcount = 1;
    object->type->destroy(object);
    if (object->refcount != 1) {
        std::array<char, 256> message{};
        std::snprintf(message.data(), message.size(),
                      "%s's Destroy() kept a reference to its object", object->type->name);
        fb_panic(message.data(), nullptr);
    }

    // ReSharper disable once CppExpressionWithoutSideEffects
    std::atomic_ref(object->type->live).fetch_sub(1, std::memory_order_relaxed);
    std::free(object);
}

} // namespace

extern "C" {

fb_object_t* fb_object_new(fb_class_t* type) {
    auto* object = static_cast<fb_object_t*>(std::calloc(1, static_cast<std::size_t>(type->size)));
    if (object == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    object->refcount = 1;
    object->type = type;

    // Objects of an ordinary Class can be made on any thread, even if they stay on it.
    // ReSharper disable once CppExpressionWithoutSideEffects
    std::atomic_ref(type->live).fetch_add(1, std::memory_order_relaxed);
    return object;
}

void fb_object_retain(fb_object_t* object) {
    if (object != nullptr) {
        ++object->refcount;
    }
}

void fb_object_release(fb_object_t* object) {
    if (object == nullptr) {
        return;
    }

    --object->refcount;
    if (object->refcount == 0) {
        destroy(object);
    }
}

void fb_object_retain_atomic(fb_object_t* object) {
    if (object != nullptr) {
        // ReSharper disable once CppExpressionWithoutSideEffects
        std::atomic_ref(object->refcount).fetch_add(1, std::memory_order_relaxed);
    }
}

void fb_object_release_atomic(fb_object_t* object) {
    if (object == nullptr) {
        return;
    }

    // Whichever thread lets go last destroys it, after every other owner's last use.
    if (std::atomic_ref(object->refcount).fetch_sub(1, std::memory_order_acq_rel) == 1) {
        destroy(object);
    }
}
}
