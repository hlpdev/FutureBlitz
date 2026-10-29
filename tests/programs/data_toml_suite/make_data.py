"""Writes data.fb, toml-test's cases for TOML 1.0 as String literals that data_toml_suite.fb reads,
since a program can't read a file until std.io's files: each valid case's document and what it
holds, written as data_toml_suite.fb's Canonical writes a Value, and each invalid case's
document. toml-test/ holds toml-test's 1.0.0 cases and its LICENSE, from
https://github.com/toml-lang/toml-test at the commit in toml-test/COMMIT. A case whose bytes
aren't UTF-8 is left out, since a String can't hold it.

A date, a time, or both reads as a String of its RFC 3339 text, with 'T' and 'Z' and its fraction
of a second without the 0s at its end, so what the case holds is written that way here.

    python make_data.py [--from <a clone of toml-test>]
"""

import argparse
import json
import re
import shutil
import struct
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
SUITE = HERE / "toml-test"


def quoted(text):
    """text in quotes, as Canonical writes a String."""
    out = ['"']
    for c in text:
        if c == '"':
            out.append('\\"')
        elif c == "\\":
            out.append("\\\\")
        elif ord(c) < 0x20 or ord(c) == 0x7F:
            out.append(f"\\u{ord(c):04x}")
        else:
            out.append(c)
    out.append('"')
    return "".join(out)


def moment(text):
    """A date or time's text as std.data's TOML reads it."""
    text = text.replace(" ", "T").replace("t", "T").replace("z", "Z")

    def trimmed(match):
        digits = match.group(1).rstrip("0")
        return "." + digits if digits else ""

    return re.sub(r"\.([0-9]+)", trimmed, text)


def canonical(node):
    """What a case holds, as Canonical writes the Value read from it."""
    if isinstance(node, list):
        return "[" + ", ".join(canonical(item) for item in node) + "]"

    if set(node) == {"type", "value"} and isinstance(node["value"], str):
        kind, value = node["type"], node["value"]
        if kind == "string":
            return quoted(value)
        if kind == "integer":
            return str(int(value))
        if kind == "float":
            if "nan" in value:
                return "nan"
            return str(struct.unpack("<Q", struct.pack("<d", float(value)))[0])
        if kind == "bool":
            return value
        return quoted(moment(value))

    parts = [quoted(key) + ": " + canonical(node[key]) for key in sorted(node)]
    return "{" + ", ".join(parts) + "}"


def literal(text):
    """text as a FutureBlitz String literal, in parts of a line each, joined by +."""
    parts = []
    for at in range(0, max(len(text), 1), 60):
        out = ['"']
        for c in text[at:at + 60]:
            code = ord(c)
            if c == "\\":
                out.append("\\\\")
            elif c == '"':
                out.append('\\"')
            elif c == "{":
                out.append("{{")
            elif c == "}":
                out.append("}}")
            elif c == "\n":
                out.append("\\n")
            elif c == "\t":
                out.append("\\t")
            elif c == "\r":
                out.append("\\r")
            elif code < 0x20 or 0x7F <= code <= 0x9F or code in (0xFEFF, 0x2028, 0x2029):
                out.append(f"\\u{{{code:x}}}")
            else:
                out.append(c)
        out.append('"')
        parts.append("".join(out))
    return " +\n         ".join(parts)


def copy_suite(clone):
    """Copies the 1.0.0 cases from a clone of toml-test into toml-test/."""
    clone = Path(clone)
    if SUITE.exists():
        shutil.rmtree(SUITE)
    names = (clone / "tests" / "files-toml-1.0.0").read_text(encoding="utf-8").split()
    for name in names:
        target = SUITE / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(clone / "tests" / name, target)
    shutil.copyfile(clone / "LICENSE", SUITE / "LICENSE")
    head = subprocess.run(["git", "-C", str(clone), "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()
    (SUITE / "COMMIT").write_text(head + "\n", encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--from", dest="clone")
    arguments = parser.parse_args()
    if arguments.clone:
        copy_suite(arguments.clone)

    valid, invalid, skipped = [], [], []
    for path in sorted(SUITE.rglob("*.toml")):
        name = path.relative_to(SUITE).with_suffix("").as_posix()
        try:
            text = path.read_bytes().decode("utf-8")
        except UnicodeDecodeError:
            skipped.append(name)
            continue
        if name.startswith("valid/"):
            expected = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
            valid.append((name[6:], text, canonical(expected)))
        else:
            invalid.append((name[8:], text))

    lines = [
        "// toml-test's cases for TOML 1.0, which make_data.py wrote from toml-test/: each valid",
        "// case's name, document, and what it holds, as data_toml_suite.fb's Canonical writes it,",
        "// and each invalid case's name and document. Left out, as a String can't hold their",
        "// bytes:",
    ]
    lines += [f"// - {name}" for name in skipped]
    lines += [
        "",
        "Import std.core",
        "",
        "Public Function ValidCases(): (String, String, String)[]",
        "    Return [",
    ]
    for at, (name, text, expected) in enumerate(valid):
        end = "" if at == len(valid) - 1 else ","
        lines.append(f"        ({literal(name)},")
        lines.append(f"         {literal(text)},")
        lines.append(f"         {literal(expected)}){end}")
    lines += ["    ]", "End Function", "", "Public Function InvalidCases(): (String, String)[]",
              "    Return ["]
    for at, (name, text) in enumerate(invalid):
        end = "" if at == len(invalid) - 1 else ","
        lines.append(f"        ({literal(name)},")
        lines.append(f"         {literal(text)}){end}")
    lines += ["    ]", "End Function", ""]
    (HERE / "data.fb").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"{len(valid)} valid, {len(invalid)} invalid, {len(skipped)} left out")


if __name__ == "__main__":
    main()
