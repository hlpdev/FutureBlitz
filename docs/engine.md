# Engine

Phase 2 work. This doc records the direction; detailed API design comes later.

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

`Flip()` is the frame boundary. It also ticks the main coroutine scheduler
(see [spec/10-concurrency.md](spec/10-concurrency.md#scheduling-proposed-recommended-not-explicitly-confirmed)).

## Threading

Engine APIs are `@MainThread`. Calling them from a worker is a compile error.

## Resource lifetime

Engine resources (entities, textures, meshes) have an explicit `Free()` because the scene
owns them. Handles are generational: using a freed resource raises a clear error, not a
crash.

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

## Engine modules (draft)

| Module | Contents |
|---|---|
| `engine.app` | window, graphics mode, `Flip`, frame timing |
| `engine.input` | keyboard, mouse, gamepad, input actions |
| `engine.gfx2d` | images, drawing, fonts, text |
| `engine.scene` | entities, pivots, cameras, lights, meshes, hierarchy |
| `engine.render` | materials, shaders, shading models, shadows, post-FX |
| `engine.assets` | see [assets.md](assets.md) |
| `engine.physics` | Blitz-style collisions and picks, raycasts, Jolt backend |
| `engine.audio` | sounds, 3D audio, music streaming (miniaudio) |
