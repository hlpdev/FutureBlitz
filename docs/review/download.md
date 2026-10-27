# The review of M6 step 32 audit gap: std.net.http's Download to a file (step 29)

1 confirmed findings: 0 high, 0 medium, 1 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## runtime-1 (low): An empty path isn't refused before the transfer: the whole body is downloaded, then the rename fails

Status: fixed in b14f190e: atomic_file_t::open gives ENOENT for an empty path, before it makes a temporary file, so fb_http_to_file refuses it before the transfer starts; http_download_file downloads to it, blocking and awaited, beside the missing folder and the folder at the path, and checks each is refused with progress told nothing and, from the test server's refused URL, an IOError and not a NetError of REFUSED, so nothing was sent

Where: runtime/src/files.cpp:1082

atomic_file_t::open("") only checks for a NUL and for a folder at the path. stat("") fails, so it goes on: temporary_beside("") names a temporary file in the working folder, which opens fine. fb_http_to_file then reports success, the transfer runs to its end, and every byte of the body is written to that temporary file. Only commit()'s rename to "" fails, with ENOENT. The download ends in the right IOError (NOT_FOUND) and leaves nothing behind, but it sends the request, writes the whole body to the working folder, and tells progress first. Every other unwritable path ('Path(".")', a folder, or a missing folder) throws before anything is sent, which is what client.fb's Download comment and docs/stdlib.md say. File.WriteTextAtomic(Path("")) takes the same route, which costs nothing there. For a download it costs the whole transfer. Checked in a debug build under the HTTP test server (build-lane1-verify's fb_http_test_server.exe).

```
// A download to an empty path throws before anything's sent, as a folder that isn't there does.
Import std.core
Import std.io
Import std.net.http

Global base = Input() ?? ""
Global told = 0

Try
    Http.Download("{base}/bytes/200000?pause=100000", Path(""), Function(got: Int64, all: Int64)
        told += 1
    End Function)
    Print("done")
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
Catch e As IOError
    Print("IOError {e.Kind}")
End Try
Print("progress told {told > 0}")
```

Expected: IOError NOT_FOUND
progress told False

Actual: IOError NOT_FOUND
progress told True

Suggested fix: In atomic_file_t::open, return UV_ENOENT for an empty path (after the has_nul check, before linked_file and the temporary file), so fb_http_to_file refuses it before the transfer starts. Add the case to tests/programs/http_download_file.fb (blocking and awaited) next to the missing folder case.

Skeptic: I built the finding's program, extended to three paths, at C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-download/empty-path/p.fb. It downloads /bytes/200000?pause=100000 to Path(""), then Path("nofolder/x.bin"), then Path("."). I ran it under build-lane1-verify's fb_http_test_server.exe in both debug and --release. Both builds printed: 'IOError NOT_FOUND / progress told True' for Path(""), 'IOError NOT_FOUND / progress told False' for the missing folder, and 'IOError IS_A_DIRECTORY / progress told False' for '.'. No temporary file was left in the working folder. So the empty path is the only unwritable path that sends the request and runs the transfer to its end before failing. The other two are refused in WriteTo, before Run. The code agrees. In runtime/src/files.cpp, atomic_file_t::open (around line 1082) checks only has_nul, then whether the path is a folder; stat("") fails, so that check passes. temporary_beside("") gets an empty folder from split_path and opens '.fb-XXXX' in the working folder. fb_http_to_file (runtime/src/http.cpp:1175) then returns 0, and only commit()'s rename to "" fails, with ENOENT. The result is the right IOError and nothing is left behind, but the whole body is fetched and progress is told first. That doesn't match client.fb's Download comment, which says an unwritable target is refused before anything's sent, the way the other unwritable paths are. Low: the final error is right and nothing leaks, so it only wastes the transfer.

## Refuted by the skeptics (no action)

- std-1 (its test added in b14f190e, as the coordinator asked: the paths that can't be written are downloaded from the refused URL too, which gives an IOError, not a NetError of REFUSED, and progress is told nothing): The test doesn't check that a missing folder fails before anything's sent. Refuted: the code does what stdlib.md and Download's comment say, and the finding names no wrong behavior and no place where the code contradicts the spec, stdlib.md or the milestones row. It only points at a gap in the test. Download in std/net/http/client.fb calls transfer.WriteTo(path) before transfer.Run(progress), so the temporary file is made before the request goes out. I couldn't find a built fb_http_test_server in build-rev4, so I pointed the download at a port nothing listens on instead. The program is C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-download/std-1/missing.fb. It downloads http://127.0.0.1:1/x into temp/"missing"/f.bin, blocking and then awaited, and counts progress calls. Debug and release both printed "IOError NOT_FOUND 0" twice. A control download into the existing folder printed "NetError REFUSED 0". If the request had gone out first, the missing folder would also have given NetError REFUSED, so the folder's IOError is thrown before anything is sent, as documented. The test gap is real: http_download_file.fb line 88 can't tell an early NOT_FOUND from a NOT_FOUND at rename time. Even so, it's a possible test addition, not a bug in this step's code.
