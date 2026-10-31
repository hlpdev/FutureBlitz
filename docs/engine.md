# Engine

The engine is phase 2, after phase 1 (M0 to M10): **fbengine**, in its own public repository,
hlpdev/fbengine, under Apache-2.0. This doc is the summary; fbengine's own docs have the rest:
docs/plan.md (the milestones' steps, what each needs, and the lanes), docs/architecture.md (how it
fits together), docs/questions.md (what waits on the user), and docs/research/ (read-throughs of
Blitz3D's and SCP: Containment Breach's source).

The engine is packages built on top of FutureBlitz, never part of the language (the user,
2026-10-30). The language, compiler, runtime, std, and spec depend on none of its features: it
uses what any library can, Extern blocks over its C API, `@MainThread`, and the main scheduler's
`Scheduler.Main.Step()`, and a program uses it by importing its packages.

fbengine is its own engine, not a modern copy of Blitz3D's: it follows std.math (right-handed,
Y up, -Z forward, radians) and loads modern formats only. Its features have to cover everything
SCP: Containment Breach needs, checked at E14, so a 1-to-1 remake can be built on it later; the
remake is a separate project, with its own converters for the game's code and assets, tied to
neither the engine nor the language (the user, 2026-10-31).

## Packages

Released together, at one version (the user, 2026-10-30):

| Package | What's in it | Native code |
|---|---|---|
| `fbengine` | the window, input, the frame loop and `Flip`, timing; 2D; the scene: entities, cameras, lights, geometry, materials, sprites, collisions, picks; animation; the renderer; loading assets | one DLL: SDL3, Vulkan through volk and VMA, fastgltf, KTX |
| `fbengine_audio` | sounds, channels, streams, 3D sound, buses, effects | one DLL: miniaudio |
| `fbengine_physics` | rigid bodies, shapes, joints, the character controller, triggers, casts | one DLL: Jolt |
| `fbengine_ui` | a game's UI: widgets, layout, themes, focus; a debug UI and console | none, on fbengine's 2D |
| `fbengine_video` | video into a texture, with its sound | one DLL: a decoder |
| `fbengine_tools` | the cooker, pak files, and typed references | none |

- The split is where a game can do without a part: a 2D game without Jolt and 3D, a server
  without a window, audio and physics without each other. The window, the GPU device, 2D, the
  scene, and the renderer share one window, device, and frame, so they're one package.
- Each package's DLL is called only by its own FB code, so no package's C++ links to another's.
- Each DLL is C++23 behind a C API of fixed-size types, wrapped by FB code through fb-bindgen's
  bindings, with its C++ runtime linked in, and exports only its C API.

## Milestones

Tests never open a window: SDL's offscreen video, and Vulkan on lavapipe and MoltenVK in CI.

| | Goal | Done when |
|---|---|---|
| **E0** | Setup: the repository, CI on every target, publishing, headless Vulkan | a hello package is added with `fb add` and runs on Windows, Linux, and macOS (2026-10-31, from staging) |
| **E1** | App and input: the window, Flip and timing, keyboard, mouse, gamepads and joysticks, actions | a test under the offscreen driver reads every kind of input, and coroutines resume once per frame (steps 1 and 2 done, 2026-10-31) |
| **E2** | The GPU core: Vulkan, the swapchain, shaders, textures, render targets and readback, the frame graph | golden images of a triangle, a textured quad, and a readback match |
| **E3** | 2D: drawing, textures, text, pixel access, 2D collisions | a small arcade game runs, and its frames match their golden images |
| **E4** | The scene: worlds, entities, transforms, cameras, lights, geometry, materials, visibility, collisions, picks, sprites, fixed steps | transforms, visibility, collisions, and picks pass their tests, with no GPU |
| **E5** | Drawing the scene: materials with layers and lightmaps, clustered lights, transparency, cameras into textures | a lightmapped, fogged, lit room with cameras rendering to textures matches its golden images |
| **E6** | The modern renderer: PBR, shadows, HDR, post effects, particles, instancing, custom shaders | Khronos's glTF sample models match their references |
| **E7** | Animation: skeletons, clips and blending, bones under game control, morph targets | skinned and morphing sample models match their reference frames |
| **E8** | Assets: the VFS and paks, loaders, the cache, the cooker, typed references, hot reload | a game runs from cooked paks, and reloads changed files in development |
| **E9** | Audio | tests mix to known buffers, and sounds follow entities |
| **E10** | Physics | Jolt's samples stay stable, deterministic runs repeat, and bodies drive entities |
| **E11** | UI | a sample game's menus, HUD, and console work by mouse, keyboard, and gamepad |
| **E12** | Video | a video plays into a texture with its sound in sync |
| **E13** | The editor | a scene is made, saved, played, and inspected in the editor |
| **E14** | The proof: a sample for each engine feature SCP:CB needs, and a modern showcase game | all run on Windows, Linux, and macOS |

The engine's docs are `fb doc`'s, on its packages' pages; futureblitz.dev's stay the language's.

## What the engine needs from the language

The engine is the largest FutureBlitz code yet, so it'll find bugs. Each is fixed in FutureBlitz,
with a test, in a 1.0.x or 1.1 release. What it needs that phase 1 left out, all of it things any
library could use ([open-questions.md](open-questions.md)):

| What | Needed by |
|---|---|
| Build steps or tools in a package, which the cooker and typed references run | E8 |
| Setting a reflected Property or Static Field (milestones.md's Deferred work) | E13's inspector, and saving scenes |
| `Plugin.Load` and `DynamicLibrary` (Deferred work) | mods, and reloading game code in the editor |
| Re-exports, so a package can hand on another's names | only an umbrella package, which the engine can do without |
| A Callback taking or giving a Struct by value (Deferred work) | nothing, while the C APIs pass Structs by pointer |

## API style (Locked: modern)

Objects with methods, not flat global functions with int handles.

```vb
Local cam = Camera()
cam.SetPosition(Vec3(0, 5, 10))
cube.Turn(0, Deg(1), 0)
cube.SetColor(Color.RED)
```

- What reaches the engine is a `@MainThread` Function or Method, never a Property, since
  `@MainThread` only goes on Functions and Methods (the user, 2026-10-31): `Window.SetTitle("x")`
  and `Window.Title()`, `cam.SetPosition(p)` and `cam.Position()`. What's only data, as a
  display's modes are, is a Struct's Fields.
- Enum members are UPPER_CASE, as std's are: `WindowMode.FULLSCREEN`, `Key.ESCAPE`.
- Engine resources (entities, textures, meshes) have an explicit `Free()`, since the scene owns
  them; handles are generational, so using a freed one is a clear error, not a crash.

## Frame loop

The **game owns the main loop**. There is no engine-owned update callback.

```vb
Try Graphics(1280, 720)

While !KeyHit(Key.ESCAPE) && !Window.Closed()
    UpdateGame()
    UpdateWorld(Frame.Delta())
    RenderWorld()
    Flip()
End While
```

`Flip()` is the frame boundary, and the engine package's: it presents the frame, reads the OS's
events, then calls `Scheduler.Main.Step()`, so the game's coroutines resume at the start of the
next frame, before its update code, and a game never steps the scheduler itself. The language
knows nothing of frames; a coroutine's `Await NextTick()` waits for the next Step, which in a game
is the next frame (see [spec/10-concurrency.md](spec/10-concurrency.md#scheduling)).
