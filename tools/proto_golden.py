#!/usr/bin/env python3
"""Makes the protobuf test programs' golden bytes with protoc, which the build doesn't need, and
checks with protoc the bytes the programs print.

    python tools/proto_golden.py --protoc <protoc 36.2> [--check]

Each tests/programs/<name>/ folder with .txtpb files gets a golden.fb: for each .txtpb, a message
in protobuf's text format whose first lines name its .proto file and type, as protobuf's own text
files do,

    # proto-file: scalars.proto
    # proto-message: test.scalars.Scalars

and, when its imports go from another folder, `# proto-include: protos`, which protoc takes as
the folder its imports' paths start from, and the .proto file's path does too. It gets
a Function that gives the bytes `protoc --encode` writes for it, named after the file: MaxBytes
for max.txtpb. A .merge file names two .txtpb files of one type, a line each, and its Function gives
what protoc writes for the message it reads from their bytes one after the other, which protobuf
merges.

Then each line `encoded <file>.txtpb [bytes]` in tests/programs/<name>.out, which the program
printed, has to be what protoc reads as the same message the .txtpb is. --check only checks those
lines, and that golden.fb is what this script makes.

The committed bytes are protoc 36.2's, from
https://github.com/protocolbuffers/protobuf/releases/tag/v36.2.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROGRAMS = ROOT / "tests" / "programs"
HEADER = re.compile(r"#\s*proto-(file|message|include):\s*(\S+)")
ENCODED = re.compile(r"^encoded (\S+\.txtpb) \[([0-9, ]*)\]$", re.M)


def run_protoc(protoc: str, folder: Path, mode: str, message: str, proto: str,
               given: bytes) -> bytes:
    include, _, proto = proto.rpartition("|")
    done = subprocess.run([protoc, f"--{mode}={message}", f"-I{include or '.'}", proto],
                          cwd=folder, input=given, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if done.returncode != 0:
        sys.exit(f"protoc --{mode} of {message} in {folder.name} failed:\n"
                 + done.stderr.decode(errors="replace"))
    return done.stdout


def header(path: Path) -> tuple[str, str]:
    """A .txtpb's .proto file, after its include folder and a '|' when it has one, and type."""
    found = dict(match.groups() for match in HEADER.finditer(path.read_text(encoding="utf-8")))
    if "file" not in found or "message" not in found:
        sys.exit(f"{path} needs '# proto-file:' and '# proto-message:' lines")
    proto = found["file"]
    if "include" in found:
        proto = f"{found['include']}|{proto}"
    return proto, found["message"]


def encoded(protoc: str, path: Path) -> tuple[bytes, str, str]:
    """A .txtpb's bytes, or a .merge's, with its .proto file and type."""
    if path.suffix == ".merge":
        parts = [path.parent / line.strip() for line in path.read_text(encoding="utf-8").splitlines()
                 if line.strip()]
        joined = b"".join(encoded(protoc, part)[0] for part in parts)
        proto, message = header(parts[0])
        text = run_protoc(protoc, path.parent, "decode", message, proto, joined)
        return run_protoc(protoc, path.parent, "encode", message, proto, text), proto, message
    proto, message = header(path)
    text = path.read_bytes()
    return run_protoc(protoc, path.parent, "encode", message, proto, text), proto, message


def function_name(path: Path) -> str:
    return "".join(part.capitalize() for part in path.stem.split("_")) + "Bytes"


def golden(protoc: str, folder: Path) -> str:
    lines = [
        f"// Made by tools/proto_golden.py with protoc 36.2 from the .txtpb and .merge files beside",
        f"// it: each Function gives the bytes protoc wrote for one. Make it again with that script.",
    ]
    for path in sorted([*folder.glob("*.txtpb"), *folder.glob("*.merge")]):
        data, proto, message = encoded(protoc, path)
        values = [str(byte) for byte in data]
        lines.append("")
        lines.append(f"// {path.name}, a {message} of {proto.rpartition('|')[2]}.")
        lines.append(f"Public Function {function_name(path)}(): UInt8[]")
        rows = [", ".join(values[i:i + 16]) for i in range(0, len(values), 16)]
        lines.append("    Return [" + (",\n            ".join(rows)) + "]")
        lines.append("End Function")
    return "\n".join(lines) + "\n"


def check_encoded(protoc: str, folder: Path) -> int:
    """Each `encoded` line the program printed reads, with protoc, as its .txtpb's message."""
    out = PROGRAMS / f"{folder.name}.out"
    checked = 0
    for name, listed in ENCODED.findall(out.read_text(encoding="utf-8") if out.exists() else ""):
        expected, proto, message = encoded(protoc, folder / name)
        given = bytes(int(value) for value in listed.split(",") if value.strip())
        theirs = run_protoc(protoc, folder, "decode", message, proto, expected)
        ours = run_protoc(protoc, folder, "decode", message, proto, given)
        if ours != theirs:
            sys.exit(f"{out.name}: protoc reads the bytes encoded for {name} as\n"
                     f"{ours.decode()}\nnot\n{theirs.decode()}")
        checked += 1
    return checked


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--protoc", required=True)
    parser.add_argument("--check", action="store_true")
    options = parser.parse_args()
    version = subprocess.run([options.protoc, "--version"], stdout=subprocess.PIPE).stdout.decode()
    if version.strip() != "libprotoc 36.2":
        sys.exit(f"the golden bytes are protoc 36.2's, not {version.strip()}'s")
    for folder in sorted({path.parent for path in PROGRAMS.glob("*/*.txtpb")}):
        made = golden(options.protoc, folder)
        target = folder / "golden.fb"
        if options.check:
            if not target.exists() or target.read_text(encoding="utf-8") != made:
                sys.exit(f"{target} isn't what protoc makes; run the script without --check")
        elif not target.exists() or target.read_text(encoding="utf-8") != made:
            target.write_text(made, encoding="utf-8", newline="\n")
            print(f"wrote {target.relative_to(ROOT).as_posix()}")
        checked = check_encoded(options.protoc, folder)
        print(f"{folder.name}: {checked} encoded messages read back")
    return 0


if __name__ == "__main__":
    sys.exit(main())
