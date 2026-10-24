# The review of M6 step 22's codecs over bytes: Deflate, Zlib, Gzip, Zstd, Lz4, and XZ compress and decompress UInt8[] (one-shot and Compressor/Decompressor Classes), Zip and Tar read and write, 7z reads (the Reader and Writer forms wait for step 13's streams)

25 confirmed findings: 3 high, 11 medium, 11 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S22-7z-docs-1 (high): A branch filter (BCJ, ARM, ...) on LZMA without an end marker, as 7-Zip writes LZMA:BCJ, reads as UNEXPECTED_END

Status: fixed in 4211691: decode() makes a 7z's LZMA coder liblzma's LZMA1EXT, given the coder's unpacked size and LZMA_LZMA1EXT_ALLOW_EOPM, so it ends at its size with or without the end marker and a branch filter after it gives its last bytes; compress_archives.fb reads bcj.7z, BCJ then LZMA with no end marker (make_archives.py's literal-only encoder writes it), which was UNEXPECTED_END before

Where: runtime/src/seven_zip.cpp:681

decode() builds LZMA1 coders as LZMA_FILTER_LZMA1 through lzma_properties_decode, which leaves the uncompressed size unknown. 7-Zip writes a 7z's LZMA stream with no end-of-payload marker, because the folder's unpack size says where it ends. Without the marker, liblzma's raw LZMA1 decoder never returns LZMA_STREAM_END. A plain LZMA folder still works, because the loop stops once output.size() reaches the size. A branch filter in front of it does not: liblzma's simple filters (x86, ARM, ARM Thumb, PowerPC, SPARC, IA-64, ARM64, RISC-V) hold back the last few unfiltered bytes until the next filter reports its end. The output stays a few bytes short, lzma_code gives LZMA_BUF_ERROR, and the file reads as UNEXPECTED_END. LZMA:BCJ is 7-Zip's chain for executables with -m0=LZMA and is common in older archives. The tests miss this because Python's lzma always writes the end marker: chain.7z uses BCJ+LZMA2, and delta.7z's LZMA1 has the marker, with Delta holding nothing back. So the done-when 'reads what common tools wrote' isn't checked for this chain. Verified: the same bytes with an ARM filter in place of BCJ also fail, an LZMA-only folder of the same stream reads fine, and bsdtar -xOf extracts the file. I built the archive with a literal-only LZMA encoder that writes no end marker, checked against Python's .lzma decoder with the size known. Debug and release builds behave the same.

```
// A 7z of text.txt, 31 bytes, by x86's BCJ then LZMA, as 7-Zip writes an LZMA folder: no end
// marker, since the folder's size says where it ends (bsdtar -xOf reads it).
Import std.core
Import std.compress

Local archive: UInt8[] = [
    0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04, 0x85, 0x56, 0x9e, 0x0a, 0x24, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x77, 0xbe, 0x87, 0x79, 0x00, 0x38, 0x1b, 0x08, 0x47, 0x82, 0xb2, 0x9c, 0xfe, 0x38,
    0x47, 0x2a, 0x03, 0x6c, 0x37, 0x4f, 0x06, 0x06, 0x32, 0xa5, 0x6a, 0x61, 0x57, 0xd2,
    0xf2, 0x27, 0x2d, 0x17, 0x89, 0x2a, 0x87, 0xf7, 0xb0, 0xf7, 0xb6, 0x00, 0x01, 0x04,
    0x06, 0x00, 0x01, 0x09, 0x24, 0x00, 0x07, 0x0b, 0x01, 0x00, 0x02, 0x23, 0x03, 0x01,
    0x01, 0x05, 0x5d, 0x00, 0x00, 0x01, 0x00, 0x04, 0x03, 0x03, 0x01, 0x03, 0x01, 0x00,
    0x0c, 0x1f, 0x1f, 0x0a, 0x01, 0xdf, 0xe5, 0xde, 0x29, 0x00, 0x08, 0x0d, 0x01, 0x09,
    0x00, 0x00, 0x05, 0x01, 0x0e, 0x01, 0x00, 0x0f, 0x00, 0x11, 0x13, 0x00, 0x74, 0x00,
    0x65, 0x00, 0x78, 0x00, 0x74, 0x00, 0x2e, 0x00, 0x74, 0x00, 0x78, 0x00, 0x74, 0x00,
    0x00, 0x00, 0x00, 0x00
]
Try
    Local reader = SevenZip.Read(archive)
    Local bytes = reader.ReadAt(0)
    Print(reader.Entries[0].Name, bytes.Count, bytes == "plain text, no branch opcodes.\n".Bytes)
Catch e As IOError
    Print(e.Kind, e.Message)
End Try
```

Expected: text.txt 31 True

Actual: UNEXPECTED_END the 7z data ends too soon

Suggested fix: Decode a 7z LZMA coder as LZMA_FILTER_LZMA1EXT, with an lzma_options_lzma from lzma_properties_decode plus ext_size_low/ext_size_high set to that coder's unpack size (folder.sizes[index]) and ext_flags = LZMA_LZMA1EXT_ALLOW_EOPM. LZMA1 then reports LZMA_STREAM_END at its size and the branch filter flushes its tail. Add a 7z to make_archives.py whose LZMA has no end marker behind BCJ (Python's lzma can't write one, so use a small literal-only encoder, or an archive 7-Zip wrote with -m0=LZMA on x86 code).

Skeptic: Copied the program to C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s22/7z-verify/f1.fb. Debug and release both print 'UNEXPECTED_END the 7z data ends too soon'. I wrote its bytes to f1.7z: Windows' bsdtar 3.8.9 lists text.txt at 31 bytes, and -xOf prints 'plain text, no branch opcodes.\n'. Python's lzma (chk1.py) shows the stream is valid and has no end marker. As .lzma with the size known it decodes all 31 bytes and reaches eof. Raw LZMA1 alone gives 31 bytes, eof False. Raw [x86, LZMA1] gives only 27 bytes: the BCJ filter holds back its tail because LZMA1 never reports its end. decode() in seven_zip.cpp (lines 681-689, 748-765) builds LZMA_FILTER_LZMA1 with no size, so output stays short and lzma_code returns LZMA_BUF_ERROR, which becomes RESULT_ENDS_TOO_SOON. 7-Zip writes 7z LZMA with no end marker, and LZMA:BCJ was its default for executables before LZMA2. stdlib.md says branch filters are read 'in chains, as 7-Zip writes them', and the milestones row says 'reads what common tools wrote', so this breaks both.

## S22-codecs-1 (high): Zstd and Lz4 can't decompress data whose size is a multiple of 64 KiB, their own output included

Status: fixed in eb84e01: Zstd's and Lz4's decoders go on past their input's end until the frame ends, rather than stopping when output fills a chunk; compress_codecs.fb decompresses 64 KiB and 128 KiB of each format at once and through a Decompressor

Where: runtime/src/zstd.cpp:82

In zstd.cpp's and lz4.cpp's decompress_all (lz4.cpp line 93), a call whose output exactly fills the 64 KiB chunk sets `full`, and the loop calls the library once more with no input. When the frame has already ended (hint 0), that extra call returns the next frame header's size as its hint, and `decoder.ended = hint == 0` (zstd.cpp:103, lz4.cpp:108) sets ended back to false. Finish then reports RESULT_ENDS_TOO_SOON. So Zstd.Decompress(Zstd.Compress(x)) and Lz4.Decompress(Lz4.Compress(x)) throw UNEXPECTED_END for every x of 65536, 131072, 196608 (and so on) bytes. The same happens through ZstdDecompressor and Lz4Decompressor, and in Tar.Read of a .tar.zst or .tar.lz4 whose tar is a multiple of 64 KiB (a 64 KiB tar fails). Deflate, Zlib, Gzip, and XZ handle the same sizes correctly: zlib.cpp's loop keeps going for a full chunk only while `!decoder.ended`. compress_codecs.fb's inputs are 0, 1, 1542, 100000, and 200000 bytes, none of them a multiple of 65536, so the round-trip done-when misses this. The bug is the same in debug and release.

```
Import std.core
Import std.collections
Import std.compress

// 64 KiB of text: a Zstd frame and an Lz4 frame of it, made by std itself.
Local data: UInt8[] = []
data.Resize(65536, 65)
Local zstd = Zstd.Compress(data)
Local lz4 = Lz4.Compress(data)

Try
    Print("Zstd.Decompress:", (Try Zstd.Decompress(zstd)).Count)
Catch e As IOError
    Print("Zstd.Decompress:", e.Kind, e.Message)
End Try

Try
    Print("Lz4.Decompress:", (Try Lz4.Decompress(lz4)).Count)
Catch e As IOError
    Print("Lz4.Decompress:", e.Kind, e.Message)
End Try

// The same through a Decompressor: the whole frame in one Add, then Finish.
Try
    Local d = ZstdDecompressor()
    Local all = Try d.Add(zstd)
    all.AddAll(Try d.Finish())
    Print("ZstdDecompressor:", all.Count)
Catch e As IOError
    Print("ZstdDecompressor:", e.Kind, e.Message)
End Try

Try
    Local d = Lz4Decompressor()
    Local all = Try d.Add(lz4)
    all.AddAll(Try d.Finish())
    Print("Lz4Decompressor:", all.Count)
Catch e As IOError
    Print("Lz4Decompressor:", e.Kind, e.Message)
End Try

// A .tar.zst and a .tar.lz4 of a 64 KiB tar.
Local file: UInt8[] = []
file.Resize(65536 - 512 - 1024, 66)
Local writer = TarWriter()
writer.Add("file.bin", file)
Local tar = writer.Finish()
Try
    Print(".tar.zst:", (Try Tar.Read(Zstd.Compress(tar))).Count, "entries")
Catch e As IOError
    Print(".tar.zst:", e.Kind, e.Message)
End Try
Try
    Print(".tar.lz4:", (Try Tar.Read(Lz4.Compress(tar))).Count, "entries")
Catch e As IOError
    Print(".tar.lz4:", e.Kind, e.Message)
End Try
```

Expected: Zstd.Decompress: 65536
Lz4.Decompress: 65536
ZstdDecompressor: 65536
Lz4Decompressor: 65536
.tar.zst: 1 entries
.tar.lz4: 1 entries

Actual: Zstd.Decompress: UNEXPECTED_END the Zstd data ends too soon
Lz4.Decompress: UNEXPECTED_END the Lz4 data ends too soon
ZstdDecompressor: UNEXPECTED_END the Zstd data ends too soon
Lz4Decompressor: UNEXPECTED_END the Lz4 data ends too soon
.tar.zst: UNEXPECTED_END the Zstd data ends too soon
.tar.lz4: UNEXPECTED_END the Lz4 data ends too soon

Suggested fix: In both decompress_all loops, go round again for a full chunk only while the frame hasn't ended, as zlib.cpp does: `while (in.pos < in.size || (full && !decoder.ended))` in zstd.cpp and `while (at < data.size() || (full && !decoder.ended))` in lz4.cpp. Another way is to leave `ended` alone when a call took no input and made no output. Add 65536- and 131072-byte inputs to compress_codecs.fb, both at once and through the Decompressors.

Skeptic: I ran the finding's program, plus some controls, as C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s22/codecs-v/f1.fb. Debug and release printed the same thing. Zstd.Decompress, Lz4.Decompress, ZstdDecompressor and Lz4Decompressor all gave 'UNEXPECTED_END the Zstd/Lz4 data ends too soon' for 65536 bytes. So did Tar.Read of a .tar.zst and of a .tar.lz4 of a tar that is exactly 65536 bytes. Inputs of 131072 and 196608 bytes also failed with UNEXPECTED_END. 65535 and 65537 round-trip fine, and Gzip and XZ handle 65536. The cause is in zstd.cpp:82 (`while (in.pos < in.size || full)`) and lz4.cpp:93. When the output fills the chunk exactly, the loop calls the library once more with no input. That call's hint is not 0, so `decoder.ended = hint == 0` (zstd.cpp:103, lz4.cpp:108) sets ended back to false. zlib.cpp's loop avoids this because it also checks `!decoder.ended`.

## S22-zip-tar-1 (high): ZipWriter takes quadratic time in the size of the zip

Status: fixed in b3817cb: a ZipWriter writes through an mz_stream of zip.cpp's own over a std::vector, which grows as a vector does; program_tests.cpp's "a ZipWriter takes time in proportion to the zip's size" writes 128 MiB stored in a debug build in under 8 seconds, which took about 20 before

Where: runtime/src/zip.cpp:263

start() sets minizip-ng's memory stream to grow by a fixed 64 KiB (chunk_size). Each time mz_stream_mem_write fills the buffer, it mallocs a new one that is only 64 KiB bigger and copies the whole zip into it. fb_zip_writer_add writes 64 KiB at a time, so every chunk of output copies everything written before it, which is O(n^2). Measured with --release, on a stored entry of n MiB of 7s: the zip took 0.38 s at 16 MiB, 1.3 s at 32, 4.9 s at 64 and 18.8 s at 128, four times as long for each doubling. TarWriter took 0.22, 0.41, 0.78 and 1.42 s for the same sizes. Projected from that, 1 GiB would take about 20 minutes. DEFLATE of incompressible data (photos, archives) has the same cost: 32 MiB of noise took 2.75 s and 64 MiB took 9.4 s, against 1.95 s and 3.5 s for TarWriter plus Gzip.Compress. Reading is linear: a 128 MiB zip read in 0.57 s.

```
Import std.core
Import std.collections
Import std.compress

// Writing a zip of 128 MiB takes about as long as writing a tar of them, and twice the bytes
// take about twice the time. Run with --release, timed from outside, at 16, 32, 64, and 128.
Local data: UInt8[] = []
data.Resize(128 * 1024 * 1024, 7)
Local writer = ZipWriter()
writer.Add("big.bin", data, ZipMethod.STORE)
Print(writer.Finish().Count > data.Count)
```

Expected: True, in about the time a TarWriter takes for the same data (1.42 s at 128 MiB), with time roughly doubling when the size doubles.

Actual: True after 18.8 s. At 16, 32, 64 and 128 MiB it takes 0.38, 1.3, 4.9 and 18.8 s, four times as long for each doubling.

Suggested fix: Make the buffer grow geometrically. Before each entry's mz_zip_entry_write_open, and before mz_zip_close, call mz_stream_mem_set_grow_size with max(chunk_size, the buffer's current length from mz_stream_mem_get_buffer_length), or with at least the data's size plus headers. Clamp it so that mz_stream_mem_write's int32 new_size (size + grow_size) can never pass INT32_MAX, and panic first, saying the zip is more than a UInt8[] holds. Another option is to give minizip-ng a small mz_stream of zip.cpp's own over a std::vector<uint8_t>.

Skeptic: Built the STORE program with --release at 16, 32 and 64 MiB, plus the same thing through TarWriter. ZipWriter took 0.38 s, 1.27 s and 4.93 s. TarWriter took 0.20 s, 0.37 s and 0.70 s. So the zip time roughly quadruples each time the size doubles. The code explains it: zip.cpp:263 calls mz_stream_mem_set_grow_size(chunk_size), so the buffer grows by a fixed 64 KiB. In mz_strm_mem.c:115-129, new_size = mem->size + grow_size when size < grow_size, and mz_stream_mem_set_size mallocs a new buffer and copies the old one into it. fb_zip_writer_add writes 64 KiB chunks, so every chunk past the end copies the whole zip written so far, which is O(n^2).

## S22-7z-docs-2 (medium): A bind pair naming a coder past the folder's last crashes decode() instead of giving INVALID_DATA

Status: fixed in e7597c0: a folder's bind pairs and packed streams name only its coders' inputs and outputs, or the 7z is INVALID_DATA; compress_archives.fb reads bind.7z, whose bind pair names a coder past its last

Where: runtime/src/seven_zip.cpp:671

read_folder() never checks that a bind pair's input or output index, or a packed stream's index, is below the folder's input or output count. decode() follows the chain of binds and checks only that the chain is no longer than the coder count and that it ends at the packed input. The indices between are not checked. Example: three Copy coders with binds (0 <- 0x10000000) and (0x10000000 <- 1). The chain is [0, 0x10000000, 1], it ends at the unbound input 1, and folder.coders[0x10000000] is then indexed. In this build the libstdc++ assertion aborts the process with 0xC0000409. Without assertions it is an out-of-bounds read 16 GiB past the vector. A 7z whose header CRC is right but whose content is crafted takes the program down instead of throwing an IOError, in both debug and release.

```
// A 7z of three Copy coders whose bind pairs go through a coder index of 0x10000000; the chain
// still ends at the packed stream, so decode() indexes coders[0x10000000].
Import std.core
Import std.compress

Local archive: UInt8[] = [
    0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04, 0x5c, 0x7c, 0x01, 0xf9, 0x0b, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xdc, 0xa7, 0x0d, 0x16, 0x66, 0x69, 0x72, 0x73, 0x74, 0x20, 0x66, 0x69, 0x6c, 0x65,
    0x0a, 0x01, 0x04, 0x06, 0x00, 0x01, 0x09, 0x0b, 0x00, 0x07, 0x0b, 0x01, 0x00, 0x03,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x10, 0xf0, 0x00,
    0x00, 0x00, 0x10, 0x01, 0x0c, 0x0b, 0x0b, 0x0b, 0x0a, 0x01, 0xdf, 0x30, 0xdb, 0xf4,
    0x00, 0x08, 0x0d, 0x01, 0x09, 0x00, 0x00, 0x05, 0x01, 0x0e, 0x01, 0x00, 0x0f, 0x00,
    0x11, 0x11, 0x00, 0x6f, 0x00, 0x6e, 0x00, 0x65, 0x00, 0x2e, 0x00, 0x74, 0x00, 0x78,
    0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00
]
Try
    Local reader = SevenZip.Read(archive)
    Print(reader.Count, (Try reader.ReadAt(0)).Count)
Catch e As IOError
    Print(e.Kind, e.Message)
End Try
Print("done")
```

Expected: INVALID_DATA the 7z data isn't valid: a folder's coders don't fit together
done

Actual: stl_vector.h:1282: ... operator[] [with _Tp = {anonymous}::coder_t ...]: Assertion '__n < this->size()' failed.
error: f2_bad_bind.exe exited with code 0xC0000409 (nothing printed)

Suggested fix: In read_folder, return broken("a folder's coders don't fit together") when a bind pair's input is >= inputs or its output is >= outputs, or when a packed index read for packed > 1 is >= inputs. Optionally also check index < folder.coders.size() in decode's chain loop. Add this archive to the broken-7z tests.

Skeptic: Ran 7z-verify/f2.fb. Debug and release both abort before printing anything: 'stl_vector.h:1282 ... operator[] [with _Tp = {anonymous}::coder_t] ... Assertion __n < this->size() failed', then 'exited with code 0xC0000409'. 'done' is never printed. Read the code. read_folder (lines 313-332) stores bind pairs and packed indices without checking them against the input and output counts. decode (lines 654-667) checks only the chain's length and that it ends at packed[0]. With binds (0<-0x10000000) and (0x10000000<-1), the chain is [0, 0x10000000, 1], and line 671 indexes folder.coders[0x10000000]. Crafted data with a correct header CRC crashes the program instead of throwing INVALID_DATA. Without libstdc++ assertions this would be an out-of-bounds read.

## S22-7z-docs-3 (medium): SevenZip.Read decodes a link's whole folder once per link, and keeps none of them, so opening takes links x folder size

Status: fixed in e7597c0: open() decodes each folder holding links once, takes every link's target from that, and keeps it for ReadAt when it fits; program_tests.cpp's "a 7z's links in one solid folder take one decoding of it" opens links.7z (64 MiB of zeros and 40 links) in a debug build in under 15 seconds, which took about 50 before, and compress_archives.fb reads its links' targets. A link whose folder doesn't decode still has Target "", and reading it gives the folder's error

Where: runtime/src/seven_zip.cpp:1036

open() fills in link targets by calling decode() on each link's folder into a fresh local vector, once per link. It neither shares that decode between links in the same folder nor keeps it in reader.decoded/folder_bytes. A solid folder with N symbolic links, as p7zip writes a Unix tree, is decoded N times before Read returns, and once more for the first ReadAt. Measured with a release build on an archive of one 64 MiB solid LZMA2 folder (zeros.bin plus 50 links to it): SevenZip.Read takes 51.3 s. The same bytes, with the links' attributes changed to plain files', open in 0.037 s, and bsdtar lists the link archive in 0.12 s. A folder of a few hundred MiB with a few hundred links would take hours to open. It also means Read can fail late or allocate a whole folder just to list entries, and a link whose folder fails to decode silently gets Target "".

```
See C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s22/7z-docs/open_links.fb (a complete 12 KB program; generated by gen/g7.py there). It builds the 7z from a 32-byte start header, a 224-byte LZMA2 chunk of 1 MiB of zeros that resets its state (so it is the same bytes each time) added 64 times, then an uncompressed LZMA2 chunk of 50 link targets ("zeros.bin" each) and the header: zeros.bin plus link0..link49, whose attributes are 0x8000 | (0o120777 << 16). It ends with:

Local archive = prefix.Clone()
For i = 1 To 64
    archive.AddAll(chunk)
End For
archive.AddAll(after)

Local reader = Try SevenZip.Read(archive)
Print(reader.Count, reader.Entries[1].Name, reader.Entries[1].Kind, reader.Entries[1].Target)

open_files.fb beside it is the same archive with plain-file attributes. Build both with fb build --release and time them.
```

Expected: 51 link0 LINK zeros.bin, printed in about one folder decode (well under a second), as with open_files.exe (0.037 s) or bsdtar -tvf (0.12 s)

Actual: 51 link0 LINK zeros.bin after 51.3 s (real time of open_links.exe)

Suggested fix: Fill in link targets folder by folder: decode each folder that holds a link once (in order of folder index), take every link's target from that one decode, and leave it in reader.decoded/folder_bytes so the first ReadAt reuses it. Or read targets lazily, on the first Target or Entries access. Add a test with several links in one solid folder.

Skeptic: Wrote my own generator, 7z-verify/gen_links.py: one solid LZMA2 folder of N MiB of zeros plus K 9-byte link targets. Python's lzma checks the folder decodes, and bsdtar lists the links as 'link0 -> zeros.bin'. Release builds, 16 MiB folder: 20 links take 4.76 s, 1 link 0.26 s, and 20 plain files of the same bytes 0.029 s. The time grows with links times folder size. The code shows why: open() at lines 1029-1045 calls decode() on the whole folder into a fresh local vector for each link. It never shares that decode or keeps it in reader.decoded/folder_bytes. A link whose folder fails to decode is skipped with Target "" (the 'continue' at line 1039). This contradicts the header comment and stdlib.md's 'a solid folder is decoded once'.

## S22-7z-docs-4 (medium): A small file in a solid folder past 2 GiB can't be read: OTHER 'comes to more than 2147483647 bytes'

Status: fixed in e7597c0: a solid folder of more bytes than an array holds is decoded, for each file read from it, up to that file's end, keeping only the file's bytes; compress_7z_solid.fb reads hello.txt from the end of a 2 GiB folder, and its first file, of more than an array holds, is an IOError (its parts are in compress_7z_solid/ since eff877c, as a program's test reads only its own folder)

Where: runtime/src/seven_zip.cpp:712

fb_xz_7z_reader_read decodes the file's whole folder, and decode() refuses any folder whose unpack size is more than most_bytes with RESULT_TOO_LARGE. A file of a few bytes that shares a solid folder with more than 2 GiB of other data therefore can't be read, though it fits an array easily. The IOError says the 7z data comes to more than 2147483647 bytes. 7-Zip's default solid block at its normal level is 2 GiB (2^31 bytes, one more than most_bytes), so large archives from 7-Zip's defaults hit this. docs/stdlib.md and the ReadAt comment don't mention it; they describe only an entry's own size, broken data, and unsupported coders. Verified with a 7z of zeros.bin (2^31 zeros) and hello.txt (6 bytes) in one LZMA2 folder. bsdtar -xOf prints hello from the same bytes.

```
// A 7z of one solid LZMA2 folder: 2 GiB of zeros (2048 LZMA2 chunks of 1 MiB that each
// reset their state, so they're the same bytes), then hello.txt, 6 bytes, in the same folder.
// bsdtar -xOf on these bytes prints "hello".
Import std.core
Import std.collections
Import std.compress

Local prefix: UInt8[] = [
    0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04, 0x78, 0xa3, 0x47, 0xe2, 0x0a, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x35, 0xe7, 0x97, 0x2a
]
Local chunk: UInt8[] = [
    0xef, 0xff, 0xff, 0x00, 0xd9, 0x5d, 0x00, 0x00, 0x6f, 0xfd, 0xff, 0xff, 0xa3, 0xb7,
    0xff, 0x47, 0x3e, 0x48, 0x15, 0x72, 0x39, 0x61, 0x51, 0xb8, 0x92, 0x28, 0xe6, 0xa3,
    0x86, 0x07, 0xf9, 0xee, 0xe4, 0x1e, 0x82, 0xd3, 0x2f, 0xc5, 0x3a, 0x3c, 0x01, 0x4b,
    0xb1, 0x7e, 0xc9, 0x8a, 0x8a, 0x4d, 0x2f, 0xa3, 0x0d, 0xd9, 0x7f, 0xa6, 0xe3, 0x8c,
    0x23, 0x11, 0x53, 0xe0, 0x59, 0x18, 0xc5, 0x75, 0x8a, 0xe2, 0x77, 0xf8, 0xb6, 0x94,
    0x7f, 0x0c, 0x6a, 0xc0, 0xde, 0x74, 0x49, 0x64, 0xe2, 0xe9, 0x5c, 0x53, 0xb2, 0x04,
    0xd8, 0xf7, 0x44, 0x0c, 0xab, 0x5f, 0x0d, 0x6d, 0x46, 0xe9, 0xe5, 0xc3, 0x76, 0x88,
    0xb7, 0x96, 0x57, 0xac, 0xb6, 0x4d, 0xe1, 0x69, 0x1d, 0x6f, 0xfb, 0x4b, 0x88, 0x10,
    0x6c, 0x42, 0xcb, 0x88, 0x3f, 0x5c, 0x00, 0x8f, 0xd0, 0x4e, 0xaf, 0x26, 0x28, 0x94,
    0x71, 0x1f, 0x3d, 0x8f, 0x24, 0xe1, 0x70, 0x9e, 0xa7, 0x23, 0x5f, 0xec, 0x28, 0xcb,
    0x85, 0xd1, 0x95, 0x98, 0x8a, 0x7e, 0x2a, 0x91, 0xf2, 0x27, 0x75, 0xf7, 0x19, 0xc0,
    0x06, 0x98, 0x4d, 0x98, 0xfd, 0xd8, 0xaf, 0xd5, 0x90, 0x0f, 0xc4, 0x25, 0x53, 0xf8,
    0xf5, 0x91, 0x36, 0x31, 0x05, 0xa5, 0xb0, 0xee, 0x6f, 0xc1, 0x70, 0x4d, 0x47, 0x0c,
    0xd1, 0x91, 0x11, 0xaa, 0xad, 0x60, 0x1d, 0xba, 0xce, 0xb1, 0x27, 0x18, 0x5c, 0x59,
    0x86, 0xe9, 0x66, 0x52, 0x58, 0xbe, 0xe9, 0x76, 0xac, 0x59, 0xe4, 0xe5, 0x5b, 0x05,
    0x08, 0xf9, 0xc7, 0xda, 0xad, 0xfc, 0xfb, 0x52, 0x2a, 0xf7, 0xe4, 0x95, 0x25, 0x00
]
Local header: UInt8[] = [
    0x01, 0x04, 0x06, 0x00, 0x01, 0x09, 0xc7, 0x0a, 0x00, 0x00, 0x07, 0x0b, 0x01, 0x00,
    0x01, 0x21, 0x21, 0x01, 0x10, 0x0c, 0xf0, 0x06, 0x00, 0x00, 0x80, 0x00, 0x08, 0x0d,
    0x02, 0x09, 0xf0, 0x00, 0x00, 0x00, 0x80, 0x0a, 0x01, 0x1c, 0xf2, 0xbd, 0x4d, 0x20,
    0x30, 0x3a, 0x36, 0x00, 0x00, 0x05, 0x02, 0x11, 0x29, 0x00, 0x7a, 0x00, 0x65, 0x00,
    0x72, 0x00, 0x6f, 0x00, 0x73, 0x00, 0x2e, 0x00, 0x62, 0x00, 0x69, 0x00, 0x6e, 0x00,
    0x00, 0x00, 0x68, 0x00, 0x65, 0x00, 0x6c, 0x00, 0x6c, 0x00, 0x6f, 0x00, 0x2e, 0x00,
    0x74, 0x00, 0x78, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00
]

Local archive = prefix.Clone()
For i = 1 To 2048
    archive.AddAll(chunk)
End For
Local tail: UInt8[] = [0x02, 0x00, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x0a, 0x00]
archive.AddAll(tail)
archive.AddAll(header)

Local reader = Try SevenZip.Read(archive)
For entry In reader.Entries
    Print(entry.Name, entry.Size)
End For

Try
    Local bytes = (Try reader.Read("hello.txt")) ?? []
    Print("hello.txt:", bytes.Count, bytes == "hello\n".Bytes)
Catch e As IOError
    Print("hello.txt:", e.Kind, e.Message)
End Try
```

Expected: zeros.bin 2147483648
hello.txt 6
hello.txt: 6 True

Actual: zeros.bin 2147483648
hello.txt 6
hello.txt: OTHER the 7z data comes to more than 2147483647 bytes

Suggested fix: Have decode take how many bytes it needs (file.offset + file.size) and where to start keeping them (file.offset), streaming the coders' output and dropping bytes before the file, so the limit applies to the entry's own size. Keep the whole-folder cache only when the folder fits. Otherwise, document in stdlib.md and the ReadAt comment that every file of a solid folder past 2 GiB is an IOError, and test it.

Skeptic: Ran 7z-verify/f4.fb. Debug and release both print 'zeros.bin 2147483648', 'hello.txt 6', then 'hello.txt: OTHER the 7z data comes to more than 2147483647 bytes'. I rebuilt the same 458888-byte archive as f4.7z (mk4.py). bsdtar lists both entries, and 'tar.exe -xOf f4.7z hello.txt' prints 'hello' in 2.8 s. The cause is decode() lines 712-714, which refuse any folder whose unpack size is over most_bytes before looking at which file is wanted. ReadAt therefore fails for a 6-byte entry that easily fits an array. Neither stdlib.md, the ReadAt comment in std/compress/seven_zip.fb, nor any listed lane decision mentions this: the limit decision covers codecs' Decompress, not an archive entry's folder. 7-Zip writes solid blocks this large at its higher levels and with -ms=on, so this misses 'reads what common tools wrote'.

## S22-codecs-2 (medium): ZstdCompressor panics when Finish follows only empty Adds

Status: fixed in eb84e01: ZstdCompressor's Add of an empty part gives nothing without asking zstd to continue, so Finish after empty Adds makes an empty frame; compress_codecs.fb has each format's Compressor given two empty parts

Where: runtime/src/zstd.cpp:134

compress_all calls ZSTD_compressStream2 even when Add's data is empty. With ZSTD_e_continue, that call still moves the CCtx out of its init stage, but `begun` stays false. fb_zstd_encoder_finish then calls ZSTD_CCtx_setPledgedSrcSize, which zstd refuses outside the init stage (stage_wrong), and check() panics with "std.compress's Zstd failed". So feeding a ZstdCompressor an empty part, such as an empty file read in parts, and then calling Finish crashes the program. Any frame that gets only empty Adds before its Finish is hit, including one after an earlier Finish. Lz4Compressor, XZCompressor, and the zlib Compressors handle the same sequence correctly (xz.cpp returns early for an empty LZMA_RUN). compress_codecs.fb never Adds an empty part: Parts() of the empty input gives no parts.

```
Import std.core
Import std.collections
Import std.compress

Local c = ZstdCompressor()
Local all: UInt8[] = []
all.AddAll(c.Add([]))
all.AddAll(c.Finish())
Print((Try Zstd.Decompress(all)).Count)
```

Expected: 0

Actual: panic: std.compress's Zstd failed
(exit code 101)

Suggested fix: Skip ZSTD_compressStream2 in compress_all when the data is empty and the directive is ZSTD_e_continue, as xz.cpp's encode_all does, so the stage stays init and the pledge works. Another way is to drop the explicit pledge: ZSTD_e_end on a CCtx still in its init stage already pledges the input's size itself. Add a test of Add([]) then Finish for every Compressor.

Skeptic: f2.fb (ZstdCompressor: Add([]) then Finish) panics with 'std.compress's Zstd failed' and exit code 101, in debug and release. In the control f2b.fb, Lz4, XZ and Gzip Compressors with Add([]) then Finish all decompress to 0 bytes. A ZstdCompressor that already finished a non-empty frame also panics on its next frame of Add([]) then Finish. In zstd.cpp, compress_all calls ZSTD_compressStream2 even for empty data while `begun` stays false. Finish (line 134) then calls ZSTD_CCtx_setPledgedSrcSize outside zstd's init stage, and check() panics.

## S22-zip-tar-2 (medium): A zip's LZMA or XZ entry allocates the 4 GiB dictionary its header claims, which XZ.Decompress refuses

Status: fixed in 9407c2b: a zip's LZMA (14) and XZ (95) entries are decoded over liblzma, LZMA's with its memory use checked first, both with no more than XZ.Decompress takes, so a 4 GiB dictionary is unsupported (OTHER, it needs more memory than an array holds); compress_archives.fb reads biglzma.zip and bigxz.zip

Where: runtime/src/zip.cpp:143

The design says memory a stream asks for beyond what an array holds is unsupported (OTHER), and that a header's sizes take no memory beyond what the data gives. XZ.Decompress follows this: an .xz stream whose LZMA2 dictionary byte says 4 GiB is OTHER. read_entry instead hands entries to minizip-ng's mz_strm_lzma, which opens lzma_alone_decoder and lzma_stream_decoder with a memlimit of UINT64_MAX. The same .xz stream as a zip XZ entry, or a zip LZMA entry whose properties say a 4 GiB dictionary, reads without error after liblzma allocates the dictionary. Measured peak commit (PeakPagedMemorySize64) was 4105 MiB for the program, against 1 MiB for the same program without the two zip reads. So a zip of a few hundred bytes makes Zip.Read(...).ReadAt allocate 4 GiB per entry. On a machine that can't commit that much, the result is INVALID_DATA "is broken" instead of OTHER. Zstd entries are not affected, because zstd's default 128 MiB window limit still applies.

```
Import std.core
Import std.collections
Import std.compress

// CRC-32, as an .xz block header's ends with.
Function Crc32(data: UInt8[], from: Int, count: Int): UInt32
    Local crc: UInt32 = 0xffffffff
    For i = from Until from + count
        crc = crc ^ (data[i] As UInt32)
        For bit = 1 To 8
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xedb88320 : crc >> 1
        End For
    End For
    Return crc ^ 0xffffffff
End Function

// Makes the .xz stream at `at` in data say its LZMA2 dictionary is 4 GiB, more than an array
// holds: its first block header's one property byte is 40, and its CRC-32 made again.
Function MakeGreedy(data: UInt8[], at: Int)
    Local header = at + 12
    Local size = ((data[header] As Int) + 1) * 4
    Assert(data[header + 2] == 0x21 && data[header + 3] == 0x01)
    data[header + 4] = 40
    Local crc = Crc32(data, header, size - 4)
    For i = 0 Until 4
        data[header + size - 4 + i] = ((crc >> (8 * i)) & 0xff) As UInt8
    End For
End Function

// Where a zip's first entry's data starts.
Function DataStart(zip: UInt8[]): Int
    Return 30 + (zip[26] As Int) + (zip[27] As Int) * 256 + (zip[28] As Int) + (zip[29] As Int) * 256
End Function

Function Outcome(zip: UInt8[]): String
    Try
        Return "{(Try (Try Zip.Read(zip)).ReadAt(0)).Count} bytes"
    Catch e As IOError
        Return "{e.Kind}: {e.Message}"
    End Try
End Function

Local one = "first file\n".Bytes

// The .xz stream alone, which XZ.Decompress doesn't ask the memory of.
Local xz = XZ.Compress(one)
MakeGreedy(xz, 0)
Try
    Print("XZ.Decompress:", XZ.Decompress(xz).Count)
Catch e As IOError
    Print("XZ.Decompress:", e.Kind, e.Message)
End Try

// A zip's XZ entry of the same stream, and its LZMA entry whose properties' dictionary, after
// the 2 bytes of the LZMA SDK's version and 2 of their size, and 1 of lc, lp, and pb, is 4 GiB.
Local writer = ZipWriter()
writer.Add("xz.txt", one, ZipMethod.XZ)
Local xzZip = writer.Finish()
MakeGreedy(xzZip, DataStart(xzZip))
Print("zip, XZ:", Outcome(xzZip))

writer.Add("lzma.txt", one, ZipMethod.LZMA)
Local lzmaZip = writer.Finish()
Local at = DataStart(lzmaZip)
For i = at + 5 Until at + 9
    lzmaZip[i] = 0xff
End For
Print("zip, LZMA:", Outcome(lzmaZip))
```

Expected: XZ.Decompress: OTHER the XZ data uses what isn't supported: it needs more memory than an array holds to decompress
zip, XZ: OTHER: the zip data uses what isn't supported: ...
zip, LZMA: OTHER: the zip data uses what isn't supported: ...
(with no 4 GiB allocation)

Actual: XZ.Decompress: OTHER the XZ data uses what isn't supported: it needs more memory than an array holds to decompress
zip, XZ: 11 bytes
zip, LZMA: 11 bytes
(peak commit 4105 MiB, against 1 MiB without the zip reads)

Suggested fix: For methods 14 and 95, don't let minizip-ng choose the memlimit. One way is to open the entry raw (mz_zip_entry_read_open(zip, 1, nullptr)) and decode it in zip.cpp with liblzma: lzma_alone_decoder for LZMA, after rebuilding the 13-byte .lzma header from the zip's 4 + 5 header bytes, and lzma_stream_decoder for XZ, using the memlimit XZ.Decompress uses and mapping LZMA_MEMLIMIT_ERROR to RESULT_UNSUPPORTED. Another way is to peek at the properties first: for LZMA, the dictionary size at bytes 5 to 8 of the data; for XZ, lzma_raw_decoder_memusage on the block header's filters. Add zips like these to compress_archives' greedy cases.

Skeptic: Ran the program unchanged. Debug output: 'XZ.Decompress: OTHER ... needs more memory than an array holds', then 'zip, XZ: 11 bytes' and 'zip, LZMA: 11 bytes'. I measured it with a job-object helper (jobpeak.cpp in the scratch dir). Peak commit was 4105 MiB, against 94 MiB for the same program without the two zip reads (f2b.fb). With the job limited to 1 GiB, both zip reads come back as 'INVALID_DATA: ... is broken', not OTHER. Cause: mz_strm_lzma.c:126/128 opens lzma_alone_decoder and lzma_stream_decoder with memlimit UINT64_MAX. This contradicts stdlib.md: data that needs more memory than an array holds to decompress, as an XZ dictionary can say, isn't supported.

## S22-zip-tar-3 (medium): A file whose MS-DOS attributes have the reparse-point bit reads as a link, and its bytes are lost

Status: fixed in 9407c2b: only an entry made by Unix, macOS, or RISC OS whose mode says link, or one with an extra field naming a target, is a link, and an MS-DOS reparse point is a file; compress_archives.fb reads reparse.zip

Where: runtime/src/zip.cpp:213

open() takes an entry's kind from mz_zip_entry_is_symlink. For an entry made by MS-DOS or NTFS, that function converts the attributes with mz_zip_attrib_win32_to_posix, which maps FILE_ATTRIBUTE_REPARSE_POINT (0x400) to S_IFLNK. Windows files that are reparse points without being links carry that bit: OneDrive cloud files, and files on deduplicated volumes. Windows zip tools that copy the file's attributes write it. Such an entry becomes a LINK with Size 0, its Target is the file's whole content, and ReadAt gives no bytes. bsdtar, Python's zipfile and Info-ZIP read it as a 21-byte file. MS-DOS attributes have no symlink.

```
Import std.core
Import std.compress

// A zip Python's zipfile wrote, of one file, report.txt, made by MS-DOS with the attributes
// 0x420: archive, and Windows' reparse point, as a file in a OneDrive folder has. It's a file,
// as bsdtar, Info-ZIP's unzip, and Python read it: there's no link in MS-DOS's attributes.
Local data: UInt8[] = [
    0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x5c,
    0xbe, 0x66, 0x3e, 0xa4, 0x15, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x72, 0x65, 0x70, 0x6f, 0x72, 0x74, 0x2e, 0x74, 0x78, 0x74, 0x74, 0x68,
    0x65, 0x20, 0x66, 0x69, 0x6c, 0x65, 0x27, 0x73, 0x20, 0x6f, 0x77, 0x6e, 0x20, 0x62,
    0x79, 0x74, 0x65, 0x73, 0x0a, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x5c, 0xbe, 0x66, 0x3e, 0xa4, 0x15, 0x00, 0x00,
    0x00, 0x15, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x20, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x65, 0x70, 0x6f, 0x72,
    0x74, 0x2e, 0x74, 0x78, 0x74, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x38, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x00, 0x00, 0x00, 0x00
]
Local reader = Try Zip.Read(data)
Local entry = reader.Entries[0]
Print(entry.Name, entry.Kind, entry.Size, (Try reader.ReadAt(0)).Count, "[{entry.Target}]")
```

Expected: report.txt FILE 21 21 []

Actual: report.txt LINK 0 0 [the file's own bytes
]

Suggested fix: Count an entry as a link only when MZ_HOST_SYSTEM(info->version_madeby) is Unix, OS X or RISC OS and the high 16 bits of the external attributes have S_IFLNK. For any other system, it's a folder by the 0x10 bit or a trailing separator, and a file otherwise. Add an MS-DOS 0x420 entry to make_archives.py's zips, with its line in compress_archives.out.

Skeptic: Ran the given zip, in debug and release. Output: 'report.txt LINK 0 0 [the file's own bytes\n]', so the 21 bytes of content become a link Target and ReadAt gives nothing. The cause is in minizip-ng's mz_zip.c: mz_zip_attrib_is_symlink calls mz_zip_attrib_convert, which for an MS-DOS or NTFS host calls mz_zip_attrib_win32_to_posix, and that maps 0x400 (reparse point) to S_IFLNK. zip.cpp:213 takes the entry's kind from that. An MS-DOS-made entry (version_madeby 0x14, host 0) has no symlinks, so a file is silently misread as a link and its data is lost.

## S22-zip-tar-4 (medium): ZipWriter writes '\' in names differently by OS, and a file whose name ends with '\' turns into a folder

Status: fixed in 9407c2b: ZipWriter's Add and AddFolder panic at a name with '\', as a zip's names have '/' between folders, so no OS changes its bytes; panic_compress_zip_backslash.fb and panic_compress_zip_folder_backslash.fb have the cases

Where: runtime/src/zip.cpp:372

fb_zip_writer_add passes the name to minizip-ng unchanged. mz_zip_entry_write_header (mz_zip.c line 795, under #if defined(_WIN32)) writes each '\' as '/'. So on Windows, Add("a\\b.txt") makes an entry named a/b.txt (byte 47), while on Linux and macOS the same call writes a\b.txt (byte 92). The same program then makes different bytes on different OSes, against "every program and OS makes the same bytes", and on Windows the name doesn't read back as it was added. Separately, CheckFileName (archive.fb line 58) only refuses a trailing '/'. Add("c\\", data) therefore writes a file whose name ends with '\' (or with '/' on Windows). The reader, whose mz_os_is_dir_separator in zip.cpp takes '\' as a separator, lists it as the FOLDER c and drops its 3 bytes, on every OS. Shown on Windows.

```
Import std.core
Import std.compress

// A zip's names are written as given, the same on every OS: a backslash stays a backslash, and a
// file whose name ends with one stays a file.
Local writer = ZipWriter()
writer.Add("a\\b.txt", "one".Bytes)
writer.Add("c\\", "two".Bytes)
Local zip = writer.Finish()
Local reader = Try Zip.Read(zip)
For i = 0 Until reader.Count
    Local entry = reader.Entries[i]
    Print(entry.Name, entry.Kind, entry.Size, (Try reader.ReadAt(i)).Count)
End For

// The name's bytes in the first local header, from byte 30.
Local name = ""
For i = 30 Until 37
    name = name + "{zip[i]} "
End For
Print(name)
```

Expected: The same output on every OS, with no file read back as a folder. Either a panic at line 7 for a '\' in a zip's name, or the names kept as given:
a\b.txt FILE 3 3
c\ FILE 3 3
97 92 98 46 116 120 116

Actual: On Windows:
a/b.txt FILE 3 3
c FOLDER 0 0
97 47 98 46 116 120 116
On Linux and macOS the first name stays a\b.txt (byte 92), and the second is still the FOLDER c.

Suggested fix: Make it the same everywhere. One way is to panic at the caller for a '\' in a zip's name, in Add and AddFolder (APPNOTE 4.4.17.1 says names use only '/', and readers treat '\' as a separator). The other is to turn each '\' into '/' on every OS in fb_zip_writer_add, and have CheckFileName refuse a name that then ends with '/'. Add a panic test or a round-trip test for it.

Skeptic: Rewrote the program with the Write tool (bash heredoc had mangled the backslashes) and ran it on Windows, debug and release. Output: 'a/b.txt FILE 3 3', 'c FOLDER 0 0', and name bytes '97 47 98 46 116 120 116'. So the stored name has '/' where '\\' was added, and the file 'c\\' reads back as a folder with its 3 bytes gone. In _deps/minizip_ng-src/mz_zip.c:795, inside #if defined(_WIN32) (which MinGW defines), mz_zip_entry_write_header rewrites each '\\' as '/'. Linux and macOS skip that block and keep byte 92, so the same program writes different bytes per OS. That breaks the docs' 'every program and OS makes the same bytes'. CheckFileName (archive.fb) only refuses a trailing '/'.

## S22-zip-tar-5 (medium): A zip entry whose compressed stream ends before its compressed size isn't checked against its CRC-32

Status: fixed in 9407c2b: every entry's bytes are checked against its CRC-32 after they're read, however its stream ends; compress_archives.fb reads badcrc.zip, whose Deflate stream ends early, which is INVALID_DATA now and read as its short bytes before

Where: runtime/src/zip.cpp:180

read_entry relies on mz_zip_entry_close returning MZ_CRC_ERROR. But mz_zip_entry_read_close only compares the CRC when the stream's total_in equals file_info.compressed_size (mz_zip.c: "If entire entry was not read verification will fail"). For a Deflate, Zstd, LZMA or XZ entry whose stream ends early, the CRC is never compared. zip.cpp then checks only that the byte count matches the size, so a different set of bytes of the right length comes back as the entry's content. In the program, the entry's Deflate stream is swapped for a shorter one of 128 'B's followed by NULs: Python's zipfile says "Bad CRC-32 for file 'crc.txt'" and bsdtar says "ZIP bad CRC", but ReadAt returns the B's. The existing test only flips a byte in a STORE entry, which always consumes compressed_size.

```
Import std.core
Import std.collections
Import std.compress

// A zip's Deflate entry whose compressed bytes are swapped for a shorter Deflate stream of other
// bytes, as many, with NULs after it: its CRC-32 still says the first bytes, so reading it is an
// IOError, as Python's zipfile says "Bad CRC-32 for file 'crc.txt'".
Local good: UInt8[] = []
For i = 0 Until 64
    good.Add(0x41)
End For
For i = 0 Until 64
    good.Add(i As UInt8)
End For
Local other: UInt8[] = []
other.Resize(128, 0x42)

Local writer = ZipWriter()
writer.Add("crc.txt", good)
Local zip = writer.Finish()

// The entry's data starts after its local header, its name, and its extra field, and ends where
// the central directory starts.
Local start = 30 + (zip[26] As Int) + (zip[27] As Int) * 256 + (zip[28] As Int) +
              (zip[29] As Int) * 256
Local finish = start
While !(zip[finish] == 0x50 && zip[finish + 1] == 0x4b && zip[finish + 2] == 0x01 &&
        zip[finish + 3] == 0x02)
    finish += 1
End While

Local swapped = Deflate.Compress(other)
Print(swapped.Count < finish - start)
For i = start Until finish
    zip[i] = i - start < swapped.Count ? swapped[i - start] : 0
End For

Try
    Local bytes = (Try Zip.Read(zip)).ReadAt(0)
    Print(bytes.Count, bytes == good, bytes == other)
Catch e As IOError
    Print(e.Kind, e.Message)
End Try
```

Expected: True
INVALID_DATA the zip data isn't valid: 'crc.txt' doesn't match its CRC-32

Actual: True
128 False True

Suggested fix: In read_entry, compute the CRC-32 of the bytes given to `add` (zlib-ng's crc32, or mz_crypt_crc32_update) and compare it with info->crc for every entry that isn't encrypted, failing with RESULT_INVALID "doesn't match its CRC-32", rather than relying on minizip-ng's conditional check. Add a test with a swapped Deflate stream like this one.

Skeptic: Ran the program, debug and release. Output: 'True' then '128 False True'. So ReadAt returned the swapped-in 128 'B' bytes, which don't match the entry's CRC-32, and raised no IOError. The cause is mz_zip.c:2132-2133: minizip-ng compares the CRC only when total_in == file_info.compressed_size ('If entire entry was not read verification will fail'). zip.cpp:180 relies on MZ_CRC_ERROR from mz_zip_entry_close and otherwise checks only the byte count against the size, so a Deflate stream that ends early is never CRC-checked.

## S22-zip-tar-6 (medium): GNU tar's incremental tars, and any tar with a type POSIX doesn't define, fail to read

Status: fixed in 6d5bb7b: a type POSIX doesn't define is a file, as POSIX says; links, devices, FIFOs, and folders skip no bytes, and every other type its size's; GNU's 'D', an incremental tar's folder, is a folder; and devices, FIFOs, and GNU's 'V' volume headers aren't listed; compress_archives.fb reads incremental.tar, a 'D' folder as tar --listed-incremental writes one, its file, and a file of a vendor's type 'Z'

Where: runtime/src/tar.cpp:261

has_bytes lists only the types 0, NUL, 7, x, g, L, K and S. For every other type the data is taken as zero bytes, whatever the size field says. GNU tar's --listed-incremental writes each folder as type 'D', with the folder's listing as its data. The reader then reads the next header from inside that listing and fails the whole tar with INVALID_DATA. POSIX says a reader should treat a type it doesn't know as a regular file, data included. GNU tar 1.35 and bsdtar list this tar (and the one `tar --listed-incremental` made here) as the folder inc/ and the file inc/a.txt. Other types that carry data are affected the same way: 'M', 'N', and vendor types 'A' to 'Z'.

```
Import std.core
Import std.collections
Import std.compress

// A tar's 512-byte header for name, of `size` bytes, of type `kind`, with ustar's magic when
// `ustar` is True, and its checksum.
Function Header(name: String, size: Int, kind: UInt8, ustar: Bool): UInt8[]
    Local header: UInt8[] = []
    header.Resize(512, 0)
    Local bytes = name.Bytes
    For i = 0 Until bytes.Count
        header[i] = bytes[i]
    End For
    Put(header, 100, 8, 0o644)
    Put(header, 108, 8, 0)
    Put(header, 116, 8, 0)
    Put(header, 124, 12, size)
    Put(header, 136, 12, 0)
    header[156] = kind
    If ustar
        Local magic = "ustar".Bytes
        For i = 0 Until 5
            header[257 + i] = magic[i]
        End For
        header[263] = 0x30
        header[264] = 0x30
    End If

    For i = 148 Until 156
        header[i] = 0x20
    End For
    Local sum = 0
    For byte In header
        sum += byte As Int
    End For
    Put(header, 148, 7, sum)
    Return header
End Function

// Puts value at `at` as octal digits that fill all but the field's last byte, which is a NUL.
Function Put(header: UInt8[], at: Int, size: Int, value: Int)
    Local rest = value
    For i = at + size - 2 To at Step -1
        header[i] = (0x30 + rest % 8) As UInt8
        rest = rest / 8
    End For
End Function

// bytes, and NULs after them to the end of their block.
Function Padded(bytes: UInt8[]): UInt8[]
    Local padded = bytes.Clone()
    While padded.Count % 512 != 0
        padded.Add(0)
    End While
    Return padded
End Function

Function List(title: String, tar: UInt8[])
    Local parts: String[] = []
    Try
        Local reader = Tar.Read(tar)
        For i = 0 Until reader.Count
            Local entry = reader.Entries[i]
            parts.Add("{entry.Name} {entry.Kind} {(Try reader.ReadAt(i)).Count}")
        End For
    Catch e As IOError
        parts.Add("{e.Kind}: {e.Message}")
    End Try
    Print("{title}: {parts.Join(", ")}")
End Function

Local ending: UInt8[] = []
ending.Resize(1024, 0)

// GNU tar's incremental tar has 'D' folders, whose data lists what they hold, as
// `tar --listed-incremental` writes them; POSIX takes a type it doesn't define as a file's.
Local listing = "Ya.txt\0\0".Bytes
Local incremental: UInt8[] = []
incremental.AddAll(Header("inc/", listing.Count, 0x44, True))
incremental.AddAll(Padded(listing))
incremental.AddAll(Header("inc/a.txt", 6, 0x30, True))
incremental.AddAll(Padded("hello\n".Bytes))
incremental.AddAll(ending)
List("incremental tar", incremental)
```

Expected: incremental tar: inc FOLDER 0, inc/a.txt FILE 6

Actual: incremental tar: INVALID_DATA: the tar data isn't valid: the header at byte 512 doesn't match its checksum

Suggested fix: Skip each entry's data by its header's size for every type except 1, 2, 3, 4, 5 and 6, whose data is none. Read 'D' as a FOLDER, and other unknown types as FILEs, or leave them out, but only after skipping their data. Add a 'D' entry to make_archives.py's tars and to the test.

Skeptic: Ran the incremental-tar program (f6.fb). Output: 'incremental tar: INVALID_DATA: the tar data isn't valid: the header at byte 512 doesn't match its checksum'. tar.cpp:261-263 gives every type outside 0, NUL, 7, x, g, L, K and S zero bytes, whatever its size field says. So a 'D' entry's listing is parsed as the next header and the whole tar fails, although GNU tar --listed-incremental writes exactly that and POSIX says an unknown type is read as a regular file. Leaving such entries out would be fine, but their data has to be skipped first.

## S22-zip-tar-7 (medium): A GNU sparse file in pax format reads as the wrong bytes, under a placeholder name

Status: fixed in 6d5bb7b: a pax sparse file (GNU.sparse.name, .size, .map, .major) is listed under its real name and size, and reading it is unsupported (OTHER), as reading an old GNU sparse file is; compress_archives.fb reads paxsparse.tar, as tar -S --format=pax writes one

Where: runtime/src/tar.cpp:172

read_pax keeps only the path, linkpath and size records. GNU tar's -S with --format=pax uses its default sparse format 1.0 there (formats 0.0 and 0.1 are similar). It writes a regular '0' entry named ./GNUSparseFile.<pid>/<name>, with the real name and size in GNU.sparse.name and GNU.sparse.realsize records, and the sparse map at the start of the entry's data. The reader lists that entry under the placeholder name, gives the stored byte count as its Size, and ReadAt returns the map plus the stored regions as if they were the file. Find("holes.bin") is Null. tar.fb and stdlib.md say a GNU sparse file's read is an IOError. GNU tar and bsdtar extract the program's tar as holes.bin containing "start". A real `tar -S --format=pax` of a 1 MiB file, made by GNU tar 1.35, read the same way: as 1536 bytes beginning "2\n0\n512\n".

```
Import std.core
Import std.collections
Import std.compress

// A tar's 512-byte ustar header for name, of `size` bytes, of type `kind`, with its checksum.
Function Header(name: String, size: Int, kind: UInt8): UInt8[]
    Local header: UInt8[] = []
    header.Resize(512, 0)
    Local bytes = name.Bytes
    For i = 0 Until bytes.Count
        header[i] = bytes[i]
    End For
    Put(header, 100, 8, 0o644)
    Put(header, 108, 8, 0)
    Put(header, 116, 8, 0)
    Put(header, 124, 12, size)
    Put(header, 136, 12, 0)
    header[156] = kind
    Local magic = "ustar".Bytes
    For i = 0 Until 5
        header[257 + i] = magic[i]
    End For
    header[263] = 0x30
    header[264] = 0x30
    For i = 148 Until 156
        header[i] = 0x20
    End For
    Local sum = 0
    For byte In header
        sum += byte As Int
    End For
    Put(header, 148, 7, sum)
    Return header
End Function

Function Put(header: UInt8[], at: Int, size: Int, value: Int)
    Local rest = value
    For i = at + size - 2 To at Step -1
        header[i] = (0x30 + rest % 8) As UInt8
        rest = rest / 8
    End For
End Function

Function Padded(bytes: UInt8[]): UInt8[]
    Local padded = bytes.Clone()
    While padded.Count % 512 != 0
        padded.Add(0)
    End While
    Return padded
End Function

// A pax record, "<length> <key>=<value>\n", whose length counts itself.
Function Record(key: String, value: String): String
    Local rest = " {key}={value}\n".Bytes.Count
    Local length = rest + 1
    While "{length}".Bytes.Count + rest != length
        length += 1
    End While
    Return "{length} {key}={value}\n"
End Function

// GNU tar's sparse file in pax's format 1.0, which `tar -S --format=pax` writes: a pax header of
// GNU.sparse's records, an entry named ./GNUSparseFile.<pid>/<name>, and in its data the map,
// one region of 5 bytes at 0, before the region's bytes. GNU tar makes holes.bin of it, of
// "start".
Local records = (Record("GNU.sparse.major", "1") + Record("GNU.sparse.minor", "0") +
                 Record("GNU.sparse.name", "holes.bin") +
                 Record("GNU.sparse.realsize", "5")).Bytes
Local tar: UInt8[] = []
tar.AddAll(Header("./PaxHeaders/holes.bin", records.Count, 0x78))
tar.AddAll(Padded(records))
tar.AddAll(Header("./GNUSparseFile.0/holes.bin", 1024, 0x30))
tar.AddAll(Padded("1\n0\n5\n".Bytes))
tar.AddAll(Padded("start".Bytes))
Local ending: UInt8[] = []
ending.Resize(1024, 0)
tar.AddAll(ending)

Local reader = Try Tar.Read(tar)
Local entry = reader.Entries[0]
Try
    Local bytes = reader.ReadAt(0)
    Print(entry.Name, entry.Kind, entry.Size, bytes.Count, bytes[0], bytes[1], bytes[2])
Catch e As IOError
    Print(entry.Name, e.Kind, e.Message)
End Try
Print(reader.Contains("holes.bin"))
```

Expected: holes.bin OTHER the tar data uses what isn't supported: 'holes.bin' is a GNU sparse file
True

Actual: ./GNUSparseFile.0/holes.bin FILE 1024 1024 49 10 48
False

Suggested fix: In read_pax, note any GNU.sparse. key: major, minor, name, realsize, size, map, numblocks. When one is present, name the entry by GNU.sparse.name (or by the pax path, for 0.x), take the size from realsize, and set sparse = true, so that ReadAt gives OTHER as it does for 'S'. Add such a tar to make_archives.py.

Skeptic: Ran the program, debug and release. Output: './GNUSparseFile.0/holes.bin FILE 1024 1024 49 10 48' and 'False'. So the entry is listed under the placeholder name, ReadAt silently returns the sparse map ('1\\n0\\n...') and the stored region as if they were the file, and Contains("holes.bin") is False. read_pax (tar.cpp:172-187) keeps only path, linkpath and size, and sparse is set only for type 'S' (line 333). tar.fb's ReadAt comment and stdlib.md say a GNU sparse file's read is an IOError, not wrong bytes. GNU tar's -S --format=pax, and libarchive's pax writer, write sparse files this way.

## S22-zip-tar-8 (medium): An old GNU sparse file with more than 4 data regions makes the whole tar unreadable

Status: fixed in 6d5bb7b: an old GNU sparse file's extension blocks, which its header's isextended byte says follow, are skipped before its data, and its size is its real size; compress_archives.fb reads extended.tar, a sparse file of 9 regions and a file after it, which made the whole tar unreadable before

Where: runtime/src/tar.cpp:268

In an 'S' header, the isextended byte at offset 482 says that 512-byte sparse extension blocks follow the header, before the data, and the size field doesn't count them. The reader doesn't skip them: data = at + block, so it takes the data from a block too early and reads part of the data as the next header. The whole tar then fails with INVALID_DATA, although a sparse file is meant to be listed and read as OTHER, as compress_archives' sparse.tar test with one region is. GNU tar writes extension blocks for any sparse file with more than 4 data regions. The program's data is GNU tar 1.35's own `tar -S --format=gnu` output, gzipped; GNU tar lists it as many_holes.bin, 65536 bytes.

```
Import std.core
Import std.compress

// GNU tar 1.35's `tar -S --format=gnu -czf` of a 64 KiB file of 8 regions of "data" between holes:
// an old GNU sparse file, whose header says more of its map follows in an extension block. The
// tar lists the file, whose read is an IOError, as a sparse file's with no extension block is.
Local data: UInt8[] = [
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0a, 0xed, 0xd1, 0xd1, 0x0a,
    0xc2, 0x20, 0x14, 0xc6, 0x71, 0x7b, 0x13, 0x9f, 0x20, 0x3c, 0x4e, 0xdd, 0x83, 0xf4,
    0x00, 0xe1, 0x28, 0x28, 0xa8, 0x09, 0x9b, 0x5d, 0xf4, 0xf6, 0x8d, 0xdd, 0x8c, 0xd6,
    0xac, 0x8b, 0xa8, 0x08, 0xfe, 0xbf, 0x1b, 0xe5, 0x28, 0x1e, 0x39, 0xdf, 0x39, 0xb6,
    0xd7, 0xed, 0x21, 0x9d, 0xf6, 0xfd, 0xba, 0x39, 0xb6, 0xea, 0x23, 0xcc, 0x20, 0x38,
    0x37, 0xae, 0x83, 0xd9, 0x2a, 0xe3, 0x5e, 0xbc, 0x0d, 0xa1, 0xf6, 0xc1, 0xbb, 0x4a,
    0x19, 0x5b, 0xd5, 0xd6, 0x29, 0xbd, 0x51, 0x5f, 0x70, 0xe9, 0x73, 0xec, 0xb4, 0x56,
    0x5d, 0x4a, 0xf9, 0xd9, 0xbd, 0x57, 0xe7, 0x6f, 0x4d, 0x67, 0x36, 0x19, 0x99, 0xf6,
    0xb6, 0x50, 0x77, 0x85, 0x7a, 0x78, 0xa8, 0xaf, 0xa6, 0x77, 0x4c, 0xa1, 0xbb, 0x14,
    0xba, 0x4b, 0xa1, 0xbb, 0x14, 0xba, 0x4b, 0x58, 0xae, 0xdb, 0xbb, 0xf7, 0x97, 0x3f,
    0x82, 0x9f, 0xd8, 0xc5, 0x1c, 0x99, 0x02, 0xf9, 0x83, 0xfc, 0x41, 0xfe, 0x20, 0x7f,
    0x90, 0x3f, 0xc8, 0x1f, 0xe4, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8,
    0x4f, 0x37, 0xfb, 0x17, 0xd0, 0xc3, 0x00, 0x28, 0x00, 0x00
]
Try
    Local reader = Tar.Read(data)
    For i = 0 Until reader.Count
        Local entry = reader.Entries[i]
        Try
            Print(entry.Name, entry.Kind, entry.Size, (Try reader.ReadAt(i)).Count)
        Catch e As IOError
            Print(entry.Name, e.Kind, e.Message)
        End Try
    End For
Catch e As IOError
    Print(e.Kind, e.Message)
End Try
```

Expected: many_holes.bin OTHER the tar data uses what isn't supported: 'many_holes.bin' is a GNU sparse file

Actual: INVALID_DATA the tar data isn't valid: the header at byte 4608 doesn't match its checksum

Suggested fix: For type 'S', while the header's isextended byte (482), and then each extension block's (504), is nonzero, skip another 512-byte block before the data starts, and return RESULT_ENDS_TOO_SOON if the bytes run out. Give make_archives.py's sparse.tar an extension block.

Skeptic: Ran the program with diagnostics added. The gunzipped tar is 10240 bytes, and its first header is type 83 ('S') named many_holes.bin with isextended = 1 at byte 482. Tar.Read fails: 'INVALID_DATA the tar data isn't valid: the header at byte 4608 doesn't match its checksum', in debug and release. tar.cpp:268 sets data = at + block and never skips the 512-byte sparse extension blocks that byte 482 announces, so the data is taken one block early and the following header is misaligned. The whole tar is lost instead of only this file's read being OTHER, as it is for a sparse file without extension blocks.

## S22-7z-docs-5 (low): docs/spec/09-errors.md still lists IOErrorKind without INVALID_DATA

Status: fixed in 0850307: docs/spec/09-errors.md lists INVALID_DATA, as S22-codecs-6 asked

Where: docs/spec/09-errors.md:28

The step adds INVALID_DATA to IOErrorKind in std/core/error.fb, between UNEXPECTED_END and INTERRUPTED, and docs/stdlib.md lists it. The spec, which CLAUDE.md calls the source of truth, still gives IOErrorKind as NOT_FOUND, PERMISSION_DENIED, ALREADY_EXISTS, UNEXPECTED_END, INTERRUPTED, OTHER. A program using IOErrorKind.INVALID_DATA compiles and runs, though the spec says there is no such Kind.

```
Import std.core

Print(IOErrorKind.INVALID_DATA, IOErrorKind.INTERRUPTED)
```

Expected: Per docs/spec/09-errors.md, IOErrorKind has no INVALID_DATA, so this shouldn't compile; either the spec or the Enum is wrong

Actual: INVALID_DATA INTERRUPTED

Suggested fix: Add `INVALID_DATA` after `UNEXPECTED_END` in docs/spec/09-errors.md's IOErrorKind list, matching std/core/error.fb and docs/stdlib.md.

Skeptic: Ran 7z-verify/f5.fb: it prints 'INVALID_DATA INTERRUPTED'. std/core/error.fb line 53 has 'NOT_FOUND, PERMISSION_DENIED, ALREADY_EXISTS, UNEXPECTED_END, INVALID_DATA, INTERRUPTED, OTHER', and docs/stdlib.md line 200 lists INVALID_DATA. docs/spec/09-errors.md line 29 still lists IOErrorKind as NOT_FOUND, PERMISSION_DENIED, ALREADY_EXISTS, UNEXPECTED_END, INTERRUPTED, OTHER. The step's diff touches only docs/stdlib.md under docs/, and the spec list dates from step 1, when it matched error.fb. The spec is the source of truth and is now stale.

## S22-codecs-3 (low): Zlib and Zstd data that needs a preset dictionary is INVALID_DATA, not unsupported

Status: fixed in 59f7a7e: Zlib's Z_NEED_DICT and zstd's dictionary_wrong are unsupported, not INVALID_DATA, and stdlib.md says preset dictionaries aren't supported (98decf2); compress_codecs.fb has a Zlib stream made with zdict and a Zstd frame naming dictionary 7

Where: runtime/src/zlib.cpp:167

A Zlib stream with FDICT set (RFC 1950's preset dictionary) is valid Zlib. inflate returns Z_NEED_DICT for it, and inflate_all's `status != Z_OK` branch turns that into RESULT_INVALID. zlib leaves no msg for Z_NEED_DICT, so the error is a bare "the Zlib data isn't valid" that gives no reason. In the same way, a Zstd frame that names a dictionary ID fails with ZSTD_error_dictionary_wrong, which zstd.cpp maps to RESULT_INVALID. The step's convention is that INVALID_DATA is for bytes that aren't the format or are broken, and OTHER is for what std doesn't support. Both of these are well-formed data that use a feature std.compress lacks.

```
Import std.core
Import std.collections
Import std.compress

// A zlib stream made with a preset dictionary ("bottles of milk on the wall"), as Python's
// zlib.compressobj(zdict=...) makes it: valid Zlib whose FDICT flag asks for a dictionary.
Local zlib: UInt8[] = [120, 187, 141, 125, 9, 238, 51, 54, 80, 72, 194, 45, 11, 0, 156, 106, 10, 113]
Try
    Print((Try Zlib.Decompress(zlib)).Count)
Catch e As IOError
    Print("zlib with a dictionary:", e.Kind, e.Message)
End Try

// A Zstd frame whose header names dictionary 7: the descriptor's low bits say a 1-byte
// dictionary ID, which goes right after the descriptor (no window descriptor, single segment).
Local frame = Zstd.Compress("hello, hello, hello".Bytes)
Local withId: UInt8[] = frame[0..4]
withId.Add(frame[4] | 1)
withId.Add(7)
withId.AddAll(frame[5..frame.Count])
Try
    Print((Try Zstd.Decompress(withId)).Count)
Catch e As IOError
    Print("zstd with a dictionary:", e.Kind, e.Message)
End Try
```

Expected: zlib with a dictionary: OTHER the Zlib data uses what isn't supported: <a reason, such as needing a preset dictionary>
zstd with a dictionary: OTHER the Zstd data uses what isn't supported: Dictionary mismatch

Actual: zlib with a dictionary: INVALID_DATA the Zlib data isn't valid
zstd with a dictionary: INVALID_DATA the Zstd data isn't valid: Dictionary mismatch

Suggested fix: In inflate_all, map Z_NEED_DICT to RESULT_UNSUPPORTED with a message of its own (e.g. "it needs a preset dictionary"). In zstd.cpp, map ZSTD_error_dictionary_wrong to RESULT_UNSUPPORTED, as is already done for windowTooLarge. Add both cases to compress_codecs.fb.

Skeptic: f3.fb printed 'zlib with a dictionary: INVALID_DATA the Zlib data isn't valid' (an empty reason) and 'zstd with a dictionary: INVALID_DATA the Zstd data isn't valid: Dictionary mismatch'. Python's zlib.compressobj(zdict=...) writes the same 6-byte header (120, 187, then the dictionary ID), so the input is real FDICT Zlib, which RFC 1950 allows. stdlib.md's std.compress section says INVALID_DATA is for bytes that aren't the format or are broken, and OTHER is for data that uses what isn't supported. zlib.cpp's inflate_all instead sends Z_NEED_DICT through the generic `status != Z_OK` branch to RESULT_INVALID. zstd.cpp maps ZSTD_error_dictionary_wrong to RESULT_INVALID, while it maps windowTooLarge to RESULT_UNSUPPORTED.

## S22-codecs-4 (low): lz4.fb says HC mode starts at level 3; in LZ4 1.10 it starts at 2

Status: fixed in 0850307: lz4.fb's comment says levels from 2 are HC's, as LZ4 1.10's are

Where: std/compress/lz4.fb:4

lz4.fb's header says levels "from 3 on, they're LZ4's slower high-compression mode". The bundled LZ4 1.10.0 has LZ4HC_CLEVEL_MIN = 2. Level 2 is LZ4HC's new "lz4mid" strategy (k_clTable[2] in lz4hc.c), and LZ4F switches to the HC context for any level >= 2. In the fast mode, levels 1 and 2 would be identical (both acceleration 1), but std gives them different bytes. On this text, level 2's output is even larger than level 1's.

```
Import std.core
Import std.compress

// lz4.fb says levels 1 and 2 are both LZ4's fast mode (HC is "from 3 on"), and LZ4's fast mode
// has no setting between them, so their bytes would be the same.
Local text = ""
For i = 200 To 1 Step -1
    text = text + "{i} bottles of milk on the wall, {i} bottles of milk.\n"
End For
Local one = Lz4.Compress(text.Bytes, 1)
Local two = Lz4.Compress(text.Bytes, 2)
Print(one == two, one.Count, two.Count)
```

Expected: True, if the comment were right (levels 1 and 2 both the fast mode)

Actual: False 2232 2377

Suggested fix: Say that levels from 2 on are LZ4's high-compression modes (2 is its middle mode, 3 to 9 HC, 10 to 12 its optimal parser), or leave the boundary out.

Skeptic: lz4.fb lines 3-4 say 'from 3 on, they're LZ4's slower high-compression mode'. The bundled _deps/lz4-src/lib/lz4hc.h has `#define LZ4HC_CLEVEL_MIN 2`. lz4hc.c's k_clTable[2] is lz4mid, and lz4frame.c switches to the HC context at `compressionLevel >= LZ4HC_CLEVEL_MIN`. f4.fb printed 'False 2232 2377 1872': levels 1 and 2 give different bytes, and level 2's output is larger than level 1's. So the comment is wrong.

## S22-codecs-5 (low): The tests never read an .lz4 (or raw Deflate or Zlib) made by a common tool

Status: fixed in dcebe0a: compress_tools.fb reads poem.txt.lz4, made by the lz4 tool (1.10.0, built from the fetched source), poem.txt.git, git's zlib object for the poem, and the Deflate stream inside gzip's poem.txt.gz

Where: tests/programs/compress_tools.fb:1

Step 22's done-when is "each format round-trips, and reads what common tools wrote". compress_tools.fb reads GNU gzip's .gz, xz's .xz, and bsdtar's zstd frame, plus archives. No test reads an LZ4 frame written by the lz4 tool (default header 04 22 4D 18 64 70 B9: independent 4 MiB blocks with a content checksum). The only Lz4 data the tests decode is std's own, which uses 64 KiB linked blocks. Deflate.Decompress and Zlib.Decompress also never see a tool's bytes: the zip entries are inflated through minizip-ng's stream, not std's Deflate. Decoding works on a hand-made lz4-tool frame, as the program shows, but the done-when isn't checked for these three formats.

```
Import std.core
Import std.compress

// A frame as the lz4 tool writes one by default: its header 04 22 4D 18 64 70 B9 (independent
// 4 MiB blocks, a content checksum), one block, the end mark, and the content's XXH32.
Local frame: UInt8[] = [
    0x04, 0x22, 0x4d, 0x18, 0x64, 0x70, 0xb9, 0x19, 0x00, 0x00, 0x80, 0x48, 0x65, 0x6c, 0x6c, 0x6f,
    0x20, 0x66, 0x72, 0x6f, 0x6d, 0x20, 0x74, 0x68, 0x65, 0x20, 0x6c, 0x7a, 0x34, 0x20, 0x74, 0x6f,
    0x6f, 0x6c, 0x21, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x8f, 0x9f, 0x7d, 0xd2
]
Print((Try Lz4.Decompress(frame)) == "Hello from the lz4 tool!\n".Bytes)
```

Expected: True, checked by a test of a file the lz4 tool wrote

Actual: True here, but no test in tests/programs reads an lz4-tool file, or a tool's raw Deflate or Zlib bytes

Suggested fix: Add a poem.txt.lz4 made by `lz4 -c poem.txt` to compress_tools/files (and to make_data.py's list), and print Matched for Lz4.Decompress of it. Also run Deflate.Decompress over sample.zip's deflated poem.txt entry (or a zlib stream from pigz -z or Python's zlib) so Deflate and Zlib read a tool's bytes too.

Skeptic: compress_tools.fb reads tool-made poem.txt.gz, .xz and .zst, plus zip, tar and 7z archives. make_data.py's FILES list has no .lz4. I grepped tests/: the only Lz4 data decoded anywhere is std's own (compress_codecs.fb, and compress_archives.fb line 155's Lz4.Compress(tar)). Deflate.Decompress and Zlib.Decompress never get bytes a tool wrote either. The milestones row 22 done-when is 'each format round-trips, and reads what common tools wrote', so it goes unchecked for Lz4, Deflate and Zlib. The finding's hand-made lz4-tool frame decodes correctly (f5.fb printed True), so this is a coverage gap, not wrong behavior.

## S22-codecs-6 (low): The spec's IOErrorKind list leaves out the new INVALID_DATA

Status: fixed in 0850307: docs/spec/09-errors.md lists INVALID_DATA with the other kinds

Where: docs/spec/09-errors.md:29

Step 22 inserted INVALID_DATA into std/core/error.fb's IOErrorKind (between UNEXPECTED_END and INTERRUPTED), and docs/stdlib.md lists it. docs/spec/09-errors.md, the source of truth, still lists IOErrorKind as NOT_FOUND, PERMISSION_DENIED, ALREADY_EXISTS, UNEXPECTED_END, INTERRUPTED, OTHER. Every codec's error for broken data is a Kind the spec says doesn't exist.

```
Import std.core

Print(IOErrorKind.INVALID_DATA, IOErrorKind.INTERRUPTED, IOErrorKind.OTHER)
```

Expected: Per docs/spec/09-errors.md there is no INVALID_DATA, so the spec should list it next to the Kinds this prints

Actual: INVALID_DATA INTERRUPTED OTHER

Suggested fix: Add `INVALID_DATA` after `UNEXPECTED_END` in 09-errors.md's IOErrorKind list.

Skeptic: In 6d8fec2..29675bb, std/core/error.fb changes IOErrorKind to NOT_FOUND, PERMISSION_DENIED, ALREADY_EXISTS, UNEXPECTED_END, INVALID_DATA, INTERRUPTED, OTHER, and docs/stdlib.md:200 lists INVALID_DATA. The diff doesn't touch docs/spec/. docs/spec/09-errors.md lines 29-30 still list `NOT_FOUND`, `PERMISSION_DENIED`, `ALREADY_EXISTS`, `UNEXPECTED_END`, `INTERRUPTED`, `OTHER`. f6.fb printed 'INVALID_DATA INTERRUPTED OTHER'. The spec, the source of truth, now misses a Kind that every codec throws.

## S22-codecs-7 (low): compress.hpp says decoders give only 0 to 3, but Zstd's and XZ's give 4

Status: fixed in 0850307: compress.hpp says what each result is, 4 too, and which functions give it

Where: runtime/include/fb/compress.hpp:16

The header comment says a decoder's add and finish give 0, 1, 2, or 3, and that 4 (unsupported) is something "an archive's reader gives ... too". zstd.cpp's decoder returns RESULT_UNSUPPORTED for a window past 128 MiB. xz.cpp's returns it for LZMA_OPTIONS_ERROR and LZMA_MEMLIMIT_ERROR. codec.fb's Failed turns those into OTHER "uses what isn't supported".

```
Import std.core
Import std.collections
Import std.compress

// A Zstd decoder gives the runtime's result 4 (unsupported), which compress.hpp says only an
// archive's reader gives: a frame whose window descriptor says 2 GiB.
Local streamed = ZstdCompressor()
Local frame = streamed.Add("some text".Bytes)
frame.AddAll(streamed.Finish())
frame[5] = 0xa8
Try
    Zstd.Decompress(frame)
Catch e As IOError
    Print(e.Kind, e.Message)
End Try
```

Expected: By compress.hpp's comment, a decoder can't give 4, so no codec error would be OTHER "uses what isn't supported"

Actual: OTHER the Zstd data uses what isn't supported: Frame requires too much memory for decoding

Suggested fix: Say in the comment that a decoder gives 4 too, for what its library supports but std.compress doesn't allow (a Zstd window past 128 MiB, XZ options liblzma lacks, or a dictionary past an array's bytes).

Skeptic: compress.hpp lines 16-20 say a decoder's add and finish give 0 to 3, and that 4 is what 'an archive's reader gives'. But zstd.cpp:94 returns RESULT_UNSUPPORTED (4) for windowTooLarge, and xz.cpp:129 and xz.cpp:132 return it from the decoder too. f7.fb, a streamed Zstd frame with its window descriptor set to 0xa8, printed 'OTHER the Zstd data uses what isn't supported: Frame requires too much memory for decoding', which is codec.fb's result-4 branch. The header comment is stale.

## S22-zip-tar-10 (low): ZipWriter stores the data instead of compressing it when LZMA or XZ is asked for at level 0

Status: fixed in 2355a3c: a zip's LZMA and XZ levels go from 1 to 9, and 0 panics at the caller, as minizip-ng stores an entry at 0; panic_compress_zip_lzma_level.fb and panic_compress_zip_xz_level.fb have the cases

Where: runtime/src/zip.cpp:384

mz_zip_entry_write_open turns any entry with compress_level 0 into STORE. For DEFLATE that matches the convention (zip -0). For LZMA and XZ, though, level 0 is liblzma's fastest real preset: XZ.Compress(data, 0) turns these 6000 bytes into 124. zip.fb's Add comment and stdlib.md say LZMA and XZ levels go from 0 to 9. Yet Add(..., ZipMethod.XZ, 0) or Add(..., ZipMethod.LZMA, 0) writes method 0 with all 6000 bytes, while level 1 compresses them to about 200 bytes.

```
Import std.core
Import std.compress

// LZMA's and XZ's level 0 is liblzma's fastest preset, as `xz -0` and XZ.Compress(data, 0) are:
// an entry added by them at 0 is compressed by them, not stored.
Local text = ""
For i = 1 To 200
    text = text + "the same line again and again\n"
End For

Local data = text.Bytes
Print("XZ.Compress at 0:", XZ.Compress(data, 0).Count, "of", data.Count)
For method In [ZipMethod.LZMA, ZipMethod.XZ]
    For level In [0, 1]
        Local writer = ZipWriter()
        writer.Add("a.txt", data, method, level)
        Local zip = writer.Finish()
        // The local header's method is at byte 8: 0 is stored, 14 LZMA, and 95 XZ.
        Print(method, level, "method", (zip[8] As Int) + (zip[9] As Int) * 256, "zip of",
              zip.Count, "bytes")
    End For
End For
```

Expected: XZ.Compress at 0: 124 of 6000
LZMA 0 method 14 zip of about 200 bytes
LZMA 1 method 14 zip of 201 bytes
XZ 0 method 95 zip of about 250 bytes
XZ 1 method 95 zip of 252 bytes
(or, if 0 is to stay unsupported, a panic at the Add saying LZMA's levels in a zip run from 1 to 9)

Actual: XZ.Compress at 0: 124 of 6000
LZMA 0 method 0 zip of 6128 bytes
LZMA 1 method 14 zip of 201 bytes
XZ 0 method 0 zip of 6128 bytes
XZ 1 method 95 zip of 252 bytes

Suggested fix: minizip-ng can't be given level 0 for LZMA or XZ without storing. Either have ZipWriter.Add check LZMA and XZ levels as 1 to 9 (panicking at 0) and say in its comment that level 0 means stored for DEFLATE, or compress LZMA and XZ entries in zip.cpp and write them raw (mz_zip_entry_write_open with raw = 1 and the sizes and CRC filled in). Add a test of level 0 for each method.

Skeptic: Ran it, debug and release. Output: 'XZ.Compress at 0: 124 of 6000', then 'LZMA 0 method 0 zip of 6128 bytes', 'LZMA 1 method 14 zip of 201 bytes', 'XZ 0 method 0 zip of 6128 bytes' and 'XZ 1 method 95 ...'. The cause is mz_zip.c:2033: compress_level == 0 makes any entry STORE. zip.fb's Add comment says LZMA and XZ levels go from 0 to 9 and that the file is 'compressed by method, at level', so asking for LZMA or XZ at 0 silently stores the data. Low: it still round-trips and no data is lost; the docs and the behavior just disagree.

## S22-zip-tar-11 (low): Every file's local header has Zip64, although the docs say Zip64 is used only where sizes need it

Status: fixed in 2355a3c: a ZipWriter's entries take Zip64 only where their sizes need it, which a UInt8[]'s don't, in their local headers too; compress_archives.fb checks a local header's version, sizes, and extra field length

Where: runtime/src/zip.cpp:383

fb_zip_writer_add sets info.zip64 = MZ_ZIP64_AUTO and leaves info.uncompressed_size at 0. mz_zip_entry_needs_zip64 then assumes Zip64 for every local header that isn't a folder's: version needed 45, both sizes 0xFFFFFFFF, and a 20-byte Zip64 extra field. The central directory record says version 20, gives the real sizes, and has no extra field. zip.fb line 3 and stdlib.md line 596 say Zip64 comes in where sizes need it, and a UInt8[]'s sizes never need it. A reader without Zip64 support, or one that reads local headers in turn, sees 4 GiB sizes, and every entry costs 20 more bytes.

```
Import std.core
Import std.compress

// A zip of a 5-byte file needs no Zip64: its local header's version needed is 20, its sizes are
// 5 and 5, and it has no extra field, as its central directory's record says.
Function UInt16At(data: UInt8[], at: Int): Int => (data[at] As Int) + (data[at + 1] As Int) * 256

Function UInt32At(data: UInt8[], at: Int): Int64
    Return (UInt16At(data, at) As Int64) + (UInt16At(data, at + 2) As Int64) * 65536
End Function

Local writer = ZipWriter()
writer.Add("a.txt", "hello".Bytes, ZipMethod.STORE)
Local zip = writer.Finish()
Print("local: version needed", UInt16At(zip, 4), "sizes", UInt32At(zip, 18), UInt32At(zip, 22),
      "extra field", UInt16At(zip, 28))

Local central = 0
While !(zip[central] == 0x50 && zip[central + 1] == 0x4b && zip[central + 2] == 0x01 &&
        zip[central + 3] == 0x02)
    central += 1
End While
Print("central: version needed", UInt16At(zip, central + 6), "sizes", UInt32At(zip, central + 20),
      UInt32At(zip, central + 24), "extra field", UInt16At(zip, central + 30))
```

Expected: local: version needed 20 sizes 5 5 extra field 0
central: version needed 20 sizes 5 5 extra field 0

Actual: local: version needed 45 sizes 4294967295 4294967295 extra field 20
central: version needed 20 sizes 5 5 extra field 0

Suggested fix: Set info.zip64 = MZ_ZIP64_DISABLE, since no entry's sizes or offsets can pass 4 GiB in a zip that has to fit in a UInt8[]. mz_zip_write_cd still writes the Zip64 end record for more than 65535 entries, which this review checked with 70000 entries. Alternatively, set info.uncompressed_size = bytes.size() before mz_zip_entry_write_open. Fix the docs if Zip64 is kept, and test the local header's version-needed and extra-field length.

Skeptic: Ran it, debug and release. Output: 'local: version needed 45 sizes 4294967295 4294967295 extra field 20' and 'central: version needed 20 sizes 5 5 extra field 0'. zip.cpp:383 sets MZ_ZIP64_AUTO with uncompressed_size left at 0, and mz_zip_entry_needs_zip64 (mz_zip.c:576-578) then assumes Zip64 for every local header. That contradicts zip.fb's and stdlib.md's 'Zip64 where sizes need it', and a UInt8[]'s sizes never need it. The zip is still valid Zip64, so this is low.

## S22-zip-tar-12 (low): Tar.Read doesn't decompress a .tar.zst that starts with a skippable frame

Status: fixed in 50c53fb: Tar.Read skips skippable frames before it looks for zstd's and LZ4's magic; compress_archives.fb reads a .tar.zst and a .tar.lz4 after two skippable frames

Where: std/compress/tar.fb:89

Unwrapped recognizes Zstd only by a frame's magic, 28 B5 2F FD. A Zstd or LZ4 stream may begin with a skippable frame (magic 0x184D2A50 to 0x184D2A5F). pzstd, zstd's parallel compressor, writes one before each frame. Zstd.Decompress reads such data, but Tar.Read treats it as a raw tar: this small one gives UNEXPECTED_END, and a longer one gives INVALID_DATA from the checksum.

```
Import std.core
Import std.collections
Import std.compress

// A .tar.zst that starts with a skippable frame, as pzstd, zstd's parallel tool, writes one,
// reads as its tar, as Zstd.Decompress reads it.
Local writer = TarWriter()
writer.Add("a.txt", "hello\n".Bytes)
Local tar = writer.Finish()

Local packed: UInt8[] = [0x50, 0x2a, 0x4d, 0x18, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
packed.AddAll(Zstd.Compress(tar))
Print("Zstd.Decompress:", (Try Zstd.Decompress(packed)) == tar)
Try
    Print("Tar.Read:", (Try Tar.Read(packed)).Count, "entries")
Catch e As IOError
    Print("Tar.Read:", e.Kind, e.Message)
End Try
```

Expected: Zstd.Decompress: True
Tar.Read: 1 entries

Actual: Zstd.Decompress: True
Tar.Read: UNEXPECTED_END the tar data ends too soon

Suggested fix: In Unwrapped, also treat data starting with 50..5F 2A 4D 18 as compressed. Since Zstd and LZ4 share that range, try Zstd.Decompress first and Lz4.Decompress if Zstd says it isn't its format. Add a test.

Skeptic: Ran it, debug and release. Output: 'Zstd.Decompress: True' and 'Tar.Read: UNEXPECTED_END the tar data ends too soon'. Unwrapped in tar.fb (line 89) recognizes Zstd only by 28 B5 2F FD, so a .tar.zst that starts with a skippable frame (50 2A 4D 18), which Zstd.Decompress accepts, is read as a raw tar. Low: only some tools, such as pzstd, write a leading skippable frame.

## S22-zip-tar-13 (low): TarWriter.Finish's panic for a tar past 2 GiB talks about compressed bytes and gives no location

Status: fixed in 50c53fb: ZipWriter's and TarWriter's Finish panic at their caller with "the zip's (or tar's) bytes are more than a UInt8[] holds"; std_tests.cpp's "an archive writer's Finish panics at its caller for an archive past its limit" checks both through a smaller limit, since a test can't fill an array

Where: runtime/src/compress_internal.hpp:95

Add accepts entries without limit, so here the writer holds 2.2 GiB. Finish then panics from fb_tar_writer_finish's add_all (tar.cpp line 535) with "the compressed bytes are more than a UInt8[] holds". A tar isn't compressed, the message names neither the tar nor the call, and there is no source location because the panic comes from the runtime, not from an @PanicsAtCaller function. Neither tar.fb's Add nor its Finish comment mentions the panic. Run with --release; it needs about 4 GiB of memory.

```
Import std.core
Import std.collections
Import std.compress

// A tar of more bytes than a UInt8[] holds can't be given, and Finish's panic says so of the tar,
// which isn't compressed.
Local data: UInt8[] = []
data.Resize(1100 * 1024 * 1024, 7)
Local writer = TarWriter()
writer.Add("one.bin", data)
writer.Add("two.bin", data)
Print("added")
Local tar = writer.Finish()
Print(tar.Count)
```

Expected: added
panic: a tar's bytes are more than a UInt8[] holds at tar_huge.fb:11:1 (or at the Add that goes past, at line 10)

Actual: added
panic: the compressed bytes are more than a UInt8[] holds

Suggested fix: Track the writer's size in fb_tar_writer_add, or have it return whether the entry still fits. Then panic at the caller from Add (marked @PanicsAtCaller) or from Finish, with a message naming the tar, and document the panic in Add's comment. Make add_all's message say which archive or codec it is for, or give add_all a message parameter.

Skeptic: Ran it with --release (the machine has 49 GB). Output: 'added', then 'panic: the compressed bytes are more than a UInt8[] holds', exit code 101, with no source location. fb_tar_writer_finish (tar.cpp:534-535) uses output_t::add_all, whose message and comment (compress_internal.hpp:90-96, 'which only an encoder's output can') assume a codec's output. A tar isn't compressed and the panic doesn't say where it came from. Neither tar.fb's Add nor its Finish mentions the panic. The program would panic either way, so only the message and the location are wrong: low.

## S22-zip-tar-9 (low): An old tar's folders read as empty files

Status: fixed in 6d5bb7b: a regular file whose name ends with '/', as an old tar writes a folder, is a folder; compress_archives.fb reads v7folder.tar

Where: runtime/src/tar.cpp:322

Before ustar's type '5', V7 tars wrote a folder as a regular entry (type '0' or NUL) whose name ends with '/'. The reader gives those KIND_FILE and then strips the '/' (without_slash, line 328), so dir/ becomes a FILE named dir. GNU tar 1.35 and bsdtar both list this tar's first entry as the folder dir/. stdlib.md and tar.fb say old tars are read.

```
Import std.core
Import std.collections
Import std.compress

// A tar's 512-byte header for name, of `size` bytes, of type `kind`, with ustar's magic when
// `ustar` is True, and its checksum.
Function Header(name: String, size: Int, kind: UInt8, ustar: Bool): UInt8[]
    Local header: UInt8[] = []
    header.Resize(512, 0)
    Local bytes = name.Bytes
    For i = 0 Until bytes.Count
        header[i] = bytes[i]
    End For
    Put(header, 100, 8, 0o644)
    Put(header, 108, 8, 0)
    Put(header, 116, 8, 0)
    Put(header, 124, 12, size)
    Put(header, 136, 12, 0)
    header[156] = kind
    If ustar
        Local magic = "ustar".Bytes
        For i = 0 Until 5
            header[257 + i] = magic[i]
        End For
        header[263] = 0x30
        header[264] = 0x30
    End If

    For i = 148 Until 156
        header[i] = 0x20
    End For
    Local sum = 0
    For byte In header
        sum += byte As Int
    End For
    Put(header, 148, 7, sum)
    Return header
End Function

// Puts value at `at` as octal digits that fill all but the field's last byte, which is a NUL.
Function Put(header: UInt8[], at: Int, size: Int, value: Int)
    Local rest = value
    For i = at + size - 2 To at Step -1
        header[i] = (0x30 + rest % 8) As UInt8
        rest = rest / 8
    End For
End Function

// bytes, and NULs after them to the end of their block.
Function Padded(bytes: UInt8[]): UInt8[]
    Local padded = bytes.Clone()
    While padded.Count % 512 != 0
        padded.Add(0)
    End While
    Return padded
End Function

Function List(title: String, tar: UInt8[])
    Local parts: String[] = []
    Try
        Local reader = Tar.Read(tar)
        For i = 0 Until reader.Count
            Local entry = reader.Entries[i]
            parts.Add("{entry.Name} {entry.Kind} {(Try reader.ReadAt(i)).Count}")
        End For
    Catch e As IOError
        parts.Add("{e.Kind}: {e.Message}")
    End Try
    Print("{title}: {parts.Join(", ")}")
End Function

Local ending: UInt8[] = []
ending.Resize(1024, 0)

// A V7 tar, without ustar's magic, whose folder is a regular entry with a '/' after its name.
Local old: UInt8[] = []
old.AddAll(Header("dir/", 0, 0, False))
old.AddAll(Header("dir/a.txt", 3, 0, False))
old.AddAll(Padded("abc".Bytes))
old.AddAll(ending)
List("old tar", old)
```

Expected: old tar: dir FOLDER 0, dir/a.txt FILE 3

Actual: old tar: dir FILE 0, dir/a.txt FILE 3

Suggested fix: Treat a '0', NUL or '7' entry whose name (before without_slash) ends with '/' as KIND_FOLDER with size 0, as GNU tar and libarchive do. Add a folder to make_archives.py's v7.tar.

Skeptic: Ran it (f6.fb, the old-tar case). Output: 'old tar: dir FILE 0, dir/a.txt FILE 3'. A ustar type-'0' 'dir/' gives the same: 'dir FILE 0'. tar.cpp:317-322 makes '0', NUL and '7' entries KIND_FILE, and without_slash (line 328) then drops the trailing '/'. GNU tar's extract and libarchive both take a regular entry with a trailing slash as a folder, and stdlib.md says old tars are read. Severity low: V7 tars with folders are rare, and every entry is still listed and readable, but a program extracting the tar would make a file named dir.
