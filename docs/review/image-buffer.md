# The review of M6 step 32 audit gap in std.image (steps 24/25 follow-up): an Image's pixels as a Buffer<UInt8>, Image.Over, and Image.Load/Font.Load from files with Async twins

1 confirmed findings: 0 high, 0 medium, 1 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## runtime-1 (low): fb_stb_encode_hdr's comment still calls its pixels a Float32[], and floats_of is left with no callers

Status: fixed in fdd78da7 (the comment says the pixels are the width * height * 4 floats at
`pixels`, and floats_of is deleted; no test, as neither changes what a program does, and the
build fails if anything still calls floats_of)

Where: runtime/include/fb/image.hpp:158

This step changed fb_stb_encode_hdr to take a `const float*` into an HdrImage's Buffer<Float32>, and fb_image_hdr_pixels now writes through a float pointer. The header's comment above fb_stb_encode_hdr still says its pixels are "a Float32[] as fb_image_hdr_pixels gives them", which no longer matches the signature or the file's opening paragraph. The stb.cpp change also removed the only call to fb::image::floats_of (runtime/src/image_internal.hpp:99), so that helper is now dead code. Grepping runtime/, tests/, compiler/, cli/, and tools/ finds no other use. Everything else I checked in the runtime part gave the right results, the same in debug and release: blit's overlap copy (self-paste down or right, offset and negative positions, a sub-view pasted onto its parent, a half-alpha self-draw that gives (100,0,100,255) as worked out by hand), every transform on an empty image, Canvas drawing past the edges of an image over the middle of a Buffer (the guard bytes stay 0), QOI/PNG/BMP/WebP/HDR encoding from a Buffer slice, and GIF animation frames.

```
Import std.core
Import std.ffi
Import std.math.color
Import std.image

// Hdr.Encode hands fb_stb_encode_hdr a pointer into a Buffer<Float32> slice, not a Float32[].
Local all = Buffer<Float32>(16)
For i = 0 Until 16
    all[i] = (i As Float32) / 4
End For
Local view = HdrImage.Over(1, 2, all.Slice(4, 12))
Try
    Local back = Try Hdr.Decode(Hdr.Encode(view))
    Print("{back.Width} {back.Height} {back.Pixels.Count} {back[0, 0]} {back[0, 1]}")
Catch e As IOError
    Print(e.Message)
End Try
```

Expected: The program prints `1 2 8 (1, 1.25, 1.5, 1) (2, 2.25, 2.5, 1)`, and the header comment says the pixels are a pointer to width * height * 4 floats, as an HdrImage's Buffer<Float32> holds them.

Actual: The program prints `1 2 8 (1, 1.25, 1.5, 1) (2, 2.25, 2.5, 1)` in debug and release, so the code is right. The comment at runtime/include/fb/image.hpp:158 still says "of `pixels`, a Float32[] as fb_image_hdr_pixels gives them", and image_internal.hpp still defines floats_of, which nothing calls.

Suggested fix: Reword the comment, for example: "Radiance's RGBE, run-length encoded, of the width * height * 4 floats at `pixels`, as fb_image_hdr_pixels gives them, whose alpha it leaves out." Then delete floats_of from runtime/src/image_internal.hpp.

Skeptic: The program runs correctly. I wrote it to C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-imgbuf/runtime-1/hdr.fb, and the debug and release builds both print `1 2 8 (1, 1.25, 1.5, 1) (2, 2.25, 2.5, 1)`. The two documentation and code-health problems the finding reports are both real.

1. The header comment is stale. In runtime/include/fb/image.hpp:158-159, the comment still says "of `pixels`, a Float32[] as fb_image_hdr_pixels gives them". The function now takes `const float* pixels`, and std/image/codec.fb:42 declares it with `pixels: Ptr<Float32>`. HdrImage stores its pixels in a `Buffer<Float32>` (std/image/hdr.fb:14).
   - The diff 8c7f3fb1..f865bd94 rewrote the two other "a Float32[]" comments in this header: fb_image_hdr_pixels's, and the removed zero-fill helper's. It left this one unchanged.
   - The file's opening paragraph now describes pixels as reached through a pointer into a Buffer, so this comment contradicts it.

2. floats_of is dead code. Before the change, its only caller was stb.cpp:166 at 8c7f3fb1, `std::vector<float> values(floats_of(pixels)...)`, which this change replaced. Grepping runtime/, tests/, compiler/, cli/, tools/, and std/ at f865bd94 finds only the definition at runtime/src/image_internal.hpp:99.

The project's rules say every finding of any severity gets fixed, so this counts. It is low severity, a stale comment plus an unused helper, with no wrong behavior.

## Refuted by the skeptics (no action)

- std-1: Image.LoadAsync's negative-limit panic has no test. I wrote rv-imgbuf/std1/a3.fb, the finding's program. In debug and in release it prints "start", then "panic: a limit of pixels can't be negative, like -1 at a3.fb:5:11", and exits 101. So the panic happens at the LoadAsync call, before the Await, and is attributed to the caller. I also wrote f.fb, which awaits Font.LoadAsync(Path("missing.ttf")) and catches the IOError. My first version used e.Path, a String?, inside a string without a fallback, which the compiler rejects, so I added ?? "none". In debug and release it prints "NOT_FOUND missing.ttf", which is right. In std/image/image.fb (lines 148-162), Load and LoadAsync are both @PanicsAtCaller and both call CheckLimit(limit) first, so the async twin's check is the same code as the sync one. panic_image_load_limit.fb tests that code through Load. image_files.fb already tests Font.LoadAsync on a text file and on a valid TTF, and tests a missing file through Image.LoadAsync, which reads it with the same File.ReadBytesAsync call. I saw no wrong behavior. The two missing cases are only extra coverage of code paths that are already tested. They don't plainly contradict stdlib.md or step 32's "documented and tested" row, which applies to the module as a whole. Refuted. If the coordinator wants the extra tests anyway, they are low priority.
