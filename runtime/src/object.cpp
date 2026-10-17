#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <span>
#include <string>
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

/// The object is gone: Weak references to it read as Null, and its fields are released. Its
/// memory stays while Weak references to it remain, and the last of them frees it.
void finish(fb_object_t* object) {
    object->refcount = 0;
    ++object->weak_count; // keeps the memory while the fields go, even if they held the last Weak
    object->type->release(object);
    // ReSharper disable once CppExpressionWithoutSideEffects
    std::atomic_ref(object->type->live).fetch_sub(1, std::memory_order_relaxed);
    if (--object->weak_count == 0) {
        std::free(object);
    }
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
            std::array<char, 256> message{};
            std::snprintf(message.data(), message.size(),
                          "%s's Destroy() kept a reference to its object", object->type->name);
            fb_panic(message.data(), nullptr);
        }
    }

    finish(object);
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
/// in the counts already.
void report_cycles() {
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
            const char* name = left.objects[object]->type->name;
            path +=
                field != nullptr ? std::string(name) + "." + field : std::string(name) + " element";
            path += " -> ";
        }

        std::fprintf(stderr, "leak: a cycle of %llu object%s: %sthe first %s\n",
                     static_cast<unsigned long long>(cycle.size()), cycle.size() == 1 ? "" : "s",
                     path.c_str(), left.objects[cycle.front().first]->type->name);
    }

    if (cycles > most) {
        std::fprintf(stderr, "leak: and %llu more cycles\n",
                     static_cast<unsigned long long>(cycles - most));
    }
}

} // namespace

extern "C" {

void fb_track_objects() {
    registry().tracking = true;
}

void fb_finish(fb_class_t* const* classes, const std::int64_t count) {
    // Whatever the program printed shows up before the report.
    std::fflush(stdout);
    if (const std::int64_t strings = fb_string_live_count(); strings != 0) {
        std::fprintf(stderr, "leak: %lld Strings were never released\n",
                     static_cast<long long>(strings));
    }

    bool leaked = false;
    for (const fb_class_t* type : std::span(classes, static_cast<std::size_t>(count))) {
        if (type->live != 0) {
            std::fprintf(stderr, "leak: %lld %s objects were never released\n",
                         static_cast<long long>(type->live), type->name);
            leaked = true;
        }
    }

    if (leaked && registry().tracking) {
        report_cycles();
    }
}

fb_object_t* fb_object_new(fb_class_t* type) {
    auto* object = static_cast<fb_object_t*>(std::calloc(1, static_cast<std::size_t>(type->size)));
    if (object == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    object->refcount = 1;
    object->type = type;
    remember(object);

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

void fb_object_retain_weak(fb_object_t* object) {
    if (object != nullptr) {
        ++object->weak_count;
    }
}

void fb_object_release_weak(fb_object_t* object) {
    if (object == nullptr) {
        return;
    }

    // Once the object is gone, its memory only waits for the Weak references.
    --object->weak_count;
    if (object->weak_count == 0 && object->refcount == 0) {
        std::free(object);
    }
}

fb_object_t* fb_object_load_weak(fb_object_t* object) {
    if (object == nullptr || object->refcount == 0) {
        return nullptr;
    }

    ++object->refcount;
    return object;
}

void fb_object_discard(fb_object_t* object) {
    // A New that gave Self to something that keeps it leaves the object there, and its Destroy
    // runs when that lets go.
    --object->refcount;
    if (object->refcount != 0) {
        return;
    }

    forget(object);
    finish(object);
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

    std::array<char, 256> message{};
    std::snprintf(message.data(), message.size(), "expected %s, found %s", type->name,
                  object != nullptr ? object->type->name : "Null");
    fb_panic(message.data(), location);
}
}
