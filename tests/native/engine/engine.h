#ifndef ENGINE_H
#define ENGINE_H

// A stand-in for the engine: a C++ library in a DLL of its own, which FutureBlitz programs use
// through this C API, as they will the real one. tests/packages/engine wraps it, through the
// bindings fb-bindgen makes of this header. Every type here is the same size on every target,
// so the bindings are too.

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32) && defined(ENGINE_BUILDING)
#define ENGINE_API __declspec(dllexport)
#elif defined(_WIN32)
#define ENGINE_API __declspec(dllimport)
#else
#define ENGINE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ENGINE_VERSION    1
#define ENGINE_MAX_BODIES 64

typedef struct engine_vec2 {
    float x;
    float y;
} engine_vec2;

typedef enum engine_shape { ENGINE_CIRCLE = 1, ENGINE_BOX = 2 } engine_shape;

// A world of bodies, which only the engine looks inside.
typedef struct engine_world engine_world;

// Called for each body, with the user data engine_each was given.
typedef void (*engine_visit)(int32_t body, float x, float y, void* user);

ENGINE_API const char* engine_version(void);

// A new world, which engine_world_free gives back, or NULL without the memory.
ENGINE_API engine_world* engine_world_new(const char* name);
ENGINE_API void engine_world_free(engine_world* world);
ENGINE_API const char* engine_world_name(const engine_world* world);
// How many worlds there are, which one copy of the engine counts.
ENGINE_API int32_t engine_worlds(void);

// A new body's number, or -1 when the world is full, the shape isn't one, or the position is
// outside the walls, which engine_error says.
ENGINE_API int32_t engine_spawn(engine_world* world, engine_shape shape, engine_vec2 position,
                                engine_vec2 velocity);
ENGINE_API int32_t engine_count(const engine_world* world);
// Moves every body by its velocity over `seconds`, bouncing off the world's walls at 0 and 100
// as many times as it reaches them.
ENGINE_API void engine_step(engine_world* world, float seconds);
// Where a body is, or false when there's no such body, which engine_error says.
ENGINE_API bool engine_position(const engine_world* world, int32_t body, engine_vec2* position);
// A body's shape, or false when there's no such body, which engine_error says.
ENGINE_API bool engine_shape_of(const engine_world* world, int32_t body, engine_shape* shape);
ENGINE_API engine_vec2 engine_center(const engine_world* world);
ENGINE_API void engine_each(const engine_world* world, engine_visit visit, void* user);

// Why this thread's last call of a function that can fail, engine_world_new, engine_spawn,
// engine_position, or engine_shape_of, failed, or "" when it didn't.
ENGINE_API const char* engine_error(void);

#ifdef __cplusplus
}
#endif

#endif
