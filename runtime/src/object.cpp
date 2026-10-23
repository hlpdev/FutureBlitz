#include "fb/runtime.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Objects of Classes: allocating them, counting their owners, destroying them, and the leak
// report, which debug builds extend with the cycles that kept objects alive.

namespace {

/// The objects that exist, while debug builds keep track of them, each with the order it was
/// made in, so a report comes out the same every time.
struct registry_t {
    bool tracking = false;
    std::mutex mutex;
    std::uint64_t made = 0;
    std::unordered_map<const fb_object_t*, std::uint64_t> objects;
};

registry_t& registry() {
    static registry_t instance;
    return instance;
}

void remember(const fb_object_t* object) {
    registry_t& known = registry();
    if (known.tracking) {
        const std::scoped_lock lock(known.mutex);
        known.objects.emplace(object, known.made++);
    }
}

void forget(const fb_object_t* object) {
    registry_t& known = registry();
    if (known.tracking) {
        const std::scoped_lock lock(known.mutex);
        known.objects.erase(object);
    }
}

/// What this thread is letting go of: whether it's releasing an object's fields, and the objects
/// whose last owner let go meanwhile, which wait for it to finish, so a long chain of objects
/// goes one at a time rather than through a call for each.
struct letting_go_t {
    bool releasing = false;
    std::vector<fb_object_t*> waiting;
};

thread_local letting_go_t t_letting_go;

/// Releases an object's fields, and any object that loses its last owner by it waits.
void release_fields(fb_object_t* object) {
    letting_go_t& state = t_letting_go;
    const bool releasing = std::exchange(state.releasing, true);
    object->type->release(object);
    state.releasing = releasing;
}

/// The object is gone: Weak references to it read as Null, and its fields are released. Its
/// memory stays while Weak references to it remain, and the last of them frees it.
void finish(fb_object_t* object) {
    object->refcount = 0;
    ++object->weak_count; // keeps the memory while the fields go, even if they held the last Weak
    release_fields(object);
    // ReSharper disable once CppExpressionWithoutSideEffects
    std::atomic_ref(object->type->live).fetch_sub(1, std::memory_order_relaxed);
    if (--object->weak_count == 0) {
        std::free(object);
    }
}

/// What a Shared Class's object counts while its Destroy() runs: so far above any real count
/// that a Weak read on another thread sees it's going, and Destroy()'s own retains and releases
/// never bring it to 0.
constexpr std::int32_t dying = 0x40000000;

[[nodiscard]] bool is_shared(const fb_object_t* object) {
    return object->type->shared != 0;
}

/// A Shared Class's object is gone. It holds a Weak reference to itself while it lives, which
/// keeps its memory while its fields go, as other threads' Weak references let go at any time.
void finish_shared(fb_object_t* object) {
    std::atomic_ref(object->refcount).store(0, std::memory_order_release);
    release_fields(object);
    // ReSharper disable once CppExpressionWithoutSideEffects
    std::atomic_ref(object->type->live).fetch_sub(1, std::memory_order_relaxed);
    if (std::atomic_ref(object->weak_count).fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::free(object);
    }
}

/// Like destroy(), for a Shared Class's object, which another thread may read through a Weak
/// reference meanwhile.
void destroy_shared(fb_object_t* object) {
    forget(object);
    if (object->type->destroy != nullptr) {
        const std::atomic_ref refcount(object->refcount);
        refcount.store(dying + 1, std::memory_order_relaxed);
        object->type->destroy(object);
        if (refcount.load(std::memory_order_acquire) != dying + 1) {
            const std::string message =
                std::string(object->type->name) + "'s Destroy() kept a reference to its object";
            fb_panic(message.c_str(), nullptr);
        }
    }

    finish_shared(object);
}

/// The last owner let go: Destroy() runs, if the object's Class or one it extends has one, then
/// the object is finished. During Destroy() the object counts one owner, so Self can be used
/// normally. If it has more afterwards, Destroy() stored Self somewhere, and that reference would
/// dangle.
void destroy(fb_object_t* object) {
    forget(object);
    if (object->type->destroy != nullptr) {
        object->refcount = 1;
        object->type->destroy(object);
        if (object->refcount != 1) {
            const std::string message =
                std::string(object->type->name) + "'s Destroy() kept a reference to its object";
            fb_panic(message.c_str(), nullptr);
        }
    }

    finish(object);
}

/// Destroys an object whose last owner let go, or without its Destroy() when it's discarded.
void end(fb_object_t* object, const bool discard) {
    if (!discard) {
        if (is_shared(object)) {
            destroy_shared(object);
        } else {
            destroy(object);
        }

        return;
    }

    forget(object);
    if (is_shared(object)) {
        finish_shared(object);
    } else {
        finish(object);
    }
}

/// Ends an object whose last owner let go, then each object that lost its last owner as its
/// fields were released, and theirs, one at a time. While another object's fields are being
/// released, the object waits for its turn, holding a Weak reference to itself so its memory
/// stays even if the last real one goes first. What an object let go of goes in the order it let
/// go of it, and before what was waiting already, so Destroy()s run in the order calls of their
/// own would run them.
void let_go(fb_object_t* object, const bool discard) {
    letting_go_t& state = t_letting_go;
    if (state.releasing) {
        // A Shared Class's object holds one already.
        if (!is_shared(object)) {
            ++object->weak_count;
        }

        state.waiting.push_back(object);
        return;
    }

    // A Destroy() that lets go of an object goes over what that let go of itself.
    const std::size_t start = state.waiting.size();
    end(object, discard);
    std::reverse(std::next(state.waiting.begin(), static_cast<std::ptrdiff_t>(start)),
                 state.waiting.end());
    while (state.waiting.size() > start) {
        fb_object_t* next = state.waiting.back();
        state.waiting.pop_back();
        // Its turn: finishing it keeps its memory from here on.
        if (!is_shared(next)) {
            --next->weak_count;
        }

        const std::size_t before = state.waiting.size();
        end(next, false);
        std::reverse(std::next(state.waiting.begin(), static_cast<std::ptrdiff_t>(before)),
                     state.waiting.end());
    }
}

/// The words of a type's name, between the punctuation that spells its kind: "MapEntry",
/// "String", and "Node" in "MapEntry<String, Node>[]".
std::vector<std::string_view> words_of(const std::string_view name) {
    constexpr std::string_view punctuation = " <>,()[]?:";
    std::vector<std::string_view> words;
    std::size_t start = 0;
    while (start < name.size()) {
        const std::size_t end = std::min(name.find_first_of(punctuation, start), name.size());
        if (end > start) {
            words.push_back(name.substr(start, end - start));
        }

        start = end + 1;
    }

    return words;
}

/// One Struct, Class, Interface, or Enum that a type is or is made of: its name, and that name
/// with its module.
struct declared_part_t {
    std::string_view name;
    std::string_view full_name;
};

/// The declarations a Class or Interface is or is made of, as sema finds them: the Map and Node
/// of "Map<String, Node>". A full name spells the name's words in order, with each declared
/// one's module first, so those are the words that differ.
template <typename Type>
std::vector<declared_part_t> declared_parts(const Type* type) {
    const std::vector<std::string_view> words = words_of(type->name);
    const std::vector<std::string_view> full_words = words_of(type->full_name);
    std::vector<declared_part_t> parts;
    if (words.size() != full_words.size()) {
        return parts;
    }

    for (std::size_t i = 0; i < words.size(); ++i) {
        if (words[i] != full_words[i]) {
            parts.push_back({ .name = words[i], .full_name = full_words[i] });
        }
    }

    return parts;
}

/// Whether a message would show two types alike, as sema decides: they have the same name but
/// are different types, or they are, or are made of, two declarations of one name, whatever
/// their type arguments, like a program's own Map and std's Map<String, Int32>, or a
/// MapEntry<String, Map>[] of the program's Map beside std's Map.
template <typename Type, typename Other>
bool alike(const Type* type, const Other* other) {
    if (static_cast<const void*>(type) == static_cast<const void*>(other) ||
        type->full_name == nullptr || other->full_name == nullptr) {
        return false;
    }

    if (std::string_view(type->name) == other->name) {
        return std::string_view(type->full_name) != other->full_name;
    }

    const std::vector<declared_part_t> theirs = declared_parts(other);
    return std::ranges::any_of(declared_parts(type), [&](const declared_part_t& part) {
        return std::ranges::any_of(theirs, [&](const declared_part_t& other_part) {
            return part.name == other_part.name && part.full_name != other_part.full_name;
        });
    });
}

/// How a message that names `type` among `others`, Classes or Interfaces, shows it: by its
/// name, or by its full name when it's alike one of the others.
template <typename Type, typename Other>
const char* shown_name(const Type* type, std::span<const Other* const> others) {
    for (const Other* other : others) {
        if (alike(type, other)) {
            return type->full_name;
        }
    }

    return type->name;
}

/// The same, for a message that names two.
template <typename Type, typename Other>
const char* shown_name(const Type* type, const Other* other) {
    return shown_name(type, std::span<const Other* const>(&other, 1));
}

/// One strong reference among the objects left at the end.
struct edge_t {
    std::size_t target;
    /// The field it's in, or null for an element of a T[].
    const char* field;
};

/// The objects left at the end, in the order they were made, and the references among them.
struct leftovers_t {
    std::vector<const fb_object_t*> objects;
    std::unordered_map<const fb_object_t*, std::size_t> positions;
    std::vector<std::vector<edge_t>> edges;
};

/// What a visit records the references of one object in.
struct visit_t {
    leftovers_t* leftovers;
    std::size_t from;
};

void record(void* context, const fb_object_t* target, const char* field) {
    auto* visit = static_cast<visit_t*>(context);
    if (target == nullptr) {
        return;
    }

    if (const auto found = visit->leftovers->positions.find(target);
        found != visit->leftovers->positions.end()) {
        visit->leftovers->edges[visit->from].push_back({ .target = found->second, .field = field });
    }
}

leftovers_t leftovers() {
    registry_t& known = registry();
    const std::scoped_lock lock(known.mutex);
    std::vector<std::pair<std::uint64_t, const fb_object_t*>> made;
    made.reserve(known.objects.size());
    for (const auto& [object, order] : known.objects) {
        made.emplace_back(order, object);
    }

    std::ranges::sort(made);
    leftovers_t result;
    for (const auto& [order, object] : made) {
        result.positions.emplace(object, result.objects.size());
        result.objects.push_back(object);
    }

    result.edges.resize(result.objects.size());
    for (std::size_t i = 0; i < result.objects.size(); ++i) {
        visit_t visit{ .leftovers = &result, .from = i };
        result.objects[i]->type->visit(result.objects[i], record, &visit);
    }

    return result;
}

/// The groups of objects that can each reach the others: Tarjan's strongly connected components,
/// without recursion, so a long chain of objects can't overflow the stack. Each group's objects
/// are in the order they were made.
std::vector<std::vector<std::size_t>> groups_of(const leftovers_t& left) {
    constexpr std::size_t unseen = std::numeric_limits<std::size_t>::max();
    const std::size_t count = left.objects.size();
    std::vector<std::size_t> order(count, unseen);
    std::vector<std::size_t> low(count, 0);
    std::vector<bool> stacked(count, false);
    std::vector<std::size_t> stack;
    std::vector<std::vector<std::size_t>> groups;
    std::size_t next = 0;
    const auto reach = [&](const std::size_t object) {
        order[object] = next;
        low[object] = next;
        ++next;
        stack.push_back(object);
        stacked[object] = true;
    };

    for (std::size_t root = 0; root < count; ++root) {
        if (order[root] != unseen) {
            continue;
        }

        // Each object on the path, with how many of its references the search has followed.
        std::vector<std::pair<std::size_t, std::size_t>> path{ { root, 0 } };
        reach(root);
        while (!path.empty()) {
            const std::size_t object = path.back().first;
            const std::size_t followed = path.back().second;
            if (followed < left.edges[object].size()) {
                ++path.back().second;
                const std::size_t target = left.edges[object][followed].target;
                if (order[target] == unseen) {
                    reach(target);
                    path.emplace_back(target, 0);
                } else if (stacked[target]) {
                    low[object] = std::min(low[object], order[target]);
                }

                continue;
            }

            path.pop_back();
            if (!path.empty()) {
                low[path.back().first] = std::min(low[path.back().first], low[object]);
            }

            if (low[object] != order[object]) {
                continue;
            }

            std::vector<std::size_t> group;
            while (true) {
                const std::size_t member = stack.back();
                stack.pop_back();
                stacked[member] = false;
                group.push_back(member);
                if (member == object) {
                    break;
                }
            }

            std::ranges::sort(group);
            groups.push_back(std::move(group));
        }
    }

    std::ranges::sort(groups, {}, [](const std::vector<std::size_t>& group) { return group[0]; });
    return groups;
}

/// The shortest way from a group's first object around to itself, as each object on the way and
/// the field it follows. Empty for a group of one that doesn't refer to itself.
std::vector<std::pair<std::size_t, const char*>> cycle_of(const leftovers_t& left,
                                                          const std::vector<std::size_t>& group) {
    const std::size_t start = group.front();
    const auto in_group = [&](const std::size_t object) {
        return std::ranges::binary_search(group, object);
    };

    // How the search reached each object: the one before it, and the field.
    std::unordered_map<std::size_t, std::pair<std::size_t, const char*>> reached;
    std::deque<std::size_t> queue{ start };
    while (!queue.empty()) {
        const std::size_t object = queue.front();
        queue.pop_front();
        for (const edge_t& edge : left.edges[object]) {
            if (edge.target == start) {
                std::vector<std::pair<std::size_t, const char*>> cycle{ { object, edge.field } };
                for (std::size_t current = object; current != start;) {
                    const auto [before, field] = reached.at(current);
                    cycle.emplace_back(before, field);
                    current = before;
                }

                std::ranges::reverse(cycle);
                return cycle;
            }

            if (in_group(edge.target) && !reached.contains(edge.target)) {
                reached.emplace(edge.target, std::pair(object, edge.field));
                queue.push_back(edge.target);
            }
        }
    }

    return {};
}

/// "leak: a cycle of 2 objects: Node.next -> Node.next -> the first Node", for each group of
/// objects left at the end that keep each other alive. Objects that only a cycle keeps alive are
/// in the counts already. The report is one message, so each Class is shown as the counts show
/// it among the `leaked` ones.
void report_cycles(const std::span<const fb_class_t* const> leaked) {
    constexpr std::size_t most = 5;
    const leftovers_t left = leftovers();
    std::size_t cycles = 0;
    for (const std::vector<std::size_t>& group : groups_of(left)) {
        const std::vector<std::pair<std::size_t, const char*>> cycle = cycle_of(left, group);
        if (cycle.empty()) {
            continue;
        }

        if (++cycles > most) {
            continue;
        }

        std::string path;
        for (const auto& [object, field] : cycle) {
            const char* name = shown_name(left.objects[object]->type, leaked);
            path +=
                field != nullptr ? std::string(name) + "." + field : std::string(name) + " element";
            path += " -> ";
        }

        std::fprintf(stderr, "leak: a cycle of %llu object%s: %sthe first %s\n",
                     static_cast<unsigned long long>(cycle.size()), cycle.size() == 1 ? "" : "s",
                     path.c_str(), shown_name(left.objects[cycle.front().first]->type, leaked));
    }

    if (cycles > most) {
        std::fprintf(stderr, "leak: and %llu more cycles\n",
                     static_cast<unsigned long long>(cycles - most));
    }
}

/// The table an object's Class has for an Interface, or null when it doesn't implement it.
void* const* find_implementation(const fb_object_t* object, const fb_interface_t* interface) {
    if (object == nullptr) {
        return nullptr;
    }

    const fb_class_t* type = object->type;
    for (const fb_implementation_t& implementation :
         std::span(type->interfaces, static_cast<std::size_t>(type->interface_count))) {
        if (implementation.interface == interface) {
            return implementation.methods;
        }
    }

    return nullptr;
}

} // namespace

extern "C" {

void fb_track_objects() {
    registry().tracking = true;
}

void fb_finish(fb_class_t* const* classes, const std::int64_t count) {
    fb_scheduler_end();
    fb_pool_finish();
    // The I/O thread stops first, letting go of the Tasks it has that nothing waits for.
    fb::runtime::run_finish();
    // Whatever the program printed shows up before the report.
    std::fflush(stdout);
    if (const std::int64_t strings = fb_string_live_count(); strings != 0) {
        std::fprintf(stderr, "leak: %lld Strings were never released\n",
                     static_cast<long long>(strings));
    }

    if (const std::int64_t copies = fb_cstring_live_count(); copies != 0) {
        std::fprintf(stderr, "leak: %lld CString copies of Strings were never freed\n",
                     static_cast<long long>(copies));
    }

    // The report shows the leaked Classes by their names, or by their full names where two are
    // alike, in the counts and the cycles both.
    std::vector<const fb_class_t*> leaked;
    for (const fb_class_t* type : std::span(classes, static_cast<std::size_t>(count))) {
        if (type->live != 0) {
            leaked.push_back(type);
        }
    }

    for (const fb_class_t* type : leaked) {
        std::fprintf(stderr, "leak: %lld %s objects were never released\n",
                     static_cast<long long>(type->live),
                     shown_name(type, std::span<const fb_class_t* const>(leaked)));
    }

    if (!leaked.empty() && registry().tracking) {
        report_cycles(leaked);
    }
}

fb_object_t* fb_object_new(fb_class_t* type) {
    auto* object = static_cast<fb_object_t*>(std::calloc(1, static_cast<std::size_t>(type->size)));
    if (object == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    object->refcount = 1;
    object->type = type;
    // A Shared Class's object holds a Weak reference to itself while it lives.
    object->weak_count = type->shared != 0 ? 1 : 0;
    remember(object);

    // Objects of an ordinary Class can be made on any thread, even if they stay on it.
    // ReSharper disable once CppExpressionWithoutSideEffects
    std::atomic_ref(type->live).fetch_add(1, std::memory_order_relaxed);
    return object;
}

void fb_object_retain(fb_object_t* object) {
    if (object == nullptr) {
        return;
    }

    if (is_shared(object)) {
        // ReSharper disable once CppExpressionWithoutSideEffects
        std::atomic_ref(object->refcount).fetch_add(1, std::memory_order_relaxed);
    } else {
        ++object->refcount;
    }
}

void fb_object_release(fb_object_t* object) {
    if (object == nullptr) {
        return;
    }

    // Whichever thread lets go of a Shared Class's object last destroys it, after every other
    // owner's last use.
    if (is_shared(object)) {
        if (std::atomic_ref(object->refcount).fetch_sub(1, std::memory_order_acq_rel) == 1) {
            let_go(object, false);
        }

        return;
    }

    --object->refcount;
    if (object->refcount == 0) {
        let_go(object, false);
    }
}

void fb_object_retain_weak(fb_object_t* object) {
    if (object == nullptr) {
        return;
    }

    if (is_shared(object)) {
        // ReSharper disable once CppExpressionWithoutSideEffects
        std::atomic_ref(object->weak_count).fetch_add(1, std::memory_order_relaxed);
    } else {
        ++object->weak_count;
    }
}

void fb_object_release_weak(fb_object_t* object) {
    if (object == nullptr) {
        return;
    }

    // A Shared Class's object holds a Weak reference to itself while it lives, so the last one
    // comes once it's gone.
    if (is_shared(object)) {
        if (std::atomic_ref(object->weak_count).fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::free(object);
        }

        return;
    }

    // Once the object is gone, its memory only waits for the Weak references.
    --object->weak_count;
    if (object->weak_count == 0 && object->refcount == 0) {
        std::free(object);
    }
}

fb_object_t* fb_object_load_weak(fb_object_t* object) {
    if (object == nullptr) {
        return nullptr;
    }

    // Another thread may let go of a Shared Class's object meanwhile, so it only gets an owner
    // while it has one.
    if (is_shared(object)) {
        const std::atomic_ref refcount(object->refcount);
        std::int32_t seen = refcount.load(std::memory_order_relaxed);
        do {
            if (seen <= 0 || seen >= dying) {
                return nullptr;
            }
        } while (!refcount.compare_exchange_weak(seen, seen + 1, std::memory_order_acquire,
                                                 std::memory_order_relaxed));
        return object;
    }

    if (object->refcount == 0) {
        return nullptr;
    }

    ++object->refcount;
    return object;
}

void fb_object_discard(fb_object_t* object) {
    // A New that gave Self to something that keeps it leaves the object there, and its Destroy
    // runs when that lets go.
    if (is_shared(object)) {
        if (std::atomic_ref(object->refcount).fetch_sub(1, std::memory_order_acq_rel) == 1) {
            let_go(object, true);
        }

        return;
    }

    --object->refcount;
    if (object->refcount == 0) {
        let_go(object, true);
    }
}

std::int32_t fb_object_is(const fb_object_t* object, const fb_class_t* type) {
    if (object == nullptr) {
        return 0;
    }

    for (const fb_class_t* current = object->type; current != nullptr; current = current->base) {
        if (current == type) {
            return 1;
        }
    }

    return 0;
}

void fb_object_expect(const fb_object_t* object, const fb_class_t* type,
                      const fb_location_t* location) {
    if (fb_object_is(object, type) != 0) {
        return;
    }

    const std::string message = std::string("expected ") +
                                (object != nullptr ? shown_name(type, object->type) : type->name) +
                                ", found " +
                                (object != nullptr ? shown_name(object->type, type) : "Null");
    fb_panic(message.c_str(), location);
}

std::uint64_t fb_hash_object(const fb_object_t* object) {
    return fb_hash_integer(reinterpret_cast<std::uintptr_t>(object));
}

void* const* fb_interface_methods(const fb_object_t* object, const fb_interface_t* interface) {
    return find_implementation(object, interface);
}

std::int32_t fb_object_implements(const fb_object_t* object, const fb_interface_t* interface) {
    return find_implementation(object, interface) != nullptr ? 1 : 0;
}

void fb_object_expect_implements(const fb_object_t* object, const fb_interface_t* interface,
                                 const fb_location_t* location) {
    if (find_implementation(object, interface) != nullptr) {
        return;
    }

    const std::string message =
        std::string("expected ") +
        (object != nullptr ? shown_name(interface, object->type) : interface->name) + ", found " +
        (object != nullptr ? shown_name(object->type, interface) : "Null");
    fb_panic(message.c_str(), location);
}
}
