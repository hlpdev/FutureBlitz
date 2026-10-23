"""Makes the archives in this folder that compress_archives.fb reads, for what common tools don't
write by default. 7z's, by 7z's format, with coders through Python's lzma module: a header that
isn't encoded, a stored solid folder with substreams and CRCs, chains of a branch or Delta filter
before LZMA2 or LZMA, BCJ before LZMA without its end marker, a coder that isn't supported, an anti-item, a Unix link, attributes for
only some files, a folder that says it's larger than its data, and a coder whose dictionary takes 4
GiB, as does an .xz stream's here. Tars, header by header: an old one without ustar's magic, a size in GNU's
base-256, links, a device, a FIFO, a global pax header, a GNU long link name, a pax size, and a
GNU sparse file. Zips, through Python's zipfile: a name in code page 437, a Unix link, an entry
that says it's encrypted, and one compressed by BZip2.

    python make_archives.py

Each archive goes in this folder, and data.fb, of their bytes, beside them.
"""

import io
import lzma
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

END, HEADER, MAIN_STREAMS, FILES_INFO = 0x00, 0x01, 0x04, 0x05
PACK_INFO, UNPACK_INFO, SUBSTREAMS = 0x06, 0x07, 0x08
SIZE, CRC, FOLDER, UNPACK_SIZE, UNPACK_STREAMS = 0x09, 0x0A, 0x0B, 0x0C, 0x0D
EMPTY_STREAM, EMPTY_FILE, ANTI, NAME, ATTRIBUTES = 0x0E, 0x0F, 0x10, 0x11, 0x15

COPY, LZMA1, LZMA2, DELTA, BCJ_X86, PPMD = b"\x00", b"\x03\x01\x01", b"\x21", b"\x03", \
    b"\x03\x03\x01\x03", b"\x03\x04\x01"


def number(value):
    """7z's number: as many leading 1 bits in the first byte as bytes follow, little-endian."""
    for extra in range(9):
        if extra == 8 or value < (1 << (8 * extra + 7 - extra)):
            break
    if extra == 8:
        return b"\xff" + value.to_bytes(8, "little")
    first = ((0xff << (8 - extra)) & 0xff) | (value >> (8 * extra))
    return bytes([first]) + (value & ((1 << (8 * extra)) - 1)).to_bytes(extra, "little")


def bits(values):
    out = bytearray((len(values) + 7) // 8)
    for i, value in enumerate(values):
        if value:
            out[i // 8] |= 0x80 >> (i % 8)
    return bytes(out)


def crc(data):
    return zlib.crc32(data).to_bytes(4, "little")


def coder(codec, properties=b""):
    flags = len(codec) | (0x20 if properties else 0)
    return bytes([flags]) + codec + (number(len(properties)) + properties if properties else b"")


def properties(filter_):
    return lzma._encode_filter_properties(filter_)


def archive(folders, files, folder_crcs=True):
    """A 7z of `folders`, each (coders, binds, packed bytes, its output sizes, its substreams),
    where coders are bytes made by coder(), binds (input, output) pairs, and substreams the
    unpacked bytes of each file in it, and `files`, each (name, has stream, is a file, is anti,
    attributes or None)."""
    packed = b"".join(folder[2] for folder in folders)
    streams = bytes([PACK_INFO]) + number(0) + number(len(folders))
    streams += bytes([SIZE]) + b"".join(number(len(folder[2])) for folder in folders)
    streams += bytes([END])
    streams += bytes([UNPACK_INFO, FOLDER]) + number(len(folders)) + b"\x00"
    for coders, binds, _, _, _ in folders:
        streams += number(len(coders)) + b"".join(coders)
        for input_, output in binds:
            streams += number(input_) + number(output)
    streams += bytes([UNPACK_SIZE])
    for _, _, _, sizes, _ in folders:
        streams += b"".join(number(size) for size in sizes)
    if folder_crcs:
        streams += bytes([CRC, 1]) + b"".join(crc(b"".join(folder[4])) for folder in folders)
    streams += bytes([END])
    streams += bytes([SUBSTREAMS, UNPACK_STREAMS])
    streams += b"".join(number(len(folder[4])) for folder in folders)
    streams += bytes([SIZE])
    for folder in folders:
        streams += b"".join(number(len(part)) for part in folder[4][:-1])
    unknown = [part for folder in folders for part in folder[4]
               if not (folder_crcs and len(folder[4]) == 1)]
    if unknown:
        streams += bytes([CRC, 1]) + b"".join(crc(part) for part in unknown)
    streams += bytes([END, END])

    info = number(len(files))
    empties = [file for file in files if not file[1]]
    info += property_(EMPTY_STREAM, bits([not file[1] for file in files]))
    info += property_(EMPTY_FILE, bits([file[2] for file in empties]))
    if any(file[3] for file in empties):
        info += property_(ANTI, bits([file[3] for file in empties]))
    names = b"\x00" + b"".join(file[0].encode("utf-16-le") + b"\x00\x00" for file in files)
    info += property_(NAME, names)
    if any(file[4] is not None for file in files):
        # As 7-Zip writes them: 1 when every file has them, or else 0 and which do.
        defined = [file[4] is not None for file in files]
        values = b"".join(file[4].to_bytes(4, "little") for file in files if file[4] is not None)
        which = b"\x01" if all(defined) else b"\x00" + bits(defined)
        info += property_(ATTRIBUTES, which + b"\x00" + values)
    info += bytes([END])

    header = bytes([HEADER, MAIN_STREAMS]) + streams + bytes([FILES_INFO]) + info + bytes([END])
    start = len(packed).to_bytes(8, "little") + len(header).to_bytes(8, "little") + crc(header)
    return b"7z\xbc\xaf\x27\x1c\x00\x04" + crc(start) + start + packed + header


def property_(kind, data):
    return bytes([kind]) + number(len(data)) + data


def raw(data, filters):
    return lzma.compress(data, format=lzma.FORMAT_RAW, filters=filters)


def lzma_literals(data):
    """An LZMA stream of data, each byte a literal, without the end marker, as 7-Zip writes an
    LZMA coder whose size its folder says, and its properties: lc 3, lp 0, pb 2, and a dictionary of
    1 MiB. Python's lzma always writes the end marker. It's LZMA's range coder, as the LZMA SDK
    has it."""
    out = bytearray()
    low, range_, cache, cache_size = 0, 0xFFFFFFFF, 0, 1

    def shift_low():
        nonlocal low, cache, cache_size
        if low < 0xFF000000 or low >= 1 << 32:
            carry = low >> 32
            temp = cache
            while True:
                out.append((temp + carry) & 0xFF)
                temp = 0xFF
                cache_size -= 1
                if cache_size == 0:
                    break
            cache = (low >> 24) & 0xFF
        cache_size += 1
        low = (low & 0x00FFFFFF) << 8

    def bit(probabilities, index, value):
        nonlocal low, range_
        bound = (range_ >> 11) * probabilities[index]
        if value == 0:
            range_ = bound
            probabilities[index] += (2048 - probabilities[index]) >> 5
        else:
            low += bound
            range_ -= bound
            probabilities[index] -= probabilities[index] >> 5
        while range_ < 1 << 24:
            range_ = (range_ << 8) & 0xFFFFFFFF
            shift_low()

    is_match = [1024] * (12 << 4)
    literals = [1024] * (0x300 << 3)
    previous = 0
    for position, byte in enumerate(data):
        # The state stays 0 after a literal, so each is a literal of state 0.
        bit(is_match, position & 3, 0)
        base = (previous >> 5) * 0x300
        symbol = 1
        for shift in range(7, -1, -1):
            value = (byte >> shift) & 1
            bit(literals, base + symbol, value)
            symbol = (symbol << 1) | value
        previous = byte
    for _ in range(5):
        shift_low()
    return bytes(out), b"\x5d" + (1 << 20).to_bytes(4, "little")


def tar_header(name, size, kind=b"0", link="", magic=True, size_field=None):
    """A tar's header, ustar's unless `magic` is False, for an old tar's, with its checksum."""
    header = bytearray(512)

    def put(at, data):
        header[at:at + len(data)] = data

    put(0, name.encode())
    put(100, b"%07o\0" % 0o644)
    put(108, b"%07o\0" % 0)
    put(116, b"%07o\0" % 0)
    put(124, size_field if size_field is not None else b"%011o\0" % size)
    put(136, b"%011o\0" % 0)
    header[156] = kind[0]
    put(157, link.encode())
    if magic:
        put(257, b"ustar\0")
        put(263, b"00")
    header[148:156] = b" " * 8
    header[148:156] = b"%06o\0 " % sum(header)
    return bytes(header)


def padded(data):
    return data + b"\0" * ((512 - len(data) % 512) % 512)


def pax(key, value):
    rest = len(f" {key}={value}\n".encode())
    length = rest + 1
    while len(str(length)) + rest != length:
        length += 1
    return f"{length} {key}={value}\n".encode()


def tars():
    made = {}
    end = b"\0" * 1024

    # An old tar, before ustar's magic, which ends where its entries do.
    made["v7.tar"] = tar_header("old.txt", 5, b"\0", magic=False) + padded(b"old!\n")

    # A size in GNU's base-256, a top bit and then the number, big-endian.
    big = b"\x80" + (5).to_bytes(11, "big")
    made["base256.tar"] = tar_header("big.txt", 0, size_field=big) + padded(b"big!\n") + end

    # Links, a device and a FIFO, which aren't entries, a global pax header, which the reader
    # leaves be, a GNU long link name, and a pax size in place of the header's.
    global_records = pax("comment", "for every entry after")
    size_records = pax("size", "3")
    long_target = "a/" * 60 + "target.txt"
    made["links.tar"] = (
        tar_header("pax_global_header", len(global_records), b"g") + padded(global_records) +
        tar_header("target.txt", 2) + padded(b"x\n") +
        tar_header("link", 0, b"2", "target.txt") +
        tar_header("hard", 0, b"1", "target.txt") +
        tar_header("dev", 0, b"3") +
        tar_header("fifo", 0, b"6") +
        tar_header("././@LongLink", len(long_target) + 1, b"K") +
        padded(long_target.encode() + b"\0") +
        tar_header("long-link", 0, b"2", long_target[:100]) +
        tar_header("PaxHeaders/sized.txt", len(size_records), b"x") + padded(size_records) +
        tar_header("sized.txt", 0) + padded(b"abc") + end)

    # GNU's sparse file, which the reader lists but doesn't read.
    made["sparse.tar"] = tar_header("sparse.bin", 4, b"S") + padded(b"spar") + end
    return made


def greedy_xz(data):
    """An .xz stream of data whose block's LZMA2 dictionary says 4 GiB, its header's CRC32 made
    again: the stream header's 12 bytes, then the block header, whose first byte is its size in
    4-byte units, less 1, and whose filter's one property byte comes just before its padding."""
    stream = bytearray(lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64))
    start = 12
    size = (stream[start] + 1) * 4
    header = stream[start:start + size - 4]
    # Its flags say one filter and no sizes, so the filter is ID 0x21, a property size of 1, and
    # the property.
    assert header[1] == 0x00 and header[2:4] == b"\x21\x01"
    header[4] = 40
    stream[start:start + size - 4] = header
    stream[start + size - 4:start + size] = zlib.crc32(header).to_bytes(4, "little")
    return bytes(stream)


def zips():
    made = {}
    date = (2026, 1, 1, 0, 0, 0)

    def zip_of(write):
        out = io.BytesIO()
        with zipfile.ZipFile(out, "w") as archive:
            write(archive)
        return out.getvalue()

    # A name in code page 437, which says it isn't UTF-8: "caf" and 0x82, which is "é" there.
    def cp437(archive):
        archive.writestr(zipfile.ZipInfo("cafX.txt", date), b"cafe\n")

    made["cp437.zip"] = zip_of(cp437).replace(b"cafX.txt", b"caf\x82.txt")

    # A Unix link, whose bytes are its target.
    def link(archive):
        info = zipfile.ZipInfo("link", date)
        info.create_system = 3
        info.external_attr = 0o120777 << 16
        archive.writestr(info, b"target.txt")
        archive.writestr(zipfile.ZipInfo("target.txt", date), b"x\n")

    made["link.zip"] = zip_of(link)

    # An entry that says it's encrypted, by its flag in both its headers.
    def secret(archive):
        archive.writestr(zipfile.ZipInfo("secret.txt", date), b"not really secret\n")

    encrypted = bytearray(zip_of(secret))
    encrypted[6] |= 1
    encrypted[encrypted.index(b"PK\x01\x02") + 8] |= 1
    made["encrypted.zip"] = bytes(encrypted)

    # BZip2, which minizip-ng isn't built with.
    def bzip2(archive):
        info = zipfile.ZipInfo("bzip2.txt", date)
        archive.writestr(info, b"compressed by BZip2\n" * 10, compress_type=zipfile.ZIP_BZIP2)

    made["bzip2.zip"] = zip_of(bzip2)
    return made


def main():
    one = b"first file\n"
    two = "second file, ünïcode\n".encode()
    three = bytes(range(256)) * 4
    made = {}

    # A stored, solid folder of three files, an empty file, and a folder, with each substream's
    # CRC after the folders, as the folder has none of its own.
    made["plain.7z"] = archive(
        [([coder(COPY)], [], one + two + three, [len(one + two + three)], [one, two, three])],
        [("plain/one.txt", True, True, False, 0x20),
         ("plain/ünïcode.txt", True, True, False, 0x20),
         ("plain/bytes.bin", True, True, False, 0x20),
         ("plain/empty.txt", False, True, False, 0x20),
         ("plain", False, False, False, 0x10)],
        folder_crcs=False)

    # x86 code through BCJ, then LZMA2, coded as 7-Zip writes a chain: the first coder takes the
    # packed stream, and the second takes the first's bytes, and gives the folder's.
    code = bytes([0xe8, 0x10, 0x20, 0x00, 0x00, 0x90, 0x90]) * 300
    lzma2 = {"id": lzma.FILTER_LZMA2, "preset": 6}
    made["chain.7z"] = archive(
        [([coder(LZMA2, properties(lzma2)), coder(BCJ_X86)], [(1, 0)],
          raw(code, [{"id": lzma.FILTER_X86}, lzma2]), [len(code), len(code)], [code])],
        [("code.bin", True, True, False, None)])

    # Two folders, each a file through Delta, then LZMA.
    samples = bytes((i * 3) % 256 for i in range(3000))
    delta = {"id": lzma.FILTER_DELTA, "dist": 2}
    lzma1 = {"id": lzma.FILTER_LZMA1, "preset": 6}
    made["delta.7z"] = archive(
        [([coder(LZMA1, properties(lzma1)), coder(DELTA, properties(delta))], [(1, 0)],
          raw(samples, [delta, lzma1]), [len(samples), len(samples)], [samples]),
         ([coder(LZMA1, properties(lzma1))], [], raw(one, [lzma1]), [len(one)], [one])],
        [("samples.bin", True, True, False, None), ("one.txt", True, True, False, None)])

    # Text through BCJ, then LZMA without its end marker, as 7-Zip writes LZMA:BCJ: the branch
    # filter holds its last bytes back until LZMA says it's ended, which its size says.
    text = b"plain text, no branch opcodes, " * 20
    literal, literal_properties = lzma_literals(text)
    assert lzma.decompress(literal_properties + len(text).to_bytes(8, "little") + literal,
                           format=lzma.FORMAT_ALONE) == text
    made["bcj.7z"] = archive(
        [([coder(LZMA1, literal_properties), coder(BCJ_X86)], [(1, 0)], literal,
          [len(text), len(text)], [text])],
        [("text.txt", True, True, False, None)])

    # PPMd, which liblzma doesn't have: the archive opens, and reading its file fails.
    made["ppmd.7z"] = archive(
        [([coder(PPMD, b"\x06\x00\x00\x10\x00")], [], b"not really PPMd", [len(one)], [one])],
        [("ppmd.txt", True, True, False, None)])

    # A Unix link, whose bytes are its target, and an anti-item, which isn't an entry.
    link = (0o120777 << 16) | 0x8000
    made["link.7z"] = archive(
        [([coder(COPY)], [], b"target.txt" + one, [len(one) + 10], [b"target.txt", one])],
        [("link", True, True, False, link), ("target.txt", True, True, False, 0x20),
         ("removed.txt", False, True, True, 0x20)])

    # Attributes for some files, which say which, and a folder known by its attributes alone,
    # which 7-Zip writes, though libarchive's reader takes the bits they're in for its own.
    made["partial.7z"] = archive(
        [([coder(COPY)], [], one, [len(one)], [one])],
        [("partial/one.txt", True, True, False, None), ("partial", False, True, False, 0x10)])

    # A folder that says it's far larger than what its coder gives, and a coder whose dictionary
    # takes 4 GiB, more than an array holds.
    made["liar.7z"] = archive(
        [([coder(LZMA2, properties(lzma2))], [], raw(one, [lzma2]), [2_000_000_000], [one])],
        [("liar.txt", True, True, False, None)], folder_crcs=False)
    made["greedy.7z"] = archive(
        [([coder(LZMA2, b"\x28")], [], raw(one, [lzma2]), [len(one)], [one])],
        [("greedy.txt", True, True, False, None)])

    made.update(tars())
    made.update(zips())
    made["greedy.xz"] = greedy_xz(one)
    for name, data in made.items():
        (HERE / name).write_bytes(data)

    lines = [
        "// The bytes of the archives in this folder, which make_archives.py made, as array",
        "// literals, since a program can't read a file until std.io's files.",
        "",
        "Import std.core",
        "",
    ]
    for name, data in made.items():
        stem, kind = name.rsplit(".", 1)
        function = stem.capitalize() + kind.capitalize()
        lines.append(f"// {name}, {len(data)} bytes.")
        lines.append(f"Public Function {function}(): UInt8[]")
        lines.append("    Return [")
        for at in range(0, len(data), 14):
            row = ", ".join(f"0x{byte:02x}" for byte in data[at:at + 14])
            lines.append(f"        {row}{'' if at + 14 >= len(data) else ','}")
        lines.append("    ]")
        lines.append("End Function")
        lines.append("")
    (HERE / "data.fb").write_text("\n".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
