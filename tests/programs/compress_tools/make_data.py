"""Writes data.fb, of the bytes of the files in files/, which common tools made, as array literals
that compress_tools.fb reads, since a program can't read a file until std.io's files.

    python make_data.py

The files were made on Windows, from a folder sample/ of hello.txt ("Hello, world!" and a line
break), poem.txt (30 lines counting down, "30 bottles of milk on the wall, 30 bottles of milk.",
each with a line break), notes/ünïcode.txt ("Grüße aus Köln" and a line break), notes/empty.txt,
and notes/<a folder with a long name>/deep.txt ("deep" and a line break), each last changed at
2026-01-01 00:00, and poem.txt, a copy of sample/poem.txt, by:

    gzip -9 -c poem.txt > poem.txt.gz                              (GNU gzip 1.14)
    xz -c poem.txt > poem.txt.xz                                   (xz 5.8.3)
    lz4 -c poem.txt > poem.txt.lz4             (lz4 1.10.0, built from LZ4's own source)
    git hash-object -w poem.txt, its loose object as poem.txt.git: a Zlib stream of "blob 1542",
        a NUL, and the file                                       (Git 2.55.0)
    tar --owner=root:0 --group=root:0 -b1 --format=gnu -cf sample.tar sample   (GNU tar 1.35)
    tar --owner=root:0 --group=root:0 -b1 --format=pax \\
        --pax-option=delete=atime,delete=ctime -cf sample-pax.tar sample
    tar --owner=root:0 --group=root:0 -czf sample.tar.gz sample
    tar --uid 0 --gid 0 --uname root --gname root, then:          (Windows' bsdtar 3.8.9)
        --zstd --format raw -cf poem.txt.zst poem.txt
        --format zip -cf sample.zip sample
        --zstd -cf sample.tar.zst sample
        -cJf sample.tar.xz sample
        --format 7zip -cf sample.7z sample
        --format 7zip --options 7zip:compression=lzma2 -cf sample-lzma2.7z sample
"""

from pathlib import Path

HERE = Path(__file__).resolve().parent
FILES = [
    ("PoemGz", "poem.txt.gz"), ("PoemXz", "poem.txt.xz"), ("PoemZst", "poem.txt.zst"),
    ("PoemLz4", "poem.txt.lz4"), ("PoemGit", "poem.txt.git"),
    ("SampleZip", "sample.zip"), ("SampleTar", "sample.tar"), ("SamplePaxTar", "sample-pax.tar"),
    ("SampleTarGz", "sample.tar.gz"), ("SampleTarZst", "sample.tar.zst"),
    ("SampleTarXz", "sample.tar.xz"), ("Sample7z", "sample.7z"),
    ("SampleLzma27z", "sample-lzma2.7z"),
]


def main():
    lines = [
        "// The bytes of the files in files/, which common tools made, as array literals, since a",
        "// program can't read a file until std.io's files; make_data.py says how each was made.",
        "",
        "Import std.core",
        "",
    ]
    for function, name in FILES:
        data = (HERE / "files" / name).read_bytes()
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
