# The review of M6 step 25, std.image II: drawing lines, shapes, fills, and text on an Image's Canvas, with fonts read by the runtime's own bounds-checked TrueType and CFF reader and filled by the same exact-coverage rasterizer as the shapes

8 confirmed findings: 1 high, 3 medium, 4 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## raster-1 (high): The rasterizer is quadratic in a row's edges, and cubic with crossings: a 4000-point line chart takes 20 s, and a 16 KB hostile font's glyph takes almost 2 minutes

Status: fixed in eaf0f6e (a row is swept a pixel at a time, with the winding of what's wholly left of the pixel kept as turns down the row, which a ring's edges that meet cancel; a pixel's exact coverage comes from its own pieces and those turns, a run of pixels with none from the turns alone, and a pixel more than 32 pieces pass through is measured along 16 lines across it; image_draw and image_text still match their references byte for byte; tests/runtime/draw_tests.cpp: an 8000-point line chart strokes in 0.15 s and a 3000-point zigzag fills in 0.34 s in a debug build, past the 10 s bound before, and a pixel of 40 overlapping squares' edges is half covered)

Where: runtime/src/draw.cpp:261

cover_row cuts each pixel row into a part at every edge end inside the row. For each part it rebuilds and sorts `across` from every active edge, O(E log E). Then it cuts the part again at every pair crossing, and fill_band costs O(E) plus row.add's columns for every one of those bands. So a row costs O(E^2) when many edges end inside it, and O(E * crossings), about E^3, when edges cross a lot. A stroke makes 7 or 8 edges a point, all ending in the few rows the line covers, so an ordinary line chart grows quadratically: 500 points take 0.32 s, 1000 take 1.36 s, 2000 take 5.45 s, and 4000 take 20.2 s (release build, 400x100 image). Self-crossing outlines are worse. crossing3.fb in the same folder, a 3000-point polygon zigzagging top to bottom over a 200x200 image, takes 3 min 17 s. A font is also affected. The task's settled call says a hostile font never hangs, and FillText goes through this same fill. I built a 16 KB TrueType font whose A is one contour of 3000 on-curve points zigzagging from y=1000 to y=-5 (C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-step25/raster/make_hostile.py, from image_text/font.py, writes hostile/font.fb). In hostile.fb, `canvas.FillText("A", font, 200, Vec2(0, 200), Color32.Black)` on a 200x200 image took 1 min 49 s in release. glyf allows 65535 points a contour, and every curve can become up to 1024 chords, so at that growth a valid-looking font can stall a program for days.

```
// A line chart: a polyline of N points across a 400 by 100 image, 1.5 wide, its y going gently up
// and down over a few rows.
Import std.core
Import std.math
Import std.math.color
Import std.math.vector
Import std.image
Import std.time

For count In [500, 1000, 2000, 4000]
    Local points: Vec2[] = []
    For i = 0 Until count
        Local x = (i As Float) * 400.0 / (count As Float)
        Local y = 50.0 + 3.0 * Sin((i As Float) * 0.05)
        points.Add(Vec2(x, y As Float))
    End For
    Local canvas = Canvas(Image(400, 100))
    canvas.LineWidth = 1.5
    Local start = Instant.Now()
    canvas.StrokePolyline(points, Color32.Black)
    Print("{count} points: {(Instant.Now() - start)}")
End For
```

Expected: Each stroke takes milliseconds, growing about linearly with the points. A scanline rasterizer handles a few thousand edges a row in well under 100 ms. A hostile glyph finishes in bounded time, or the font is refused.

Actual: 500 points: 321.1156ms
1000 points: 1.356244s
2000 points: 5.4523344s
4000 points: 20.1541771s
(The 3000-point self-crossing polygon took 3m17s. The hostile font's one glyph took 1m49s.)

Suggested fix: Stop rescanning every active edge for every part and band. Keep the active edges sorted across a row and, at each event (an edge end or a crossing), swap or insert only the edges involved. Accumulate each stretch's area per pixel with signed-area accumulation (the `left` prefix sums already do this) instead of re-walking the whole list per band. If exact per-band trapezoids stay, cap the work: bound the edges a glyph may contribute, or refuse outlines past a crossing budget, so FillText on an untrusted font is bounded. Add a timing or size regression test for a long polyline.

Skeptic: Reproduced in release builds, with my programs in scratchpad/rv-step25/refute-raster/. r1.fb, the finding's line chart on a 400x100 image at LineWidth 1.5, printed 500 points: 228ms, 1000: 1.01s, 2000: 4.19s. Each doubling costs about 4x, so it is quadratic. cross.fb, a self-crossing zigzag FillPolygon on a 200x200 image, printed 250 points: 140ms, 500: 912ms, 1000: 6.43s. Each doubling costs about 7x, so it is nearly cubic. hostile.fb uses a copy of the reviewer's 16 KB font, whose A is one 3000-point zigzag contour. FillText("A", font, 200, ...) took 1m17.6s. The cause is in runtime/src/draw.cpp. cover_row (around line 261) rebuilds and sorts `across` from every active edge for each part between edge ends. It then cuts at every crossing pair, and fill_band (around line 240) re-evaluates and re-sorts all the edges for each band. Nothing bounds this work, and FillText runs through the same fill(). So a valid-looking font can stall a program for minutes, and larger contours (glyf allows up to 65535 points) would stall it far longer. That breaks the settled call that a broken or hostile font never hangs.

## fonts-1 (medium): A 552-byte CFF font makes one glyph allocate about 4 GB and take seconds to draw, though it stays inside every limit the reader sets

Status: fixed in e061d30 (a glyph is drawn from at most 65536 points once its curves are cut, or not at all, a curve whose control points are all outside the image and a pixel around it is one chord, which covers nothing of the image the curve doesn't, and a CFF path, like a TrueType outline, has at most 65536 pieces; image_tests.cpp draws the finding's 552-byte font's A holding under 64 MB in 0.01 s, 4 GB before)

Where: runtime/src/cff.cpp:33

The reader's limits on a CFF glyph (most_steps = 2^20, subroutine depth 10, a stack of 48, points within most_units = 2^24 units) don't bound how much drawing a glyph costs. A charstring that calls a global subroutine 4 times per level, 7 levels deep, down to one hhcurveto of 12 curves, gives 196,608 cubics in about 900K steps. Each cubic's control points swing 32000 units out and back, so draw.cpp's add_cubic cuts each into most_chords = 1024 chords at an ordinary size (16 px an em on a 16-unit em; at 1000 units an em it's still about 280). That is about 200M ring points, 16 bytes each, from one glyph. fb_image_fill_text also collects every glyph's rings before filling and doesn't cache outlines, so 'AAAA' costs four times as much. This breaks the step's stated property that a hostile font never allocates without bound. TrueType has the same gap: most_points allows 2^20 quadratic segments per glyph, each of which can also become up to 1024 chords. No test checks the reader's resource limits (steps, depth, visits, points) on fonts that parse.

```
// A 552-byte CFF font whose "A" is one charstring calling a global subroutine 7 deep, 4 times
// at each level, down to a leaf of one hhcurveto of 12 curves that swing 32000 units out and
// back: 196608 cubics, each within the reader's limits (steps, depth, stack, 2^24 units), and
// each cut into 1024 chords when drawn at 16 pixels an em on its 16-unit em.
Import std.core
Import std.math.color
Import std.math.vector
Import std.time
Import std.image

Function FontBytes(): UInt8[]
    Return [79, 84, 84, 79, 0, 6, 0, 0, 0, 0, 0, 0, 67, 70, 70, 32, 0, 0, 0, 0, 0, 0, 0, 108, 0,
            0, 1, 36, 99, 109, 97, 112, 0, 0, 0, 0, 0, 0, 1, 144, 0, 0, 0, 44, 104, 101, 97,
            100, 0, 0, 0, 0, 0, 0, 1, 188, 0, 0, 0, 54, 104, 104, 101, 97, 0, 0, 0, 0, 0, 0, 1,
            244, 0, 0, 0, 36, 104, 109, 116, 120, 0, 0, 0, 0, 0, 0, 2, 24, 0, 0, 0, 8, 109, 97,
            120, 112, 0, 0, 0, 0, 0, 0, 2, 32, 0, 0, 0, 6, 1, 0, 4, 4, 0, 1, 4, 0, 0, 0, 1, 0,
            0, 0, 2, 65, 0, 1, 4, 0, 0, 0, 1, 0, 0, 0, 18, 29, 0, 0, 1, 14, 17, 29, 0, 0, 0, 0,
            29, 0, 0, 1, 36, 18, 0, 0, 0, 8, 4, 0, 0, 0, 1, 0, 0, 0, 123, 0, 0, 0, 132, 0, 0, 0,
            141, 0, 0, 0, 150, 0, 0, 0, 159, 0, 0, 0, 168, 0, 0, 0, 177, 0, 0, 0, 186, 28, 125,
            0, 28, 131, 0, 139, 28, 125, 0, 28, 131, 0, 28, 125, 0, 139, 28, 131, 0, 28, 125, 0,
            28, 131, 0, 139, 28, 125, 0, 28, 131, 0, 28, 125, 0, 139, 28, 131, 0, 28, 125, 0,
            28, 131, 0, 139, 28, 125, 0, 28, 131, 0, 28, 125, 0, 139, 28, 131, 0, 28, 125, 0,
            28, 131, 0, 139, 28, 125, 0, 28, 131, 0, 28, 125, 0, 139, 28, 131, 0, 28, 125, 0,
            28, 131, 0, 139, 28, 125, 0, 28, 131, 0, 28, 125, 0, 139, 28, 131, 0, 28, 125, 0,
            28, 131, 0, 139, 28, 125, 0, 28, 131, 0, 28, 125, 0, 139, 28, 131, 0, 27, 11, 32,
            29, 32, 29, 32, 29, 32, 29, 11, 33, 29, 33, 29, 33, 29, 33, 29, 11, 34, 29, 34, 29,
            34, 29, 34, 29, 11, 35, 29, 35, 29, 35, 29, 35, 29, 11, 36, 29, 36, 29, 36, 29, 36,
            29, 11, 37, 29, 37, 29, 37, 29, 37, 29, 11, 38, 29, 38, 29, 38, 29, 38, 29, 11, 0,
            2, 4, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 8, 14, 139, 139, 21, 39, 29, 14, 0, 0, 0, 1,
            0, 3, 0, 1, 0, 0, 0, 12, 0, 4, 0, 32, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 65, 255, 255,
            0, 0, 0, 65, 255, 255, 255, 192, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 95, 15, 60, 245, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 3, 32, 255, 56,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2,
            1, 244, 0, 0, 1, 244, 0, 0, 0, 0, 80, 0, 0, 2, 0, 0]
End Function

Local font = Try Font.Parse(FontBytes())
Print("font bytes:", FontBytes().Count)
Local canvas = Canvas(Image(16, 16))
Local start = Instant.Now()
canvas.FillText("A", font, 16, Vec2(2, 14), Color32.Black)
Print("one glyph:", start.Elapsed)
```

Expected: font bytes: 552, then one glyph drawn in milliseconds with memory in proportion to the font: a hostile font's glyph is cut off (no outline, as for one past the step limit) once its flattened size passes a fixed budget.

Actual: font bytes: 552
one glyph: 9.3630147s  (the process's peak working set, measured from PowerShell, was 4112 MB. The same font with 6 levels peaked at 1032 MB in 2.8 s; with 5, 261 MB in 0.7 s. The reader itself makes the 196609 segments in 0.01 s.)

Suggested fix: Give each glyph a work budget that covers what drawing will do, not just the charstring's steps. For example, in add_glyph_rings or fb_image_fill_text, cap the total chords per glyph (and per FillText call), and drop a glyph's outline when it goes over. Or have the reader reject outlines whose control points lie far outside the em (most_units relative to units_per_em, not 2^24 absolute) and cap CFF path segments the way TrueType's most_points does. Add font_tests cases for fonts built to hit each limit: steps, subroutine depth, stack, composite depth and visits, points.

Skeptic: I built the finding's 552-byte CFF program in release with fb build --release and ran it while sampling its peak working set from PowerShell. It printed 'font bytes: 552' and 'one glyph: 8.87s', and the peak was 4111 MB; fb run --release gave 8.06 s. The code agrees. cff.cpp limits only charstring steps (most_steps = 2^20), depth (10), stack (48) and coordinates (most_units = 2^24 absolute). draw.cpp's add_cubic and add_quadratic cut each curve into up to most_chords = 1024 points, and nothing caps the chords per glyph. fb_image_fill_text also collects every glyph's rings into one vector before fill() runs, so the cost multiplies with the length of the text. The step's stated property is that a hostile font never hangs or allocates without bound. A 552-byte font that costs 4 GB and 9 s per glyph, more for every repeat in the string, breaks that in practice: the limits bound the charstring, not the drawing.

## fonts-2 (medium): A TrueType composite fan-out makes outline() of one glyph take seconds: 65536 visits, each re-walking up to 32767 contour ends

Status: fixed in 7c68bd4 (an outline counts each glyph it visits and each contour end, flag, and coordinate it reads against one budget of 2^20, takes a glyph's points from its last contour end before reading them, and is none once it's past the budget or the points; image_tests.cpp draws the finding's fan-out A in 0.01 s, 63 s before)

Where: runtime/src/font.cpp:476

add_outline caps visits at most_visits = 65536 and the path at most_points. But each visit to a simple glyph parses all of its endPtsOfContours (up to 32767) into a fresh vector before the 'path.size() + points > most_points' check, and the check is against what was added, not what was read. So after the path fills, each of the remaining ~65,500 visits still does about 32767 iterations and an allocation: roughly 2e9 steps for one glyph. A broken simple glyph is worse, because it adds nothing and never fills the path. Give it 65535 points whose flag runs are all there but whose coordinates are missing: every visit then pushes 65535 flags and allocates xs and ys before failing. That variant is 50 KB, takes 4.1 s at -O2, and draws nothing. Outlines aren't cached, so each copy of the character in a FillText or Width string repeats the cost.

```
// A 116 KB TrueType font whose "A" is a composite of 16 composites of 8192 parts each, every
// part one simple glyph of 32767 two-point contours: the reader's 65536 visits each walk all
// 32767 contour ends again before its point limit stops them.
Import std.core
Import std.math.color
Import std.math.vector
Import std.time
Import std.image

Function U8(b: UInt8[], v: Int)
    b.Add((v & 255) As UInt8)
End Function

Function U16(b: UInt8[], v: Int)
    U8(b, v >> 8)
    U8(b, v)
End Function

Function U32(b: UInt8[], v: Int)
    U16(b, (v >> 16) & 0xFFFF)
    U16(b, v & 0xFFFF)
End Function

Function Zeros(b: UInt8[], n: Int)
    For i = 0 Until n
        b.Add(0)
    End For
End Function

Function Composite(b: UInt8[], part: Int, count: Int)
    U16(b, 0xFFFF)
    Zeros(b, 8)
    For i = 0 Until count
        U16(b, i + 1 < count ? 0x0022 : 0x0002)
        U16(b, part)
        U16(b, 0)
    End For
End Function

Function FontBytes(): UInt8[]
    // glyf: 0 empty, 1 simple, 2 and 3 composites.
    Local glyf: UInt8[] = []
    Local loca: Int[] = [0, 0]
    U16(glyf, 32767)
    Zeros(glyf, 8)
    For i = 0 Until 32767
        U16(glyf, 2 * i + 1)
    End For
    U16(glyf, 0)
    Local left = 65534
    While left > 0
        Local k = left < 256 ? left : 256
        U8(glyf, 0x39)
        U8(glyf, k - 1)
        left -= k
    End While
    While glyf.Count % 4 != 0
        glyf.Add(0)
    End While
    loca.Add(glyf.Count)
    Composite(glyf, 1, 8192)
    loca.Add(glyf.Count)
    Composite(glyf, 2, 16)
    loca.Add(glyf.Count)

    Local locaTable: UInt8[] = []
    For offset In loca
        U32(locaTable, offset)
    End For
    Local headTable: UInt8[] = []
    U32(headTable, 0x00010000)
    Zeros(headTable, 8)
    U32(headTable, 0x5F0F3CF5)
    U16(headTable, 0)
    U16(headTable, 1000)
    Zeros(headTable, 30)
    U16(headTable, 1)
    U16(headTable, 0)
    Local hheaTable: UInt8[] = []
    U32(hheaTable, 0x00010000)
    U16(hheaTable, 800)
    U16(hheaTable, -200)
    Zeros(hheaTable, 26)
    U16(hheaTable, 4)
    Local maxpTable: UInt8[] = []
    U32(maxpTable, 0x00005000)
    U16(maxpTable, 4)
    Local hmtxTable: UInt8[] = []
    For i = 0 Until 4
        U16(hmtxTable, 500)
        U16(hmtxTable, 0)
    End For
    // cmap: format 4, "A" to glyph 3.
    Local cmapTable: UInt8[] = []
    For v In [0, 1, 3, 1, 0, 12, 4, 32, 0, 4, 0, 0, 0, 0x41, 0xFFFF, 0, 0x41, 0xFFFF,
              (3 - 0x41) & 0xFFFF, 1, 0, 0]
        U16(cmapTable, v)
    End For

    Local tags = [0x636D6170, 0x676C7966, 0x68656164, 0x68686561, 0x686D7478, 0x6C6F6361,
                  0x6D617870]
    Local tables = [cmapTable, glyf, headTable, hheaTable, hmtxTable, locaTable, maxpTable]
    Local font: UInt8[] = []
    U32(font, 0x00010000)
    U16(font, tables.Count)
    Zeros(font, 6)
    Local offset = 12 + 16 * tables.Count
    For i = 0 Until tables.Count
        U32(font, tags[i])
        U32(font, 0)
        U32(font, offset)
        U32(font, tables[i].Count)
        offset += (tables[i].Count + 3) / 4 * 4
    End For
    For table In tables
        For byte In table
            font.Add(byte)
        End For
        While font.Count % 4 != 0
            font.Add(0)
        End While
    End For
    Return font
End Function

Local bytes = FontBytes()
Local font = Try Font.Parse(bytes)
Print("font bytes:", bytes.Count)
Local canvas = Canvas(Image(16, 16))
Local start = Instant.Now()
canvas.FillText("A", font, 16, Vec2(2, 14), Color32.Black)
Print("one glyph:", start.Elapsed)
```

Expected: font bytes: 115632, then one glyph in well under a second: the reader's work per outline bounded by a fixed budget, as the visit and point limits mean it to be.

Actual: font bytes: 115632
one glyph: 57.8783934s  (with this build's Debug runtime. A harness that compiles font.cpp at -O2 and calls font_t::outline alone takes 3.229 s, giving 1081311 segments. The 50 KB variant with missing coordinates takes 4.134 s at -O2 and gives 0 segments.)

Suggested fix: Charge the work, not just the visits. Keep one per-outline budget (e.g. most_points) and take every contour end, flag and coordinate read out of it, failing the outline when it runs out. Or at least check 'path.size() + points' and the budget straight after reading the last contour end, before building last_points, flags, xs and ys, and stop the whole outline (not just that part) once the path is full. Add a font_tests case with a composite fan-out like this one and a time or work bound.

Skeptic: I ran the finding's composite fan-out program (115632 bytes) with fb run --release. It printed 'one glyph: 58.4163236s' for a single FillText of "A". In font.cpp's add_outline (lines 476-487), each visit to the simple glyph reads all 32767 endPtsOfContours into a new last_points vector before it checks 'path.size() + points > most_points'. Once the path is full, each of the remaining ~65K visits (most_visits = 2^16) still repeats that walk and allocation, about 2e9 iterations. The visit limit also ends the outline quietly rather than failing it. The comment on most_visits says it is there so such a font 'takes no longer than that', but the work per visit is not bounded, so one glyph from a 116 KB font takes about a minute in this build.

## text-1 (medium): A small hostile font makes FillText of one glyph at 12 px take 91 s and 6.5 GB

Status: fixed in e061d30, with fonts-1 (the same budget: B's 983025 pieces are past 65536, so it isn't drawn, and Z's curves off the image are a chord each; image_tests.cpp draws both of the finding's font at 12 pixels holding under 64 MB in 0.02 s, 6.5 GB and 91 s before)

Where: runtime/src/draw.cpp:650

The step's decision says a hostile font must never hang or allocate without bound. font.cpp caps a glyph's outline at most_points (1<<20) segments, but add_glyph_rings / add_quadratic / add_cubic then cut every curve into as many as most_chords (1024) chords. Nothing caps the total: not per glyph, not per text, and not by whether the glyph is anywhere near the image. fb_image_fill_text builds every ring and hands them all to fill(), which sorts all the edges and walks the dense rows. A 197 KB TrueType font does this: 'Z' is one contour of 65535 off-curve points zigzagging 30000 units across, and 'B' is a composite of 15 Zs, which stays under most_points. Drawing one 'B' at 12 px an em into a 16x16 image builds about 93 million chord points, and most of them are outside the image. With units per em of 16 (still accepted), each curve gets 750 chords and the same glyph would need over 10 GB. Measured with a release build on Windows, peak working set read by PowerShell.

```
Import std.core
Import std.time
Import std.math.color
Import std.math.vector
Import std.image
// A 197 KB font: Z is one contour of 65535 points off the curve, zigzagging 30000 units
// across; B is a composite of 15 Zs, under most_points.

Function U16(b: UInt8[], v: Int)
    b.Add(((v >> 8) & 255) As UInt8)
    b.Add((v & 255) As UInt8)
End Function

Function U32(b: UInt8[], v: Int64)
    U16(b, ((v >> 16) & 65535) As Int)
    U16(b, (v & 65535) As Int)
End Function

Function Pad(b: UInt8[])
    While b.Count % 4 != 0
        b.Add(0)
    End While
End Function

Function Zeros(b: UInt8[], n: Int)
    For i = 0 Until n
        b.Add(0)
    End For
End Function

Function Build(upem: Int, points: Int, parts: Int): UInt8[]
    Local head: UInt8[] = []
    U32(head, 0x00010000)
    U32(head, 0x00010000)
    U32(head, 0)
    U32(head, 0x5F0F3CF5)
    U16(head, 0)
    U16(head, upem)
    Zeros(head, 16)
    Zeros(head, 8)
    Zeros(head, 4)
    U16(head, 2)
    U16(head, 1)
    U16(head, 0)

    Local hhea: UInt8[] = []
    U32(hhea, 0x00010000)
    U16(hhea, 800)
    U16(hhea, 65536 - 200)
    U16(hhea, 0)
    Zeros(hhea, 24)
    U16(hhea, 3)

    Local maxp: UInt8[] = []
    U32(maxp, 0x00005000)
    U16(maxp, 3)

    Local hmtx: UInt8[] = []
    For i = 0 Until 3
        U16(hmtx, 500)
        U16(hmtx, 0)
    End For

    // cmap format 4: B -> 2, Z -> 1.
    Local cmap: UInt8[] = []
    U16(cmap, 0)
    U16(cmap, 1)
    U16(cmap, 3)
    U16(cmap, 1)
    U32(cmap, 12)
    Local segs = 3
    U16(cmap, 4)
    U16(cmap, 16 + segs * 8)
    U16(cmap, 0)
    U16(cmap, segs * 2)
    U16(cmap, 4)
    U16(cmap, 1)
    U16(cmap, 2)
    U16(cmap, 0x42)
    U16(cmap, 0x5A)
    U16(cmap, 0xFFFF)
    U16(cmap, 0)
    U16(cmap, 0x42)
    U16(cmap, 0x5A)
    U16(cmap, 0xFFFF)
    U16(cmap, (2 - 0x42 + 65536) % 65536)
    U16(cmap, (1 - 0x5A + 65536) % 65536)
    U16(cmap, 1)
    U16(cmap, 0)
    U16(cmap, 0)
    U16(cmap, 0)

    // glyf: 0 empty, 1 the zigzag, 2 the composite.
    Local glyf: UInt8[] = []
    Local start1 = glyf.Count
    U16(glyf, 1)
    Zeros(glyf, 8)
    U16(glyf, points - 1)
    U16(glyf, 0)
    Local flagged = 0
    While flagged < points
        glyf.Add(0x2C)
        glyf.Add(255)
        flagged += 256
    End While
    For i = 0 Until points
        U16(glyf, i % 2 == 0 ? 30000 : 65536 - 30000)
    End For
    For i = 0 Until points
        glyf.Add(1)
    End For
    Pad(glyf)
    Local start2 = glyf.Count
    U16(glyf, 65535)
    Zeros(glyf, 8)
    For i = 0 Until parts
        U16(glyf, i + 1 < parts ? 0x0023 : 0x0003)
        U16(glyf, 1)
        U16(glyf, 0)
        U16(glyf, 0)
    End For
    Pad(glyf)
    Local end2 = glyf.Count

    Local loca: UInt8[] = []
    U32(loca, 0)
    U32(loca, start1)
    U32(loca, start2)
    U32(loca, end2)

    Local tables = [head, hhea, maxp, hmtx, cmap, glyf, loca]
    Local tags = [0x68656164, 0x68686561, 0x6D617870, 0x686D7478, 0x636D6170, 0x676C7966, 0x6C6F6361]
    Local out: UInt8[] = []
    U32(out, 0x00010000)
    U16(out, tables.Count)
    U16(out, 0)
    U16(out, 0)
    U16(out, 0)
    Local offset = 12 + 16 * tables.Count
    For i = 0 Until tables.Count
        U32(out, tags[i])
        U32(out, 0)
        U32(out, offset)
        U32(out, tables[i].Count)
        offset += tables[i].Count
        offset += (4 - tables[i].Count % 4) % 4
    End For
    For t In tables
        For v In t
            out.Add(v)
        End For
        Pad(out)
    End For
    Return out
End Function

Local font = Try Font.Parse(Build(1000, 65535, 15))
For text In ["Z", "B"]
    Local canvas = Canvas(Image(16, 16))
    Local start = Instant.Now()
    canvas.FillText(text, font, 12, Vec2(0, 12), Color32.Black)
    Print(text, Instant.Now() - start)
End For
```

Expected: Both draws finish quickly, in milliseconds, with bounded memory. The font is either refused, or drawn under a bounded budget of chord points.

Actual: (fb run --release) Z 9.6942986s, then B 1m30.9436585s, with a peak working set of 6550 MB.

Suggested fix: Count the chords themselves against a budget, not just the outline's segments. Two ways: stop or skip a glyph once its chord points pass a cap, or have add_quadratic and add_cubic decrement a per-text budget shared with most_points. Also skip a glyph whose scaled bounds (from glyf's bbox or the outline's control points) miss the image, and clip edges to the image's rows before sorting. Add a test with a font like this one that checks the draw is fast and bounded.

Skeptic: I ran the finding's program as rv-step25/text/t1.fb with fb run --release. The font is 197568 bytes. Output: 'Z 10.4861832s', then 'B 1m31.2603125s'. I sampled t1.exe's PeakWorkingSet64 and got 6868967424 bytes (about 6.9 GB). The code matches the description. font.cpp:486 caps only the outline's segments, at most_points = 1<<20. In draw.cpp, add_quadratic and add_cubic (lines 603-655) clamp each curve to most_chords = 1024 chords, so one glyph can grow to about 1M x 1024 points. Nothing limits the total per glyph or per text, and nothing culls glyphs that fall outside the image. fb_image_fill_text (line 801) collects every glyph's rings and passes them all to fill(). Cost also grows with the text's length: 'BBBB' would need about 4x the memory. So a small hostile font can make FillText hang for minutes and allocate gigabytes. That breaks the lane's own rule that a hostile font never hangs or allocates without bound.

## fonts-3 (low): A broken composite glyph draws the parts read before the break, though font_t::outline's doc says a broken outline gives none

Status: fixed in 7c68bd4, with fonts-2 (add_outline gives false for a part cut short, or broken, and outline() clears the path, as cff_outline does; image_font_broken, the finding's program, prints 108 0, 108 108 before)

Where: runtime/src/font.cpp:558

font_internal.hpp documents outline() as giving 'none when it has no outline or its outline is broken', and cff_outline matches that by clearing the path when a charstring fails. In the TrueType composite loop, though, a record cut short (cursor + 4 > end, or a short argument, scale or matrix field) just returns from add_outline. The parts already added stay in the path, so a composite whose flags say MORE_COMPONENTS but whose data ends draws its first part. Simple glyphs and CFF glyphs that are broken draw nothing. The code and the doc disagree, and TrueType and CFF behave differently.

```
// A TrueType font of a square, glyph 1 for "A", and glyph 2 for "B", a composite of glyph 1
// whose flags say MORE_COMPONENTS but whose record ends there: a broken outline.
Import std.core
Import std.math.color
Import std.math.vector
Import std.image

Function FontBytes(): UInt8[]
    Return [0, 1, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 99, 109, 97, 112, 0, 0, 0, 0, 0, 0, 0, 124, 0,
            0, 0, 52, 103, 108, 121, 102, 0, 0, 0, 0, 0, 0, 0, 176, 0, 0, 0, 92, 104, 101, 97,
            100, 0, 0, 0, 0, 0, 0, 1, 12, 0, 0, 0, 54, 104, 104, 101, 97, 0, 0, 0, 0, 0, 0, 1,
            68, 0, 0, 0, 36, 104, 109, 116, 120, 0, 0, 0, 0, 0, 0, 1, 104, 0, 0, 0, 12, 108,
            111, 99, 97, 0, 0, 0, 0, 0, 0, 1, 116, 0, 0, 0, 16, 109, 97, 120, 112, 0, 0, 0, 0,
            0, 0, 1, 132, 0, 0, 0, 32, 0, 0, 0, 1, 0, 3, 0, 1, 0, 0, 0, 12, 0, 4, 0, 40, 0, 0,
            0, 6, 0, 0, 0, 0, 0, 0, 0, 65, 0, 66, 255, 255, 0, 0, 0, 65, 0, 66, 255, 255, 255,
            192, 255, 192, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 1,
            1, 1, 1, 0, 100, 0, 0, 1, 244, 0, 0, 0, 0, 2, 188, 0, 0, 253, 68, 0, 0, 0, 1, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 1, 1, 1, 1, 0, 100, 0, 0, 1, 244, 0, 0, 0, 0, 2, 188,
            0, 0, 253, 68, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 35, 0, 1, 0, 0, 0, 0, 0,
            0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 95, 15, 60, 245, 0, 0, 3, 232, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
            0, 0, 0, 0, 1, 0, 0, 3, 32, 255, 56, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 1, 244, 0, 0, 1, 244, 0, 0, 1, 244, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 36, 0, 0, 0, 72, 0, 0, 0, 92, 0, 1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
End Function

Function Covered(font: Font, text: String): Int
    Local canvas = Canvas(Image(16, 16))
    canvas.FillText(text, font, 16, Vec2(2, 14), Color32.Black)
    Local count = 0
    For y = 0 Until 16
        For x = 0 Until 16
            If canvas.Image[x, y].a != 0 Then count += 1
        End For
    End For
    Return count
End Function

Local font = Try Font.Parse(FontBytes())
Print(Covered(font, "A"), Covered(font, "B"))
```

Expected: 108 0 (the broken composite gives no outline, as the doc says and as a broken CFF or simple glyph does)

Actual: 108 108

Suggested fix: Have add_outline report failure (return bool), and have outline() clear the path when any part of a composite is cut short, the same as cff_outline. Or, if drawing the parts read so far is intended, change the font_internal.hpp comment to say so. Either way, add a test with a truncated composite.

Skeptic: I ran the finding's program in debug and release. Both printed '108 108': "B", a composite whose flags say MORE_COMPONENTS but whose data ends after its first record, draws exactly what "A" (the square) does. In font.cpp's composite loop, a record cut short ('cursor + 4 > end', a short arguments, scale or matrix field) just returns from add_outline, and outline() doesn't clear the path, so the parts already added stay. font_internal.hpp documents outline() as giving 'none when it has no outline or its outline is broken', and cff_outline clears the path on failure, so the TrueType composite path contradicts its own doc and behaves differently from CFF. A broken simple part inside a composite, or hitting most_visits, leaves a partial outline the same way.

## raster-2 (low): Shapes whose vertices are far outside the image are drawn in the wrong place, or not at all

Status: fixed in 629ec0c (fill cuts each ring to a box 2^20 pixels past the image, as Sutherland and Hodgman do, and a stroke cuts its line to that box and as far again as its joins and caps reach before stroking it, each crossing worked out to twice a double's bits, so a ring or line inside the box is itself, bit for bit; image_draw_far: the triangle with its corners 1e20 away draws the staircase they draw at 1e9, the line's (3, 3) and (0, 7) are 255 and 0, and a frame of corners that far is stroked where it runs)

Where: runtime/src/draw.cpp:57

edge_t::x_at interpolates from the edge's own top end: x0 + (y - y0) * (x1 - x0) / (y1 - y0). Inputs are only checked to be finite Floats, so endpoints can be up to 3.4e38. When the ends are far from the image, `y - y0` loses the pixel's y entirely, and x0 + ... cancels to within ulp(|x0|). At 1e20 that ulp is about 16384 pixels. A triangle covering the half-plane below the diagonal y = x, with its corners at +-1e20, draws nothing. A 2-wide line along that diagonal from (-1e20,-1e20) to (1e20,1e20) also draws nothing. With the corners at +-1e9 the same program draws the expected staircase, which shows the geometry itself is fine. Strokes from far-off points, the usual way to draw an infinite line or half-plane, go wrong once a coordinate passes about 1e14.

```
// The region left of and below the diagonal y = x, as a triangle whose other corners are far
// outside the image: each pixel on the diagonal is half covered, those below it wholly.
Import std.core
Import std.math.color
Import std.math.vector
Import std.image

Local canvas = Canvas(Image(8, 8))
canvas.FillPolygon([Vec2(-1e20, -1e20), Vec2(1e20, 1e20), Vec2(-1e20, 1e20)], Color32.Black)
For y = 0 Until 8
    Local row: Int[] = []
    For x = 0 Until 8
        row.Add(canvas.Image[x, y].a As Int)
    End For
    Print(row)
End For

// The line along that diagonal, 2 wide, from far outside the image.
Local line = Canvas(Image(8, 8))
line.LineWidth = 2
line.StrokeLine(Vec2(-1e20, -1e20), Vec2(1e20, 1e20), Color32.Black)
Print("line's (3, 3) and (0, 7):", line.Image[3, 3].a, line.Image[0, 7].a)
```

Expected: [128, 0, 0, 0, 0, 0, 0, 0]
[255, 128, 0, 0, 0, 0, 0, 0]
[255, 255, 128, 0, 0, 0, 0, 0]
[255, 255, 255, 128, 0, 0, 0, 0]
[255, 255, 255, 255, 128, 0, 0, 0]
[255, 255, 255, 255, 255, 128, 0, 0]
[255, 255, 255, 255, 255, 255, 128, 0]
[255, 255, 255, 255, 255, 255, 255, 128]
line's (3, 3) and (0, 7): 255 0
(This is what the same program prints with 1e9 in place of 1e20.)

Actual: [0, 0, 0, 0, 0, 0, 0, 0]  (all eight rows)
line's (3, 3) and (0, 7): 0 0

Suggested fix: Before rasterizing, clip each edge to a box a little larger than the image, computing the intersections precisely: double-double, or an exact two-product, so a far-off edge keeps where it crosses the image. Or set a coordinate range std promises to draw exactly (say |coordinate| <= 2^40), panic or document past it, and add a test of a far-off shape.

Skeptic: r2.fb is the finding's program. In debug it printed all eight rows as [0, 0, 0, 0, 0, 0, 0, 0] and "line's (3, 3) and (0, 7): 0 0". I added two controls. The same triangle at 1e9 gives row 3 as 255 128 0, which is correct, and at 1e15 it is still correct. So the failure starts somewhere between 1e15 and 1e20, not at 1e14 as the finding says. The cause is edge_t::x_at in draw.cpp (line 57), which interpolates from the far-off end: x0 + (y - y0)(x1 - x0)/(y1 - y0). At 1e20, ulp(x0) is far larger than a pixel. stdlib.md only panics for non-finite coordinates, so these finite inputs are accepted and silently drawn wrong, or not drawn at all. I rate it low because only coordinates far past any practical range are affected.

## raster-3 (low): StrokeEllipse with a line wider than its radius is a coarse polygon, far outside the hundredth-of-a-pixel tolerance

Status: fixed in 7fe270b (an ellipse's stroke is the polygon of the sides for its larger radius and half the line's width, in draw.cpp and image_draw's reference.py; image_draw's new thick ring, a radius of 0.5 stroked 12 wide, matches the reference, and its circle and ellipse strokes, regenerated, still do)

Where: runtime/src/draw.cpp:796

fb_image_stroke_ellipse strokes ellipse(center, rx, ry), whose side count quarter_sides picks for max(rx, ry) alone. The stroke's outer edge, though, is at radius r + w/2. The miter tips sit (w/2)(1/cos(pi/N) - 1) beyond it, and the chords' offsets sag inside it by the same angle. The comment there says a miter is "the curve's offset within the tolerance", and stdlib.md describes an ellipse as a polygon within a hundredth of a pixel. For StrokeCircle(Vec2(20, 20), 0.5) with LineWidth 30, the radius-0.5 polygon has only 16 sides. The 15.5-radius disk it should cover comes out a 16-gon whose corners stick out about 0.29 px. Exact coverage, by numeric integration of the disk, gives alpha 125, 125, and 102 at (35,19), (35,20), and (31,30). FillCircle of 15.5 at the same center gives 123, 123, and 100, within its own tolerance. The stroke gives 177, 177, and 157. A thick ring around a small center is ordinary use, such as a dot with a heavy outline.

```
// A circle of radius 0.5 stroked 30 wide covers the disk of radius 15.5; FillCircle of 15.5 for
// comparison.
Import std.core
Import std.math.color
Import std.math.vector
Import std.image

Local thick = Canvas(Image(40, 40))
thick.LineWidth = 30
thick.StrokeCircle(Vec2(20, 20), 0.5, Color32.Black)
Print("thick ring's (35, 19), (35, 20), (31, 30):", thick.Image[35, 19].a, thick.Image[35, 20].a, thick.Image[31, 30].a)
Local disk = Canvas(Image(40, 40))
disk.FillCircle(Vec2(20, 20), 15.5, Color32.Black)
Print("disk's (35, 19), (35, 20), (31, 30):", disk.Image[35, 19].a, disk.Image[35, 20].a, disk.Image[31, 30].a)
```

Expected: thick ring's (35, 19), (35, 20), (31, 30): 125 125 102   (exact; within a couple of levels, as the disk is)
disk's (35, 19), (35, 20), (31, 30): 123 123 100

Actual: thick ring's (35, 19), (35, 20), (31, 30): 177 177 157
disk's (35, 19), (35, 20), (31, 30): 123 123 100

Suggested fix: In fb_image_stroke_ellipse, pick the polygon's sides for max(rx, ry) + line_width / 2, the stroke's outer radius, not the ellipse's. quarter_sides would need that radius passed in, and reference.py should do the same. Add an image_draw case of a stroke wider than its radius.

Skeptic: r3.fb is the finding's program. In debug it printed "thick ring's (35, 19), (35, 20), (31, 30): 177 177 157" and "disk's ...: 123 123 100". By hand, the exact coverage of (35,19) for a 15.5-radius disk at (20,20) is about 0.49, alpha 125, so the stroke is about 52 levels high. The cause is in fb_image_stroke_ellipse (draw.cpp, around line 796). It strokes ellipse(center, rx, ry), whose sides quarter_sides picks from radius 0.5 alone, giving a 16-gon. With w/2 = 15, the miter tips sit at 15.49/cos(pi/16), about 15.79, roughly 0.29 px past the true outer circle. The code's comment says the miter is "the curve's offset within the tolerance", and that is false here. stdlib.md literally defines the stroke as the polygon's stroke with miters, and the reference matches it, so the doc's wording is followed. But the stroke's outline is visibly polygonal and not within the hundredth of a pixel stdlib.md gives for ellipses. Ordinary widths, at or below the radius, stay near the tolerance, so I rate it low.

## raster-4 (low): Ellipses of radius past about 34000 px aren't within the hundredth of a pixel the docs promise (1024 sides a quarter at most)

Status: fixed in 7e51ce5 (a quarter of an ellipse has up to 32768 sides, which keep one of a radius up to 34 million pixels within the tolerance, as stdlib.md and draw.cpp now say, and reference.py's most is the same; image_draw_far's circle of radius 100000 gives (76, 8) 246, within a hundredth of a pixel of the circle's 0.9711, 245 to 250, 240 before)

Where: runtime/src/draw.cpp:67

quarter_sides stops at most_sides = 1024. A chord of pi/2048 radians is r(1 - cos(pi/4096)), about 2.94e-7 r, from its arc, so above r of about 34000 the polygon is more than 0.01 px from the ellipse. That breaks the promise in draw.cpp's header, in stdlib.md ("An ellipse is a polygon within a hundredth of a pixel of it"), and in the reference's claim. A radius-100000 circle whose top runs along y = 8 is 0.0294 px inside the arc in the middle of its first chord. Exact coverage of pixel (76, 8), integrated numerically, is 0.97074, which is alpha 248. The canvas draws 240. Large radii are how a gentle arc across an image gets drawn.

```
// A circle of radius 100000 whose top passes along y = 8.
Import std.core
Import std.math.color
Import std.math.vector
Import std.image

Local canvas = Canvas(Image(100, 16))
canvas.FillCircle(Vec2(0, 100008), 100000, Color32.Black)
Print("big circle's (76, 8):", canvas.Image[76, 8].a)
```

Expected: big circle's (76, 8): 248

Actual: big circle's (76, 8): 240

Suggested fix: Pick the side count from the tolerance with no fixed cap. To keep the work bounded, generate only the arc within the image's bounds plus the stroke width, or cap at a much higher count. Or state in stdlib.md and draw.cpp's header the radius up to which the hundredth-of-a-pixel bound holds.

Skeptic: r4.fb (debug) printed "big circle's (76, 8): 240". By hand: the true arc at x=76 is at y = 100008 - sqrt(1e10 - 5776), about 8.0289, so coverage is 0.9711, alpha 248. The capped polygon's first chord runs from (0, 8) to about (153.4, 8.1177). At x=76 it is at 8.058, coverage 0.942, alpha 240, which is exactly what the canvas drew. quarter_sides (draw.cpp, line 67) stops at most_sides = 1024 a quarter. The sag is then r(1 - cos(pi/4096)), about 2.94e-7 r, which passes 0.01 px once r is above about 34000. That contradicts stdlib.md's "An ellipse is a polygon within a hundredth of a pixel of it" and draw.cpp's header. Such radii are rare, so I rate it low.

## Refuted by the skeptics (no action)

- text-2: Width and the metrics return Inf for a finite size and text instead of panicking. I reproduced the behavior. rv-step25/text/t2.fb prints 'Inf Inf 2.4e38 Inf' in both debug and release, for Width("AAAAAA",1e38), LineHeight(3.3e38), Ascent(3e38) and LineHeight(3.4e38). This is not a bug, though. It is ordinary IEEE Float overflow: the result is a finite size times a ratio, rounded to Float32. docs/spec has no rule that Float arithmetic which overflows panics. Width's doc only promises a panic for a size that isn't finite or is negative, and it never promises a finite result. CornersOf panics because the rasterizer needs finite coordinates to draw. That is a precondition of drawing, not a rule for measuring functions. Nothing in stdlib.md or milestones row 25 asks metrics to panic on overflow.
- text-3: No test covers FillText's non-finite-position panic or Width's size panic. Both panics work as documented. rv-step25/text/t3.fb prints 'panic: text is drawn from a finite position, not (NaN, 7) at t3.fb:10:1' (exit 101). t3b.fb prints 'panic: a font's size is finite and not negative, not -1 at t3b.fb:6:7'. A grep confirms the gap is real: 'finite position' appears only in std/image/canvas.fb, and no test calls Width with a bad size. panic_image_text_size.fb only calls Width("AV", 0), which is valid. But milestones row 25's done-when is 'drawn images match reference images pixel for pixel'. Neither that row nor stdlib.md nor the spec requires a test for each documented panic; only some other steps' rows, such as step 3, say so. The behavior is correct and no documented rule is broken, so this is a coverage suggestion, not a confirmed defect.
