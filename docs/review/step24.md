# The review of M6 step 24, std.image I: the codecs (PNG, JPEG, BMP, TGA, GIF, HDR, QOI, WebP, SVG, animated GIF and APNG) into an Image, and the transforms (resize, crop, flip, rotate, convert)

7 confirmed findings: 0 high, 4 medium, 3 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## decode-1 (medium): GIF decoder reserves memory by a frame descriptor's size, which the limit doesn't check

Status: fixed in 4044cfb (each index is drawn on the canvas as it's decoded, with no buffer sized by the frame's descriptor, and a frame that isn't interlaced stops at its first row past the screen, so a frame holds no more memory than the screen; tests/runtime/image_tests.cpp, whose operator new counts the heap held, decodes the finding's 35-byte GIF holding under 1 MB, 4294836399 bytes before)

Where: runtime/src/gif_decode.cpp:137

decode() checks only the logical screen's width and height against `limit`. Each frame's own width and height, up to 65535 by 65535, then goes to decompress() as `count`, and decompress() calls indexes.reserve(count) before it reads any codes. A 1 by 1 GIF of about 40 bytes whose image descriptor says 65535 by 65535 therefore commits about 4.1 GB. On a machine with less commit available, std::bad_alloc escapes the extern "C" function, which terminates the program. Indexes past the screen are also kept and walked, though they're never drawn. This breaks the step's rule that decoders on hostile input don't allocate without bound, and the limit a caller passes doesn't help. I measured it with a polling monitor: peak private memory 4105 MB for this program, while its working set stayed at 4 MB.

```
Import std.core
Import std.image

Function Data(): UInt8[]
    Return [
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xff, 0xff, 0xff, 0x2c, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x02, 0x02, 0x4c, 0x01, 0x00, 0x3b
    ]
End Function

Try
    Local image = Gif.Decode(Data(), 100)
    Print("{image.Width} by {image.Height}")
Catch e As IOError
    Print("IOError: {e.Message}")
End Try
```

Expected: "1 by 1", with memory proportional to the 1-pixel screen or the 35-byte input (a few KB), or an IOError for a frame far larger than the screen.

Actual: Prints "1 by 1", but the process commits about 4.1 GB first (measured peak private memory 4105 MB). Where that much commit isn't available, std::bad_alloc escapes fb_image_gif_decode and the program terminates instead of throwing an IOError.

Suggested fix: In decompress(), don't reserve `count` up front. Either reserve min(count, codes.size() * 4096), or better, clip what's decoded to the screen: for frames, stop at (or count only) indexes that land inside width by height, and cap `count` at the frame's on-screen part (or reject a frame whose descriptor goes past the screen, as many decoders do). Add a test of a 1 by 1 GIF with a 65535 by 65535 frame descriptor.

Skeptic: I read runtime/src/gif_decode.cpp. decode() checks only the screen's size against `limit` (line 258). It reads frame_width and frame_height from the image descriptor (lines 330-331), and neither is checked against the screen or the limit. It then calls decompress(codes, least, frame_width * frame_height, indexes) (line 354), and decompress() does `indexes.reserve(count)` (line 142) before it reads any codes. I ran the finding's 35-byte program from rv-step24/decode-1/gif.fb with `fb run`; it printed "1 by 1". I then built a copy that sleeps 4 s after Decode, in both debug and --release builds, and read the process counters from PowerShell while it slept. Both builds showed PeakPagedMemorySize64 (peak private commit) of 4,105 MB, peak virtual 8,245 MB, and a peak working set of 4 MB. A baseline that is the same except for a 1 by 1 frame descriptor (base.fb) peaked at 0.7 MB of private commit. So a 35-byte input with limit 100 commits about 4.1 GB, and the caller's limit doesn't prevent it. That breaks the step's rule that decoders on hostile input don't allocate without bound. Where the commit isn't available, bad_alloc would leave the extern "C" fb_image_gif_decode. I didn't force that failure, but the unchecked 4 GB reserve is plain in the code and I measured it. I rate it medium: it's a resource-exhaustion problem on untrusted input, not memory corruption. The fix is to cap the reserve, for example at min(count, codes.size() * 4096) or at the frame's on-screen part, or to reject frames that go past the screen. A regression test should decode a 1 by 1 GIF whose frame descriptor says 65535 by 65535.

## encode-1 (medium): Gif.EncodeAnimation drops the delay and plays of a one-frame animation

Status: fixed in f643fb2 (the GIF encoder writes an animation's NETSCAPE2.0 and Graphic Control Extensions when it has more than one frame, plays other than once, or a delay, so a one-frame Animation keeps both, and a still image is as it was; image_codecs' one-frame animation, opaque so no transparency writes the extension anyway, gives 1 0 [500] and 1 3 [500], 1 1 [0] before)

Where: runtime/src/gif_encode.cpp:372

gif_encoder_t::finish only treats the GIF as animated when it has more than one frame (`animated = m_frames.size() > 1`). For a one-frame Animation it writes no NETSCAPE2.0 extension, so the plays are lost. It also writes no Graphic Control Extension unless a pixel is transparent, so the delay is lost too. Reading it back with Gif.DecodeAnimation gives plays 1 and delay 0, whatever the Animation had. Png.EncodeAnimation keeps both for the same animation. This breaks the round trip that docs/stdlib.md and the image_codecs test describe as "animations keep their frames, delays, and plays". The test only uses animations of 2 or more frames, so it misses this.

```
Import std.core
Import std.math.color
Import std.image

Local one = Animation(2, 2, 0)
Local img = Image(2, 2)
img.Fill(Color32(255, 0, 0))
one.Add(img, 500)
Local g = Try Gif.DecodeAnimation(Gif.EncodeAnimation(one))
Print("GIF one frame:", g.Count, g.Plays, g[0].Delay)
Local p = Try Png.DecodeAnimation(Png.EncodeAnimation(one))
Print("APNG one frame:", p.Count, p.Plays, p[0].Delay)
one.Plays = 3
g = Try Gif.DecodeAnimation(Gif.EncodeAnimation(one))
Print("GIF one frame plays 3:", g.Count, g.Plays, g[0].Delay)
```

Expected: GIF one frame: 1 0 500
APNG one frame: 1 0 500
GIF one frame plays 3: 1 3 500

Actual: GIF one frame: 1 1 0
APNG one frame: 1 0 500
GIF one frame plays 3: 1 1 0

Suggested fix: Make the GIF encoder know it is encoding an Animation: pass a flag from EncodeAnimation, or use plays != 1 or delay != 0. When it is, write the GCE with the delay for every frame, and write NETSCAPE2.0 whenever plays != 1, even for one frame. Add a one-frame case to image_codecs.fb.

Skeptic: I ran the finding's program in debug and release. It printed 'GIF one frame: 1 1 0', 'APNG one frame: 1 0 500' and 'GIF one frame plays 3: 1 1 0'. In runtime/src/gif_encode.cpp finish(), `const bool animated = m_frames.size() > 1;` decides whether NETSCAPE2.0 is written (`animated && m_plays != 1`). The GCE with the delay is written only when `animated || frame.transparent >= 0`. So a one-frame Animation loses its plays and its delay. docs/stdlib.md says Gif.EncodeAnimation writes a GIF's delays and plays, and image_codecs.fb says 'Animations keep their frames, delays, and plays'. APNG keeps both for the same animation.

## encode-2 (medium): PNG, APNG, BMP, and HDR files std.image writes with a side over 2^24 can't be read back by std.image

Status: fixed in 8b4607a (stb.c defines STBI_MAX_DIMENSIONS as 0x7fffffff; stb_image also reads no PNG of more than 2^30 bytes of pixels, nor an HDR image of more than an int's bytes of floats, so Png.Encode and Png.EncodeAnimation panic past 268435456 pixels, and an HdrImage holds 134217727 at most, as many as stb_image reads, so what std.image writes it reads back; and stb.c's fb_stbi_info_of asks the one format the data starts as, so a PNG too large is OTHER rather than INVALID_DATA's "not of any known type", and a BMP whose headers end too soon is UNEXPECTED_END; tests: image_wide, a 16777217 by 1 PNG and BMP round trip, and a PNG header of 16385 by 16384 is OTHER; panic_image_apng_pixels and panic_image_hdr_size; image_codecs' 4-byte BMP)

Where: third_party/stb.c:20

Png.Encode, Bmp.Encode, Hdr.Encode, and Png.EncodeAnimation take any side up to 2147483647 (CheckEncodable). stb_image is built with its default STBI_MAX_DIMENSIONS of 1 << 24, so it rejects any side over 16777216. A 16777217-by-1 image, which is legal and well under MOST_PIXELS and the limit, encodes fine but fails to decode. PNG gives INVALID_DATA "Image not of any known type, or corrupt", which is the wrong kind for a valid file. BMP and HDR give OTHER "Very large image (corrupt?)" even though the pixel count is under the limit. QOI, which the runtime decodes itself, round-trips. This breaks the done-when "each format round-trips where it can be written", and the docs say no such limit exists for these formats.

```
Import std.core
Import std.image

Local wide = Image(16777217, 1)
For name In ["PNG", "BMP", "QOI"]
    Try
        Local back: Image
        Select name
            Case "PNG"
                back = Png.Decode(Png.Encode(wide))
            Case "BMP"
                back = Bmp.Decode(Bmp.Encode(wide))
            Default
                back = Qoi.Decode(Qoi.Encode(wide))
        End Select
        Print(name, back.Width, back.Height)
    Catch e As IOError
        Print(name, e.Kind, e.Message)
    End Try
End For
Local hdr = HdrImage(16777217, 1)
Try
    Local back = Hdr.Decode(Hdr.Encode(hdr))
    Print("HDR", back.Width, back.Height)
Catch e As IOError
    Print("HDR", e.Kind, e.Message)
End Try
```

Expected: PNG 16777217 1
BMP 16777217 1
QOI 16777217 1
HDR 16777217 1

Actual: PNG INVALID_DATA the PNG data isn't valid: Image not of any known type, or corrupt
BMP OTHER the BMP image is too large: Very large image (corrupt?)
QOI 16777217 1
HDR OTHER the HDR image is too large: Very large image (corrupt?)

Suggested fix: In third_party/stb.c, define STBI_MAX_DIMENSIONS (for example 0x7fffffff) before including stb_image.h. The runtime's own over_limit already guards the pixel count, and stb keeps its own overflow checks. The other option is to cap the encoders' sides at 16777216 and document that. Either way, add a test of a side past 2^24.

Skeptic: I ran the finding's program. The output was 'PNG INVALID_DATA the PNG data isn't valid: Image not of any known type, or corrupt', 'BMP OTHER the BMP image is too large: Very large image (corrupt?)', 'QOI 16777217 1' and 'HDR OTHER the HDR image is too large: Very large image (corrupt?)'. third_party/stb.c defines no STBI_MAX_DIMENSIONS, so stb_image's default of 1<<24 applies. docs/stdlib.md gives side limits only for JPEG, TGA, GIF (65535) and WebP (16383), so PNG, BMP and HDR encode a 16777217x1 image (under the pixel limit) that std.image can't decode. That breaks the milestones row's 'each format round-trips where it can be written'. The PNG error also names the wrong kind (INVALID_DATA for a valid file).

## transform-1 (medium): An image with zero area and a huge side crashes Resize (bad_alloc) and makes Crop spin for a minute

Status: fixed in ccdcc5e (resizing to no pixels, cropping to none, and flipping or turning an image of none return at once, before any taps or rows; image_no_pixels resizes by three filters, crops, flips, and turns Image(0, 2147483647) and Image(2147483647, 0), a bad_alloc before)

Where: runtime/src/transform.cpp:174

CheckSize only limits width*height, so Image(0, 2147483647) and Image(2147483647, 0) are valid Images with no pixels. Resize allows going from no pixels to no pixels, but resized() still sizes its buffers from the side that isn't zero. In `std::vector<float> wide(columns * source_rows * 4)` that is 5 * 2147483647 * 4 floats (172 GB). Exceptions are off in the runtime, so std::bad_alloc goes to std::terminate and the program dies. The program below dies about 1.4 s in, and its first Print is lost. The other direction, Image(2147483647, 0).Resize(5, 0, ResizeFilter.BILINEAR), builds tap vectors of about 8.6e8 doubles for each output pixel in taps_of. It ran for 1m43s and then hit the same bad_alloc abort. On the same image, Crop(0, 0, 0, 2147483647) runs fb_image_crop's row loop 2^31 times, inserting nothing each time, and takes about 64 s for an image with no pixels. NEAREST and the rotations return straight away. (Program files: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-step24/transform/tall.fb, wide.fb, cropzero.fb.)

```
Import std.core
Import std.image

Local image = Image(0, 2147483647)
Print(image.Width, image.Height, image.ToBytes().Count)
image.Resize(5, 0, ResizeFilter.BILINEAR)
Print(image.Width, image.Height, image.ToBytes().Count)
```

Expected: 0 2147483647 0
5 0 0

Actual: terminate called after throwing an instance of 'std::bad_alloc'
  what():  std::bad_alloc
error: tall.exe exited with code 0xC0000409

Suggested fix: In fb_image_resize, fb_image_crop and fb_image_orient, return an empty output straight away when width*height or new_width*new_height is 0, before building taps or the intermediate buffer. Another option is to have CheckSize limit each side, for example to MOST_PIXELS. Add a test program that resizes and crops Image(0, 2147483647) and Image(2147483647, 0).

Skeptic: Ran tall.fb (Image(0, 2147483647).Resize(5, 0, BILINEAR)) in debug. It died after 1.5 s with 'terminate called after throwing an instance of std::bad_alloc' and exit 0xC0000409, and the first Print was lost. crop.fb (Image(0, 2147483647).Crop(0, 0, 0, 2147483647)) printed the right sizes but took 38 s for an image with no pixels. The code matches the report. CheckSize (std/image/image.fb:233) passes any side when the other is 0. Resize only panics when there are no pixels and the new size has some. resized() in runtime/src/transform.cpp then allocates wide(columns * source_rows * 4), and taps_of loops over the full side. fb_image_crop loops `for row = y; row < y + crop_height` even when crop_width is 0.

## encode-3 (low): Docs say a GIF's median-cut palette has 255 colors, but an opaque image gets 256

Status: fixed in 90ad627 (gif.fb's comment and stdlib.md say median cut picks 256 colors for an opaque image, and 255 and the transparent one when a pixel is; image_codecs pins it: GIF's palettes: 256 255)

Where: std/image/gif.fb:6

gif.fb's header comment and docs/stdlib.md both say that, past 256 colors, the palette is "the 255 median cut picks". In indexed(), room is palette_size - (transparent ? 1 : 0), so median cut picks 256 colors for an image with no transparent pixels. Only an image with a transparent pixel gets 255 plus the transparent entry. The code is the better behavior, so the docs should change.

```
Import std.core
Import std.math.color
Import std.collections
Import std.image

Local img = Image(64, 64)
For y = 0 Until 64
    For x = 0 Until 64
        img[x, y] = Color32((x * 4) As UInt8, (y * 4) As UInt8, ((x + y) * 2) As UInt8)
    End For
End For
Local back = Try Gif.Decode(Gif.Encode(img))
Local seen = Set<UInt32>()
Local bytes = back.ToBytes()
For i = 0 Until bytes.Count Step 4
    seen.Add(((bytes[i] As UInt32) << 16) | ((bytes[i + 1] As UInt32) << 8) | (bytes[i + 2] As UInt32))
End For
Print("opaque, 4096 colors in: {seen.Count} colors out")
```

Expected: opaque, 4096 colors in: 255 colors out (as the docs say)

Actual: opaque, 4096 colors in: 256 colors out

Suggested fix: Change gif.fb's comment and docs/stdlib.md to say "or else the 256 median cut picks, 255 when one pixel is transparent".

Skeptic: I ran the finding's program, which printed 'opaque, 4096 colors in: 256 colors out'. In runtime/src/gif_encode.cpp:228, `room = palette_size - (transparent ? 1 : 0)` and median_cut(histogram, room), so an opaque image gets 256 median-cut colors. std/image/gif.fb's header and docs/stdlib.md ('or else 255 that median cut picks') both say 255. The docs contradict the code, and the docs are the side to fix.

## transform-2 (low): Resize's taps are a separate heap vector for each output pixel, so a long thin result uses about 8x the memory NEAREST does

Status: fixed in 03f8885 (a row's taps down are worked out as it's drawn, and the taps across for a run of columns at a time, at most 65536 weights, which each row uses, by the same arithmetic in the same order, so the pixels are the same; image_tests.cpp resizes a pixel to 1 by 2000000 and 2000000 by 1 by BILINEAR holding under 12 MB and 48 MB, the image and what's between the passes, 108 MB and 123 MB before)

Where: runtime/src/transform.cpp:103

taps_t keeps a std::vector<double> for each output column or row, plus a first index. That adds roughly 60 bytes or more of overhead per output pixel along a side, compared with 4 bytes of output per pixel for a 1-wide image. Measured with --release builds by polling PeakWorkingSet64: the BILINEAR resize below peaks at 1236 MB, while the same resize with NEAREST peaks at 156 MB. The output is 80 MB either way. Scaled to a 1 by 536870911 result, which an Image is documented to hold, the BILINEAR taps alone would need about 33 GB, and with exceptions off that ends in a bad_alloc abort where NEAREST needs about 4 GB. (Program files: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-step24/transform/upscale.fb and upscale_nearest.fb.)

```
Import std.core
Import std.image

Local image = Image(1, 1)
image.Resize(1, 20000000, ResizeFilter.BILINEAR)
Print(image.Width, image.Height, image.ToBytes().Count)
```

Expected: 1 20000000 80000000, with peak memory near the NEAREST version's (156 MB)

Actual: 1 20000000 80000000, with a peak working set of 1236 MB (the same program with ResizeFilter.NEAREST: 156 MB)

Suggested fix: Store taps flat, as Pillow does: one std::vector<double> of count * window weights, where window = ceil(support) * 2 + 1, with per-pixel first and length arrays. Taps memory is then bounded by count * window * 8 bytes and needs no allocation per pixel.

Skeptic: Built thin.fb (Image(1,1).Resize(1, 20000000, BILINEAR)) and thin_n.fb (the same with NEAREST) with --release and polled PeakWorkingSet64. BILINEAR peaked at 1236 MB and NEAREST at 157 MB, and both printed '1 20000000 80000000'. taps_t (transform.cpp:103) holds a std::vector<std::vector<double>> with one heap vector per output pixel along a side, about 55 B each. A 1 by 536870911 result, which is documented as valid, would need about 30 GB of taps. With exceptions off, that ends in a bad_alloc abort where NEAREST would succeed. Storing the weights flat fixes it.

## transform-3 (low): The docs say Resize's bytes are within 1 of Pillow's, but that holds only for opaque images

Status: fixed in eb4c2d6 (stdlib.md says an opaque image's bytes are within 1 of Pillow's, and a translucent one's colors are more exact, as Pillow premultiplies alpha in bytes; image_transforms pins the finding's faint pixels by BILINEAR and BOX, whose BILINEAR red, 2450 / 26, is 94)

Where: docs/stdlib.md:924

docs/stdlib.md says Resize's sums are Doubles where Pillow's are fixed-point, "and its bytes within 1 of Pillow's", with no condition. Pillow resizes RGBA by premultiplying into 8-bit RGBa first. At low alpha that rounds colors away, so for translucent pixels Resize differs from Pillow by far more than 1. Resize's result is the mathematically right one: for pixel 0 under BILINEAR the weights are 3/7, 3/7 and 1/7, so red = (200*3*3 + 10*5*3 + 250*2*1) / (3*3 + 5*3 + 2*1) = 2450/26 = 94.2. Pillow 12.3 gives 63. Only the doc's claim is wrong. image_transforms.fb tests only an opaque image (its make_data.py notes this), so the general claim has no test behind it.

```
Import std.core
Import std.math.color
Import std.image

// Pillow 12.3: Image.new("RGBA", (4, 1)) with these pixels, .resize((2, 1), BILINEAR) gives
// (63, 191, 63, 4) (85, 85, 170, 3), and by BOX (63, 191, 127, 4) (170, 85, 170, 3).
For filter In [ResizeFilter.BILINEAR, ResizeFilter.BOX]
    Local image = Image.FromBytes(4, 1, [200, 100, 50, 3, 10, 240, 90, 5, 250, 30, 160, 2, 60, 70, 220, 4])
    image.Resize(2, 1, filter)
    Print(filter, image[0, 0], image[1, 0])
End For
```

Expected: Per the docs, within 1 of Pillow's:
BILINEAR (63, 191, 63, 4) (85, 85, 170, 3)
BOX (63, 191, 127, 4) (170, 85, 170, 3)

Actual: BILINEAR (94, 175, 82, 4) (99, 97, 176, 3)
BOX (81, 187, 75, 4) (123, 57, 200, 3)

Suggested fix: Limit the claim in docs/stdlib.md to opaque images, and say why: Pillow premultiplies alpha in bytes, while Resize weights by alpha in Doubles, so translucent pixels keep their colors more exactly than Pillow's. Optionally add a test that pins this case's output to the exact value computed by hand.

Skeptic: alpha.fb printed 'BILINEAR (94, 175, 82, 4) (99, 97, 176, 3)' and 'BOX (81, 187, 75, 4) (123, 57, 200, 3)'. Local Pillow 12.3.0, given the same pixels, gives BILINEAR [(63,191,63,4),(85,85,170,3)] and BOX [(63,191,127,4),(170,85,170,3)]. That is off by up to 47, not within 1. Pillow premultiplies RGBA into 8-bit RGBa before resampling, so its colors round away at low alpha. The hand-computed BILINEAR red is 2450/26 = 94.2, so Resize's own output is the more exact one. docs/stdlib.md:912-913 still claims, with no condition, that its bytes are 'within 1 of Pillow's'. That holds only for opaque images, so the doc needs the limit.
