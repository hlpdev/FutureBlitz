# Asset System

Direction is **Locked**; detailed design happens at milestone M14.

Blitz3D's asset handling is a mess. FutureBlitz standardizes and modernizes it.

| Blitz3D problem | FutureBlitz |
|---|---|
| Magic-number flags (`LoadTexture(f, 1+8+256)`) | Typed import settings: a sidecar file (`wall.png.toml`) or an options struct in code |
| Loading the same file twice creates two copies | Automatic cache: same path returns the same asset; refcounted; freed when unused |
| Paths relative to the working directory; case-sensitivity issues across platforms | Virtual filesystem with `res://` paths, normalized; mountable pak files and mod overlays |
| Every load blocks | `Await Assets.LoadAsync<Mesh>(...)` with progress reporting for loading screens |
| `.x`, `.3ds`, `.b3d` formats | **glTF** is the primary mesh format; old formats go through a one-time converter |
| Raw source files shipped with the game | Build step **cooks** assets into compressed GPU formats in pak files; dev builds load source files directly with **hot reload** |
| Typo in a path is a runtime failure | Build generates **typed references** (`Res.Textures.Wall`); a missing file is a compile error |

## Sketch

```vb
// typed, cached load
Local wall = Assets.Load<Texture>(Res.Textures.Wall)

// async load with progress
Local level = Await Assets.LoadAsync<Scene>(Res.Levels.Lobby, progress: bar)

// import settings in code instead of flags
Local tex = Assets.Load<Texture>("res://textures/hud.png",
                                 TextureOptions(mipmaps: False, filter: Filter.Nearest))
```

```toml
# wall.png.toml (sidecar import settings)
mipmaps = true
filter = "trilinear"
compression = "bc7"
```

## Formats

| Kind | Formats | Library |
|---|---|---|
| Meshes | glTF 2.0 (primary); `.b3d`/`.x`/`.3ds` via converter | fastgltf |
| Textures | PNG, JPG, KTX2 | stb_image, KTX |
| Audio | WAV, OGG, FLAC | miniaudio |
