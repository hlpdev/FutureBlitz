# The review of M6 step 30's static files (HttpRouter.Files: a folder's files with ETags, 304s, and ranges)

7 confirmed findings: 1 high, 1 medium, 5 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## files-1 (high): Dot files and dot folders are served on Windows by their 8.3 short names

Status: fixed in df6a52c3: once a file is found, its real path, through the runtime's new fb_io_real_path (libuv's uv_fs_realpath, which gives each name as the disk has it, the long one on Windows), must be inside the folder's, with every name past it one Servable takes, so SECRET~1, which is .secret, is a 404; http_files asks for SECRET~1, GIT~1/config, GIT~1/, and .git/config, each a 404

Where: std/net/http/files.fb:168

Servable refuses a segment that starts with '.', and the docs (files.fb's comment and stdlib.md) say that refusal holds 'whatever the OS'. NTFS also gives each long name an 8.3 short name: .secret is SECRET~1 and .git is GIT~1. Those short names pass Servable, and File.MetadataAsync and ReadBytesAsync resolve them to the hidden file. On a default Windows volume, anyone can fetch a served folder's .env, .git/config, and so on. Shown on Windows 11, in both debug and release builds.

```
// Windows gives each long name an 8.3 short name too: .secret's is SECRET~1 and .git's GIT~1,
// so a dot file, which Files says is a 404 on every OS, is served by its short name.
Import std.core
Import std.collections
Import std.crypto
Import std.io
Import std.net
Import std.net.http
Import std.text

Local folder = ((Try? Dir.Temp()) ?? Path(".")).Join(Path("fb-rv-short-{Hex.Encode(SecureRandom.Bytes(8))}"))
Try
    Dir.CreateAll(folder.Join(Path(".git")))
    File.WriteText(folder.Join(Path(".secret")), "hidden")
    File.WriteText(folder.Join(Path(".git/config")), "[remote] url = secret")

    Local server = HttpServer()
    server.Files("/files", folder)
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local url = "http://127.0.0.1:{server.LocalAddress.Port}/files"
    Local client = HttpClient()
    For name In [".secret", "SECRET~1", ".git/config", "GIT~1/config"]
        Local response = Await client.GetAsync("{url}/{name}")
        Print(name, response.Status, response.Text)
    End For
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
Try? Dir.RemoveAll(folder)
```

Expected: .secret 404 
SECRET~1 404 
.git/config 404 
GIT~1/config 404 

Actual: .secret 404 
SECRET~1 200 hidden
.git/config 404 
GIT~1/config 200 [remote] url = secret

Suggested fix: After the file is found, check the name it actually has on disk, not the one the request spelled. For example, compare each segment with the directory entry's real name (FindFirstFile's cFileName, or GetFinalPathNameByHandle on the opened file) and give a 404 when they differ or when the real name starts with '.'. Or refuse a segment that has '~' followed by a digit on every OS. Add SECRET~1 and GIT~1/config to http_files' 404 list.

Skeptic: Ran rv-files/v4/f1.fb in debug and release with the rev-4 fb.exe. Both printed '.secret 404', 'SECRET~1 200 hidden', '.git/config 404', 'GIT~1/config 200 [remote] url = secret'. Servable (files.fb:168-185) checks only the segment as the request spells it: a leading '.', a trailing '.' or space, '\', ':', controls, and device names. 'SECRET~1' passes, and File.MetadataAsync and ReadBytesAsync resolve it to .secret. That contradicts files.fb's comment ('starts with a '.' ... is a 404 ... whatever the OS') and stdlib.md ('a 404 on every OS'), and it exposes .git and .env on default NTFS volumes.

## files-2 (medium): Symbolic links and junctions in the folder are followed outside it, though the docs say nothing outside is reached

Status: fixed in df6a52c3: by the same check, as the coordinator called it, a file whose real path isn't inside the folder's is a 404, so a link out of the folder, to a file or to a folder, reaches nothing; http_files makes both kinds of link (CreateSymbolicLinkA on Windows, symlink elsewhere) and asks for each, a 404

Where: std/net/http/files.fb:56

stdlib.md says a refused segment is a 404 on every OS, 'so nothing outside the folder is reached', and the lane's settled call is 'never anything outside the folder'. Answer follows a symbolic link (link.txt -> ../outside/secret.txt) and a directory junction or symlink (dir -> ../outside) to files outside the folder and serves them with 200. The task names links as part of what the path checks should cover.

```
// A link in the served folder to a file outside it: Files' docs say nothing outside the folder
// is reached, so it should be a 404.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text

Extern "C"
    Function system(command: CString): Int32
End Extern

Local base = ((Try? Dir.Temp()) ?? Path(".")).Join(Path("fb-rv-links-{Hex.Encode(SecureRandom.Bytes(8))}"))
Local root = base.Join(Path("root"))
Local outside = base.Join(Path("outside"))
Try
    Dir.CreateAll(root)
    Dir.CreateAll(outside)
    File.WriteText(outside.Join(Path("secret.txt")), "outside the folder")
    // A symbolic link and (on Windows) a junction, made by the shell.
    system("mklink \"{root.Join(Path("link.txt"))}\" \"{outside.Join(Path("secret.txt"))}\" >NUL 2>&1 || ln -s \"{outside.Join(Path("secret.txt"))}\" \"{root.Join(Path("link.txt"))}\"")
    system("mklink /J \"{root.Join(Path("dir"))}\" \"{outside}\" >NUL 2>&1 || ln -s \"{outside}\" \"{root.Join(Path("dir"))}\"")

    Local server = HttpServer()
    server.Files("/files", root)
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local url = "http://127.0.0.1:{server.LocalAddress.Port}/files"
    Local client = HttpClient()
    For name In ["link.txt", "dir/secret.txt"]
        Local response = Await client.GetAsync("{url}/{name}")
        Print(name, response.Status, response.Text)
    End For
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
Try? Dir.RemoveAll(base)
```

Expected: link.txt 404 
dir/secret.txt 404 
(or, if following links is intended, docs that say so instead of 'nothing outside the folder is reached')

Actual: link.txt 200 outside the folder
dir/secret.txt 200 outside the folder

Suggested fix: Either refuse links: give a 404 when any segment's metadata (without following links) IsLink, or when the file's resolved real path doesn't start with the folder's resolved real path. Or keep following them, as nginx and Go's http.Dir do, and change files.fb's comment and stdlib.md to say that links in the folder are followed, so the guarantee covers the request's spelling only. Add a test either way.

Skeptic: Ran rv-files/v4/f2.fb in debug and release. mklink made a file symlink and a junction (both exited 0), and the program printed 'link.txt 200 outside the folder' and 'dir/secret.txt 200 outside the folder'. Answer (files.fb:56-91) calls File.MetadataAsync and ReadBytesAsync on the joined path, which follow links, and never checks the resolved path or IsLink. files.fb:23-24 says 'so a request never reaches outside the folder', stdlib.md says 'so nothing outside the folder is reached', and the lane's call is 'never anything outside the folder'. So either the code or those docs must change. It is medium, not high, because the links have to be placed in the served folder by its owner.

## files-4 (low): A folder's 301 rebuilds the query from QueryParams and changes it

Status: fixed in df6a52c3: the folder's redirect keeps the query as the request's target has it; http_files asks for docs with ?flag, ?a&b=1, ?x=1;y=2, and ?

Where: std/net/http/files.fb:61

The redirect from a folder's path to the same path with '/' writes "?{request.Query}", which re-serializes the parsed QueryParams instead of keeping the request's raw query. A query that isn't name=value pairs comes back different: '?flag' becomes '?flag=', '?a&b=1' becomes '?a=&b=1', and '?x=1;y=2' becomes '?x=1%3By%3D2', which turns y=2 into part of x's value.

```
// A folder's redirect rebuilds the query from QueryParams, so a query that isn't name=value
// pairs comes back changed.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text

Local folder = ((Try? Dir.Temp()) ?? Path(".")).Join(Path("fb-rv-query-{Hex.Encode(SecureRandom.Bytes(8))}"))
Try
    Dir.CreateAll(folder.Join(Path("docs")))
    File.WriteText(folder.Join(Path("docs/index.html")), "<p>docs</p>")

    Local server = HttpServer()
    server.Files("/files", folder)
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    For query In ["?flag", "?a&b=1", "?x=1;y=2", "?"]
        Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
        Local request = "GET /files/docs{query} HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n"
        Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
        Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
        For line In answer.Split("\r\n")
            If line.StartsWith("Location: ") Then Print(query, "->", line)
        End For
    End For
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
Try? Dir.RemoveAll(folder)
```

Expected: ?flag -> Location: /files/docs/?flag
?a&b=1 -> Location: /files/docs/?a&b=1
?x=1;y=2 -> Location: /files/docs/?x=1;y=2
? -> Location: /files/docs/ (or /files/docs/?)

Actual: ?flag -> Location: /files/docs/?flag=
?a&b=1 -> Location: /files/docs/?a=&b=1
?x=1;y=2 -> Location: /files/docs/?x=1%3By%3D2
? -> Location: /files/docs/

Suggested fix: Use the request target's raw query: the text after the first '?' in request.Target, or the Url's raw Query, which HttpRequest could keep. Add a '?flag' case to http_files.

Skeptic: Ran rv-files/v4/f4.fb with a raw TCP request. It printed '?flag -> Location: /files/docs/?flag=', '?a&b=1 -> Location: /files/docs/?a=&b=1', '?x=1;y=2 -> Location: /files/docs/?x=1%3By%3D2', and '? -> Location: /files/docs/'. files.fb:61 builds the query from the parsed QueryParams ('?{request.Query}') instead of the raw query in request.Target, so a 301 that should only add '/' changes the query's meaning (y=2 becomes part of x's value).

## files-5 (low): Valid Range headers are ignored: a range unit in another case, and a last-pos or suffix of more than 18 digits

Status: fixed in df6a52c3: RangeOf reads the unit before the '=' whatever its letters' case, and a position of more digits than an Int64 holds as past the end; http_files asks for Bytes=2-3, BYTES=2-3, bytes=7-99999999999999999999, bytes=-99999999999999999999 (206s), and bytes=99999999999999999999- (a 416)

Where: std/net/http/files.fb:144

RFC 9110 section 14.1 says range unit names are case-insensitive, but RangeOf checks StartsWith("bytes="), so 'Bytes=2-3' gets the whole file. Section 14.1.2 says a last-pos at or past the representation's length means the rest of the file, but DigitsOf returns Null past 18 digits, so 'bytes=7-99999999999999999999' and 'bytes=-99999999999999999999' are treated as invalid and get the whole file instead of a 206. A server may ignore Range, so the content isn't wrong, but the doc comment calls these 'a Range it serves the whole file for: of another unit, ... or that isn't a Range's', and they are valid single byte ranges.

```
// RFC 9110: a range unit's name is case-insensitive (14.1), and a last-pos at or past the end
// means the rest of the file (14.1.2), however many digits it has.
Import std.core
Import std.collections
Import std.crypto
Import std.io
Import std.net
Import std.net.http
Import std.text

Local folder = ((Try? Dir.Temp()) ?? Path(".")).Join(Path("fb-rv-ranges-{Hex.Encode(SecureRandom.Bytes(8))}"))
Try
    Dir.CreateAll(folder)
    File.WriteText(folder.Join(Path("digits.txt")), "0123456789")

    Local server = HttpServer()
    server.Files("/files", folder)
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local url = "http://127.0.0.1:{server.LocalAddress.Port}/files/digits.txt"
    Local client = HttpClient()
    For range In ["bytes=2-3", "Bytes=2-3", "BYTES=2-3", "bytes=7-99999999999999999999",
                  "bytes=-99999999999999999999"]
        Local headers = Headers()
        headers.Add("Range", range)
        Local response = Await client.SendAsync(HttpMethod.GET, url, Null, headers)
        Print(range, response.Status, response.Text)
    End For
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
Try? Dir.RemoveAll(folder)
```

Expected: bytes=2-3 206 23
Bytes=2-3 206 23
BYTES=2-3 206 23
bytes=7-99999999999999999999 206 789
bytes=-99999999999999999999 206 0123456789

Actual: bytes=2-3 206 23
Bytes=2-3 200 0123456789
BYTES=2-3 200 0123456789
bytes=7-99999999999999999999 200 0123456789
bytes=-99999999999999999999 200 0123456789

Suggested fix: Compare the unit case-insensitively (find the '=' and use EqualsIgnoreCase("bytes")). For last-pos and suffix-length, accept any run of digits and clamp it: when DigitsOf overflows but the text is all digits, treat the value as larger than size. Add these cases to http_files' range list.

Skeptic: Ran rv-files/v4/f5.fb in debug and release. 'bytes=2-3' got 206, but 'Bytes=2-3', 'BYTES=2-3', 'bytes=7-99999999999999999999', and 'bytes=-99999999999999999999' all got 200 with the whole file. RangeOf (files.fb:144) uses a case-sensitive StartsWith("bytes="), although RFC 9110 section 14.1 makes range unit names case-insensitive. DigitsOf returns Null on overflow, although section 14.1.2 says a last-pos at or past the length means the rest of the file. RFC 9110 lets a server ignore Range, but RangeOf's own comment limits the whole-file answer to another unit, several ranges, or a malformed Range, and stdlib.md promises a 206 for one range of bytes. These requests are all one valid range of bytes.

## files-6 (low): If-Modified-Since in RFC 850 or asctime form is ignored

Status: fixed in df6a52c3: ParseHttpDate reads IMF-fixdate, RFC 850's form, with RFC 9110's rule for its two-digit year, and asctime's; http_files sends each, the future ones 304s and the older ones 200s

Where: std/net/http/files.fb:257

RFC 9110 section 5.6.7 says a recipient that parses an HTTP-date MUST accept all three formats: IMF-fixdate, RFC 850, and asctime. ParseHttpDate reads only IMF-fixdate, so the same instant in either obsolete form gets 200 with the whole file instead of 304. ('01-Jan-70' in RFC 850 form is 2070, per RFC 9110's 50-year rule for two-digit years.)

```
// RFC 9110's section 5.6.7: a recipient MUST accept all three HTTP-date formats, so the same
// instant in RFC 850's and asctime's forms is as current as in IMF-fixdate's.
Import std.core
Import std.collections
Import std.crypto
Import std.io
Import std.net
Import std.net.http
Import std.text

Local folder = ((Try? Dir.Temp()) ?? Path(".")).Join(Path("fb-rv-dates-{Hex.Encode(SecureRandom.Bytes(8))}"))
Try
    Dir.CreateAll(folder)
    File.WriteText(folder.Join(Path("a.txt")), "text")

    Local server = HttpServer()
    server.Files("/files", folder)
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local url = "http://127.0.0.1:{server.LocalAddress.Port}/files/a.txt"
    Local client = HttpClient()
    For date In ["Wed, 01 Jan 2070 00:00:00 GMT", "Wednesday, 01-Jan-70 00:00:00 GMT",
                 "Wed Jan  1 00:00:00 2070"]
        Local headers = Headers()
        headers.Add("If-Modified-Since", date)
        Print(date, (Await client.SendAsync(HttpMethod.GET, url, Null, headers)).Status)
    End For
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
Try? Dir.RemoveAll(folder)
```

Expected: Wed, 01 Jan 2070 00:00:00 GMT 304
Wednesday, 01-Jan-70 00:00:00 GMT 304
Wed Jan  1 00:00:00 2070 304

Actual: Wed, 01 Jan 2070 00:00:00 GMT 304
Wednesday, 01-Jan-70 00:00:00 GMT 200
Wed Jan  1 00:00:00 2070 200

Suggested fix: Have ParseHttpDate also try the RFC 850 pattern ("EEEE, dd-MMM-yy HH:mm:ss 'GMT'", with RFC 9110's rule for two-digit years) and the asctime pattern ("EEE MMM d HH:mm:ss yyyy", where a one-digit day is padded with a space). Add both forms to http_files' conditions.

Skeptic: Ran rv-files/v4/f6.fb. It printed 'Wed, 01 Jan 2070 00:00:00 GMT 304', 'Wednesday, 01-Jan-70 00:00:00 GMT 200', and 'Wed Jan  1 00:00:00 2070 200'. ParseHttpDate (files.fb:257-260) reads only IMF-fixdate. RFC 9110 section 5.6.7 says a recipient that parses an HTTP-date MUST accept all three formats. The asctime case is unambiguously 2070. The RFC 850 '70' is also 2070 under the 50-year rule, since it is 44 years ahead of 2026.

## files-7 (low): A file over the 2 GiB read limit gets a 404, as if it weren't there

Status: fixed in df6a52c3: a file past 2 GiB is a 500 before it's read, and a read that fails other than for a file that's gone is a 500 too; http_files makes a sparse 2,200,000,000-byte file and asks for a range of it, a 500; stdlib.md says so

Where: std/net/http/files.fb:89

stdlib.md says 'A file is read whole, 2 GiB at most', but it doesn't say what happens to a bigger file. Answer catches ReadBytesAsync's IOError (OTHER, too big) and returns 404 Not Found, even though the metadata it just read shows the file exists and is a regular file. A Range request for its first bytes gets the same 404. That misleads both clients and the person debugging the server.

```
// A file past the 2 GiB that Files reads whole is there, but Files answers as if it weren't.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text

Extern "C"
    Function system(command: CString): Int32
End Extern

Local folder = ((Try? Dir.Temp()) ?? Path(".")).Join(Path("fb-rv-big-{Hex.Encode(SecureRandom.Bytes(8))}"))
Try
    Dir.CreateAll(folder)
    Local big = folder.Join(Path("big.bin"))
    system("fsutil file createnew \"{big}\" 2200000000 >NUL 2>&1 || truncate -s 2200000000 \"{big}\"")
    Print("on disk:", File.Metadata(big).Size)

    Local server = HttpServer()
    server.Files("/files", folder)
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local url = "http://127.0.0.1:{server.LocalAddress.Port}/files/big.bin"
    Local headers = Headers()
    headers.Add("Range", "bytes=0-3")
    Print("served:", (Await HttpClient().SendAsync(HttpMethod.GET, url, Null, headers)).Status)
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
Try? Dir.RemoveAll(folder)
```

Expected: on disk: 2200000000
served: 500 (or another status that isn't 404, and docs that name it)

Actual: on disk: 2200000000
served: 404

Suggested fix: Check metadata.Size against the read limit before reading, and answer a file that's too big with a 500 (or 413/501), not a 404. Keep 404 for a read that fails because the file is gone. Say in stdlib.md which status a file too big to serve gets.

Skeptic: Ran rv-files/v4/f7.fb, which made a 2,200,000,000-byte file with fsutil. It printed 'on disk: 2200000000' and 'served: 404' for a Range of bytes 0-3. files.fb:86-91 catches any IOError from ReadBytesAsync and returns NOT_FOUND, after MetadataAsync has already shown a regular file. Files' doc says a 404 is for a refused segment or 'what isn't there', and this file is there. stdlib.md says only '2 GiB at most', without naming a status, so it is low.

## files-8 (low): stdlib.md lost the backslash in the list of refused characters

Status: fixed in a3db0356: stdlib.md says "a backslash"; docs only, so no test

Where: docs/stdlib.md:1376

The new paragraph says a segment that 'has a '', a ':', or a control character' is a 404. The '\' is missing from the source text, so the docs list an empty character instead of the backslash that Servable refuses (0x5C).

```
// A segment with a backslash is a 404, which stdlib.md should name.
Import std.core
Import std.collections
Import std.crypto
Import std.io
Import std.net
Import std.net.http
Import std.text

Local folder = ((Try? Dir.Temp()) ?? Path(".")).Join(Path("fb-rv-doc-{Hex.Encode(SecureRandom.Bytes(8))}"))
Try
    Dir.CreateAll(folder)
    File.WriteText(folder.Join(Path("a.txt")), "text")
    Local server = HttpServer()
    server.Files("/files", folder)
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local url = "http://127.0.0.1:{server.LocalAddress.Port}/files/x%5Ca.txt"
    Print((Await HttpClient().GetAsync(url)).Status)
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
Try? Dir.RemoveAll(folder)
```

Expected: 404, with stdlib.md line 1376 reading "has a '\', a ':', or a control character"

Actual: 404, with stdlib.md line 1376 reading "has a '', a ':', or a control character"

Suggested fix: Write the character as `'\'` (in a code span) or as 'a backslash' in docs/stdlib.md line 1376.

Skeptic: grep -n "has a ''" on rev-4's docs/stdlib.md matches line 1376: 'starts with a '.', has a '', a ':', or a control character'. The backslash is missing from the source. rv-files/v4/f8.fb shows /files/x%5Ca.txt is a 404, so the docs leave out a character that Servable (0x5C) does refuse.

## Refuted by the skeptics (no action)

- files-3: A prefix ending with '/' is accepted without a panic, and then serves nothing at prefix/name. Reproduced the behavior with rv-files/v4/f3.fb in debug and release: '/assets/style.css 404' and '/assets//style.css 200'. But it does not contradict the docs. server.fb's SegmentsOf (lines 311-327) accepts a route path with a trailing '/' (an empty last segment), so '/assets/' is 'a path as routes have them'. Files' doc says 'a GET ... of prefix/a/b.png gives folder's a/b.png', and for prefix '/assets/' that is literally '/assets//a/b.png', which is what it serves. Group joins a prefix's segments the same way (Joined(Prefix, SegmentsOf(prefix))), so Files matches the router's existing convention. It is a usability footgun worth considering, but not a bug against the spec, stdlib.md, or the milestones row.
