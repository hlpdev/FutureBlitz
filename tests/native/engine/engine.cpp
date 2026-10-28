#include "engine.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The engine is C++ inside: classes, the standard library, and exceptions, none of which cross
// its C API.

struct engine_world {
    struct body_t {
        engine_shape shape;
        engine_vec2 position;
        engine_vec2 velocity;
    };

    std::string name;
    std::vector<body_t> bodies;

    [[nodiscard]] const body_t& at(const int32_t body) const {
        if (body < 0 || static_cast<std::size_t>(body) >= bodies.size()) {
            throw std::out_of_range(std::format("{} has no body {}", name, body));
        }

        return bodies[static_cast<std::size_t>(body)];
    }
};

namespace {

constexpr float wall = 100.0F;

std::atomic<int32_t> worlds{ 0 };

thread_local std::string last_error;

/// Runs `call` for a C caller, to whom an exception is `failed`, with engine_error()'s text.
template <typename Result, typename Call>
Result guarded(Call call, Result failed) noexcept {
    try {
        last_error.clear();
        return call();
    } catch (const std::exception& error) {
        last_error = error.what();
    } catch (...) {
        last_error = "an unknown error";
    }

    return failed;
}

/// Moves a coordinate by its speed, bouncing it back off the walls.
void move(float& at, float& speed, const float seconds) {
    at += speed * seconds;
    if (at < 0.0F) {
        at = -at;
        speed = -speed;
    } else if (at > wall) {
        at = (2 * wall) - at;
        speed = -speed;
    }
}

} // namespace

extern "C" {

const char* engine_version(void) {
    return "stand-in engine 1.0";
}

engine_world* engine_world_new(const char* name) {
    return guarded<engine_world*>(
        [&] {
            auto world = std::make_unique<engine_world>();
            world->name = name == nullptr ? "world" : name;
            ++worlds;
            return world.release();
        },
        nullptr);
}

void engine_world_free(engine_world* world) {
    if (world != nullptr) {
        --worlds;
    }

    delete world;
}

const char* engine_world_name(const engine_world* world) {
    return world->name.c_str();
}

int32_t engine_worlds(void) {
    return worlds;
}

int32_t engine_spawn(engine_world* world, const engine_shape shape, const engine_vec2 position,
                     const engine_vec2 velocity) {
    return guarded<int32_t>(
        [&] {
            if (world->bodies.size() >= ENGINE_MAX_BODIES) {
                throw std::length_error(std::format("{} is full", world->name));
            }

            world->bodies.push_back({ .shape = shape, .position = position, .velocity = velocity });
            return static_cast<int32_t>(world->bodies.size() - 1);
        },
        -1);
}

int32_t engine_count(const engine_world* world) {
    return static_cast<int32_t>(world->bodies.size());
}

void engine_step(engine_world* world, const float seconds) {
    for (engine_world::body_t& body : world->bodies) {
        move(body.position.x, body.velocity.x, seconds);
        move(body.position.y, body.velocity.y, seconds);
    }
}

bool engine_position(const engine_world* world, const int32_t body, engine_vec2* position) {
    return guarded<bool>(
        [&] {
            *position = world->at(body).position;
            return true;
        },
        false);
}

engine_shape engine_shape_of(const engine_world* world, const int32_t body) {
    return guarded<engine_shape>([&] { return world->at(body).shape; }, ENGINE_CIRCLE);
}

engine_vec2 engine_center(const engine_world* world) {
    engine_vec2 center{ .x = 0.0F, .y = 0.0F };
    for (const engine_world::body_t& body : world->bodies) {
        center.x += body.position.x;
        center.y += body.position.y;
    }

    const auto count = static_cast<float>(std::max<std::size_t>(world->bodies.size(), 1));
    return { .x = center.x / count, .y = center.y / count };
}

void engine_each(const engine_world* world, const engine_visit visit, void* user) {
    for (std::size_t i = 0; i < world->bodies.size(); ++i) {
        const engine_vec2 at = world->bodies[i].position;
        visit(static_cast<int32_t>(i), at.x, at.y, user);
    }
}

const char* engine_error(void) {
    return last_error.c_str();
}
}
