# The review of M7 step 6 (a package with prebuilt natives)

7 confirmed findings, each with its Status.

## package-1 (medium): World.ShapeOf drops the engine's error: a body that isn't there comes back as Circle

engine_shape_of runs the lookup through guarded(), so for a missing body it sets engine_error() ("<world> has no body N") and returns ENGINE_CIRCLE as its failed value. ShapeOf never reads LastError() and isn't declared Throws EngineError, so a bad body number quietly turns into Shape.Circle. Position, which does the same lookup, throws EngineError for that body. That breaks call (4), which says the engine's exceptions become EngineError, and it is a wrong result. The game doesn't call ShapeOf with a bad body, so no test catches it.

Fix: Declare `Public Method ShapeOf(body: Int32): Shape Throws EngineError`. Check the body first (body < 0 Or body >= Count) or check that LastError() isn't empty after engine_shape_of, then Throw EngineError(LastError()). Add a line to engine_game (and engine_game.out) that calls ShapeOf on a missing body.

Status: fixed: World.ShapeOf Throws EngineError when the engine has no such body; engine_game's ShapeOf(7) prints its error

## engine-1 (low): engine_step reflects a coordinate only once, so bodies leave the 0..100 world and bodies spawned outside it jump inside

move() handles at most one bounce: when |speed * seconds| carries a coordinate more than 100 past a wall, the single reflection leaves it outside [0, 100], and on the next step it keeps going (no further bounce, since the speed was flipped away from the wall). engine_spawn doesn't check the position either, so a body spawned at x = 150 with zero velocity is teleported to 50 by the first step. engine.h promises that engine_step moves every body 'bouncing off the world's walls at 0 and 100'.

Fix: Reflect until the coordinate is inside the walls (a loop, or fold it with fmod over a 2*wall period and flip the speed when the fold count is odd), and have engine_spawn refuse a position outside [0, wall] with a std::out_of_range that guarded turns into -1 and engine_error's text. Add a body that moves more than 100 in one step, and one spawned outside, to engine_game (and its .out).

Status: fixed: a coordinate bounces off the walls as many times as it reaches them, and engine_spawn refuses a position outside them; engine_game spawns a body outside (an error) and one that crosses the world twice in a step, at 90 and then 10

## engine-2 (low): engine_shape_of can't report a missing body: it returns ENGINE_CIRCLE, so World.ShapeOf(7) silently says Circle

engine_shape_of's failure value is ENGINE_CIRCLE, a valid shape, and engine.h says nothing about failure for it (unlike engine_position, documented as false plus engine_error). A caller can't tell 'body 7 is a circle' from 'there is no body 7' without clearing and reading engine_error, which the wrapper can't do reliably (see engine-3). The engine package's World.ShapeOf therefore returns Shape.Circle for any bad body number, while World.Position(7) on the same body throws EngineError('w has no body 7'). An invalid shape passed to engine_spawn is also stored as-is (engine_shape_of returns 7 for it).

Fix: Give engine_shape a value for no shape (e.g. ENGINE_NONE = 0) and return it from engine_shape_of's guarded failure, or change the function to bool engine_shape_of(world, body, engine_shape* shape). Document it in engine.h, regenerate tests/packages/engine/c/engine.fb with fb bindgen, make World.ShapeOf Throws EngineError, and have engine_spawn reject a shape that isn't ENGINE_CIRCLE or ENGINE_BOX. Add ShapeOf on a missing body to engine_game and its .out.

Status: fixed: engine_shape_of is bool engine_shape_of(world, body, shape*), as engine_position is, and engine_spawn refuses a shape that isn't one; the bindings are fb-bindgen's again, and engine_game asks for body 7's shape

## engine-3 (low): engine_error is documented as the last failed call's text, but any successful guarded call clears it, while unguarded calls don't

engine.h says engine_error returns 'What went wrong in this thread's last call that failed, or ""'. In fact guarded() clears last_error at the start of every guarded call (engine_world_new, engine_spawn, engine_position, engine_shape_of), so a successful one of those erases an earlier failure. The other calls (engine_count, engine_step, engine_center, engine_each, engine_world_name, ...) never touch it, so after them the old text stays. engine_error really means 'the last guarded call's failure, if that call failed'.

Fix: Reword engine.h's comment to match the code, for example: 'Why this thread's last call of a function that can fail failed, or "" when it succeeded.' Name which calls can fail (new, spawn, position, shape_of).

Status: fixed: engine.h says engine_error is why the thread's last call of a function that can fail, named, failed, or "" when it didn't (a comment, which nothing runs)

## engine-4 (low): fb_engine's elseif(NOT APPLE) branch gives MSVC the GCC options -static-libstdc++ -static-libgcc

The branch is if(MINGW) ... elseif(NOT APPLE), so an MSVC build (a configuration the project supports: cmake/warnings.cmake, cmake/stack.cmake, the root CMakeLists and fb_vecmath and fb_dyncalc all have MSVC branches) takes the Linux branch. fb_dyncalc's equivalent is if(WIN32) ... elseif(NOT APPLE), which excludes MSVC. The comment above it also covers only MinGW, Linux and macOS.

Fix: Write the branch as elseif(UNIX AND NOT APPLE) (or if(MINGW) ... elseif(NOT MSVC AND NOT APPLE)), and say in the comment that MSVC's runtime needs nothing extra.

Status: fixed: the C++ runtime options go only to UNIX AND NOT APPLE, and the comment says MSVC's and macOS's runtimes are the system's (no MSVC build here to run it)

## engine-5 (low): On Linux, libengine.so exports about 3,400 libstdc++ symbols, and a program can take over some of the engine's own C++ runtime calls

CXX_VISIBILITY_PRESET hidden hides only engine.cpp's own symbols. The libstdc++.a and libgcc objects that -static-libstdc++ -static-libgcc pull in keep default visibility, so the .so exports them. Template code engine.o instantiates from libstdc++'s headers (namespace std is visibility("default")) is exported too. Exported symbols can be preempted, so when the program also links libstdc++ statically (fb's Linux link options are -static-libstdc++ -static-libgcc -pthread), the dynamic linker binds some of the engine's references to the executable's copies. The CMake comment says the engine's 'C++ runtime is linked into it', but part of the C++ runtime it actually runs is the program's. A prebuilt engine and a program built by a different GCC then mix two libstdc++ versions. The engine's API is C only, so it should export only its engine_* functions.

Fix: On Linux, link fb_engine with a version script that exports only engine_* (global: engine_*; local: *;), or at least -Wl,--exclude-libs,ALL plus -fvisibility-inlines-hidden. Keep the CLI test's needs-only-system-libraries check, and add an export-list check on Linux if loaded_libraries' reader can list exports.

Status: fixed: on Linux the engine links with engine.map, a version script that exports only engine_*, and inline functions are hidden everywhere; the CLI test reads the .so's .dynsym on Linux and checks each export is engine_'s, a reader checked by hand in WSL (13 exports with the script, about 3,900 without)

## package-2 (low): world.fb's opening comment says programs can't see the bindings, but they can import them

The comment says c/engine.fb holds "the bindings fb-bindgen made of tests/native/engine/engine.h, which programs don't see". 'Import engine' does leave them out. But c/engine.fb's declarations are Public (call 3), so a program that depends on engine can write 'Import engine.c.engine' and call the raw C API, engine_world_new and engine_world_free included. When the bare 'Import engine' uses one, the compiler's own error points there: "'engine_worlds' is in engine.c.engine, which this file doesn't import". The .fbi also exports module engine/c/engine.fb with all its Public declarations, and a check against the prebuilt package accepts the same import.

Fix: Change the comment to something like "...which `Import engine` leaves out (they're engine.c.engine)". To actually hide them, the bindings would have to be Package rather than Public, which goes against call (3), so fixing the comment is enough.

Status: fixed: world.fb's comment says `Import engine` leaves the bindings out, and that they're engine.c.engine
