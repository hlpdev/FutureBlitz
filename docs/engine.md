# Engine

The engine is phase 2, after phase 1 (M0 to M10). Its milestones are E0 to E14, below; each
module's API is designed in its milestone.

The engine is packages built on top of FutureBlitz, never part of the language (the user,
2026-10-30). The language, compiler, runtime, std, and spec depend on none of its features: it
uses what any library can, Extern blocks over its C API, `@MainThread`, and the main scheduler's
`Scheduler.Main.Step()`, and a program uses it by importing its packages.

Its goal: any Blitz3D game can be remade with it, looking nearly identical, and a modern game can
be made with it from the start.

## Packages

The engine is **fbengine**, in its own public repository, hlpdev/fbengine, under Apache-2.0. Its
packages are released together, at one version (the user, 2026-10-30):

| Package | What's in it | Native code |
|---|---|---|
| `fbengine` | the window, input and gamepads, the frame loop and `Flip`, timing; 2D: images, drawing, fonts; the 3D scene: entities, cameras, lights, meshes, terrain, sprites; animation; the renderer, Blitz's look and PBR; loading assets; Blitz's collisions and picks, in `UpdateWorld` | one DLL: SDL3, Vulkan through volk and VMA, fastgltf, KTX |
| `fbengine_audio` | sounds, channels, 3D sound, streamed music, buses, effects | one DLL: miniaudio |
| `fbengine_physics` | rigid bodies, shapes, joints, the character controller, triggers, casts | one DLL: Jolt |
| `fbengine_ui` | a game's UI: widgets, layout, themes, gamepad focus; and a debug UI | none, on fbengine's 2D |
| `fbengine_tools` | the cooker, pak files, typed references, and the mesh converter for .b3d, .x, and .3ds | none |

The editor and the migrator are programs built on these.

- The split is where a game can do without a part: a 2D game without Jolt and 3D, a server
  without a window, audio and physics without each other. The window, the GPU device, 2D, the
  scene, and the renderer share one window, device, and frame, and every Blitz3D game uses them
  together, so they're one package.
- Each package's DLL is called only by its own FB code, so no package's C++ links to another's:
  physics gets a mesh's vertices from fbengine's FB API and hands them to Jolt, as M7 step 6's
  stand-in did with one DLL per package.
- Each DLL is C++23 behind a C API of fixed-size types, wrapped by FB code through fb-bindgen's
  bindings, with its C++ runtime linked in, as tests/native/engine is.

## Milestones

Each is done when its last column holds. Tests render headless, on lavapipe or SwiftShader in
CI and SDL's offscreen driver, and never open a window on a developer's machine; macOS's
rendering is checked on Apple silicon in CI and on an Intel Mac by hand.

| | Goal | Done when |
|---|---|---|
| **E0** | Setup: the repository, CMake, CI building each DLL for every target into its package's archives, trusted publishing, headless rendering in CI | a hello package is added with `fb add` and runs on Windows, Linux, and macOS (2026-10-31, from staging) |
| **E1** | App and input: SDL3 windows and graphics modes, fullscreen and high DPI, frame timing, `Flip` stepping `Scheduler.Main`; keyboard, mouse, gamepads, text input, input actions and rebinding | under the offscreen driver, a test reads input and its coroutines resume once per frame |
| **E2** | The GPU core: Vulkan through volk and VMA, the swapchain and frames in flight, shaders in Slang compiled to SPIR-V, bindless textures, offscreen targets, MoltenVK on macOS | a triangle and a textured quad match their golden images |
| **E3** | 2D: Blitz's 2D commands, modernized: images, frames, handles, and masks, primitives, text and fonts, viewports and origins, pixel access through a CPU staging path, CopyRect, image collisions, drawing into images, batched sprites | a whole small arcade game runs, and its frames match their golden images |
| **E4** | The scene: entities and the hierarchy, every transform and TForm, cameras (projection, viewports, picking, fog), lights, meshes and surfaces, the primitive shapes, sprites, terrain, BSP and MD2, EntityFX, EntityOrder, alpha, auto-fade; Blitz's collisions and picks | transforms, picks, and collision responses match Blitz3D's source |
| **E5** | The Blitz look: `Shading.Vertex`, DX7's fixed-function lighting, linear fog, every TextureBlend mode, eight texture layers, texture flags, sphere and cube maps, Blitz's alpha sorting | reference scenes match screenshots taken in Blitz3D within a tolerance |
| **E6** | The modern renderer: PBR with glTF's materials, image-based lighting, cascaded, spot, and point shadows, HDR and tonemapping, bloom, SSAO, TAA, FXAA, and MSAA, clustered lights, instancing, culling and LOD, GPU particles, decals, sky, materials with their own shaders | Khronos's glTF sample models match their reference images |
| **E7** | Animation: skeletons from glTF and .b3d, blending, morph targets, vertex animation, Blitz's Animate, SetAnimTime, and ExtractAnimSeq | animated sample models match their reference frames |
| **E8** | Assets: `res://` paths on std.vfs, a refcounted cache, async loading with progress, sidecar import settings, the cooker (BC7 and ASTC in KTX2, meshoptimizer), pak files, typed references, hot reload | a game runs from cooked paks in a release build and reloads changed assets in a dev build |
| **E9** | Audio: Blitz's sound commands, channels, 3D sound with Doppler, streaming, buses, effects | on miniaudio's null backend, tests mix to known buffers |
| **E10** | Physics: Jolt's bodies, shapes, joints, character controller, triggers, and casts, deterministic when asked | Jolt's sample scenes stay stable, and a deterministic run repeats exactly |
| **E11** | UI: widgets, layout, themes, gamepad focus, SDF text, localization hooks, a debug UI | a sample game's menus and HUD work with a mouse and with a gamepad |
| **E12** | The editor: code on fb-lsp, a scene editor and inspector, an asset browser, play in the editor, the debugger | a scene is made, saved, played, and debugged in the editor |
| **E13** | The migrator ([migration.md](migration.md)): a .bb parser after Blitz3D's source, the deterministic translation to fbengine's API, the AI-assisted pass, the mesh converter | the sample games that come with Blitz3D translate and run |
| **E14** | The proof: SCP: Containment Breach ported end to end, looking nearly identical, and a modern showcase game | both run on Windows, Linux, and macOS |

The engine's docs are `fb doc`'s, on its packages' pages; futureblitz.dev's stay the language's.

## What the engine needs from the language

The engine is the largest FutureBlitz code yet, so it'll find bugs. Each is fixed in FutureBlitz,
with a test, in a 1.0.x or 1.1 release. What it needs that phase 1 left out, all of it things any
library could use ([open-questions.md](open-questions.md)):

| What | Needed by |
|---|---|
| Build steps or tools in a package, which the cooker and typed references run | E8 |
| Setting a reflected Property or Static Field (milestones.md's Deferred work) | E12's inspector, and saving scenes |
| `Plugin.Load` and `DynamicLibrary` (Deferred work) | mods, and reloading game code in the editor |
| Re-exports, so a package can hand on another's names | only an umbrella package, which the engine can do without |
| A Callback taking or giving a Struct by value (Deferred work) | nothing, while the C APIs pass Structs by pointer |

## API style (Locked: modern)

Objects with methods and properties, not Blitz's flat global functions with int handles.

```vb
// Blitz3D
Local cam = CreateCamera()
PositionEntity(cam, 0, 5, -10)
TurnEntity(cube, 0, 1, 0)
EntityColor(cube, 255, 0, 0)

// FutureBlitz
Local cam = Camera()
cam.Position = Vec3(0, 5, -10)
cube.Turn(0, 1, 0)
cube.Color = Color.Red
```

Why: discoverable via autocomplete, type-safe (a texture can't be passed where an entity
goes), and uses `Vec3`/`Color` naturally.

## Frame loop

The **user owns the main loop**, like Blitz3D. There is no engine-owned update callback.

```vb
Graphics3D(1280, 720)

While !KeyHit(KEY_ESCAPE)
    UpdateGame()
    UpdateWorld()
    RenderWorld()
    Flip()
End While
```

`Flip()` is the frame boundary, and the engine package's: it presents the frame, then calls
`Scheduler.Main.Step()`, so the game's coroutines resume at the start of the next frame, before
its update code, and a game never steps the scheduler itself. The language knows nothing of
frames; a coroutine's `Await NextTick()` waits for the next Step, which in a game is the next
frame (see [spec/10-concurrency.md](spec/10-concurrency.md#scheduling)).

## Threading

Engine APIs are `@MainThread`, as any library's main-thread APIs are. Calling them from a
worker is a compile error.

## Resource lifetime

Engine resources (entities, textures, meshes) have an explicit `Free()` because the scene
owns them, as native resources do (spec 08). Handles are generational: using a freed resource
raises a clear error, not a crash.

## Renderer

- **One renderer**, own **Vulkan** backend (MoltenVK for macOS).
- **No "classic vs modern" split.** Every feature is first-class.
- The Blitz3D look is just one material setting among the others:

```vb
mat.Shading = Shading.Vertex     // Blitz-style per-vertex Gouraud lighting
mat.Shading = Shading.PBR
mat.Shading = Shading.Unlit
```

- Fog, blend modes, shadows, post-FX are ordinary engine features.

### Reproducing the Blitz3D look

So migrated games can look nearly identical, the renderer must be able to match DX7
fixed-function behavior:

- per-vertex Gouraud lighting with Blitz's light model (`Shading.Vertex`)
- linear fog
- texture blend modes (multiply, add, alpha, ...)
- the equivalents of `EntityFX` flags and `EntityOrder`
- Blitz's alpha sorting behavior

Compatibility traps to handle:

- **Pixel operations** (`WritePixelFast`, `LockBuffer`, `CopyRect` equivalents) need a
  CPU-side staging path or they'll be slow on a modern GPU.
- **Coordinate system:** Blitz3D is left-handed; the mesh importers/converter must account
  for it.

The original Blitz3D source is open (zlib license) and should be used as the reference for
exact behavior, rather than guessing.

## Modules (draft)

| Module | Contents |
|---|---|
| `fbengine.app` | window, graphics mode, `Flip`, frame timing |
| `fbengine.input` | keyboard, mouse, gamepad, input actions |
| `fbengine.gfx2d` | images, drawing, fonts, text |
| `fbengine.scene` | entities, pivots, cameras, lights, meshes, hierarchy, Blitz's collisions and picks |
| `fbengine.render` | materials, shaders, shading models, shadows, post-FX |
| `fbengine.assets` | see [assets.md](assets.md) |
| `fbengine_physics` | Jolt's bodies, joints, characters, and casts |
| `fbengine_audio` | sounds, 3D audio, music streaming (miniaudio) |
