"""Writes data.fb, yaml-test-suite's cases for YAML 1.2's core schema as String literals that
data_yaml_suite.fb reads, since a program can't read a file until std.io's files: each case's
name, its document, and what it holds, written as data_yaml_suite.fb's Canonical writes each
document's Value, or "error" for one that isn't YAML, or "" for one that reads but has no JSON
of what it holds. yaml-test-suite/ holds the suite's cases, from
https://github.com/yaml/yaml-test-suite's data-2022-01-17 release, and its License.

Left out are the cases whose bytes aren't UTF-8, those with a tag that isn't the core schema's,
and those whose mappings a Value can't hold: a key that's a mapping or a sequence, or the same
key twice.

    python make_data.py [--from <the release's folder>]
"""

import argparse
import json
import re
import shutil
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
SUITE = HERE / "yaml-test-suite"
CORE = {"tag:yaml.org,2002:" + name for name in ("str", "int", "float", "bool", "null", "seq", "map")}
CORE.add("!")


def quoted(text):
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


def canonical(node):
    if node is None:
        return "null"
    if node is True:
        return "true"
    if node is False:
        return "false"
    if isinstance(node, int):
        return str(node)
    if isinstance(node, float):
        if node != node:
            return "nan"
        return str(struct.unpack("<Q", struct.pack("<d", node))[0])
    if isinstance(node, str):
        return quoted(node)
    if isinstance(node, list):
        return "[" + ", ".join(canonical(item) for item in node) + "]"
    parts = [quoted(key) + ": " + canonical(node[key]) for key in sorted(node)]
    return "{" + ", ".join(parts) + "}"


def documents(text):
    """The JSON values text holds, one after another."""
    decoder = json.JSONDecoder()
    values, at = [], 0
    while True:
        while at < len(text) and text[at] in " \t\r\n":
            at += 1
        if at == len(text):
            return values
        value, at = decoder.raw_decode(text, at)
        values.append(value)


def complex_keys(events):
    """Whether a mapping in the events has a key that's a mapping or a sequence, or an alias of
    one."""
    stack, collections = [], set()
    for line in events.splitlines():
        kind = line[:4]
        if kind in ("+MAP", "+SEQ", "=VAL", "=ALI"):
            if stack and stack[-1][0] == "map":
                key = stack[-1][1]
                stack[-1][1] = not key
                alias = kind == "=ALI" and line.split()[1][1:] in collections
                if key and (kind in ("+MAP", "+SEQ") or alias):
                    return True
            anchor = re.search(r" &(\S+)", line)
            if anchor and kind in ("+MAP", "+SEQ"):
                collections.add(anchor.group(1))
            if kind == "+MAP":
                stack.append(["map", True])
            elif kind == "+SEQ":
                stack.append(["seq", False])
        elif kind in ("-MAP", "-SEQ"):
            stack.pop()
    return False


def literal(text):
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


def copy_suite(release):
    """Copies each case's in.yaml, in.json, error, and test.event, and the tags' lists."""
    release = Path(release)
    if SUITE.exists():
        shutil.rmtree(SUITE)
    for test in sorted(p for p in release.iterdir() if re.fullmatch(r"[A-Z0-9]{4}", p.name)):
        for file in test.rglob("*"):
            if file.is_file() and file.name in ("in.yaml", "in.json", "error", "test.event", "==="):
                target = SUITE / file.relative_to(release)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(file, target)
    lines = []
    for tag in sorted((release / "tags").iterdir()):
        lines.append(tag.name + ": " + " ".join(sorted(p.name for p in tag.iterdir())))
    (SUITE / "tags.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--from", dest="release")
    arguments = parser.parse_args()
    if arguments.release:
        copy_suite(arguments.release)

    tags = {}
    for line in (SUITE / "tags.txt").read_text(encoding="utf-8").splitlines():
        name, _, ids = line.partition(": ")
        for test in ids.split():
            tags.setdefault(test, set()).add(name)

    cases, skipped = [], []
    for test in sorted(p for p in SUITE.iterdir() if p.is_dir()):
        parts = [test] if (test / "in.yaml").exists() else sorted(p for p in test.iterdir() if p.is_dir())
        for case in parts:
            name = case.relative_to(SUITE).as_posix()
            events = (case / "test.event").read_text(encoding="utf-8")
            used = set(re.findall(r"<([^>]*)>", events))
            kinds = tags.get(test.name, set())
            try:
                text = (case / "in.yaml").read_bytes().decode("utf-8")
            except UnicodeDecodeError:
                skipped.append((name, "its bytes aren't UTF-8"))
                continue
            if (case / "error").exists():
                cases.append((name, text, "error"))
                continue
            if used - CORE:
                skipped.append((name, "a tag isn't the core schema's"))
                continue
            if "complex-key" in kinds or complex_keys(events):
                skipped.append((name, "a key is a mapping or a sequence"))
                continue
            if "duplicate-key" in kinds:
                skipped.append((name, "a mapping has a key twice"))
                continue
            if (case / "in.json").exists():
                values = documents((case / "in.json").read_text(encoding="utf-8"))
                cases.append((name, text, "\n---\n".join(canonical(value) for value in values)))
            else:
                cases.append((name, text, ""))

    lines = [
        "// yaml-test-suite's cases, which make_data.py wrote from yaml-test-suite/: each case's",
        "// name, document, and what it holds, as data_yaml_suite.fb's Canonical writes it, or",
        "// \"error\", or \"\" when the suite has no JSON of it. Left out:",
    ]
    lines += [f"// - {name}, as {why}" for name, why in skipped]
    lines += ["", "Import std.core", "", "Public Function Cases(): (String, String, String)[]",
              "    Return ["]
    for at, (name, text, expected) in enumerate(cases):
        end = "" if at == len(cases) - 1 else ","
        lines.append(f"        ({literal(name)},")
        lines.append(f"         {literal(text)},")
        lines.append(f"         {literal(expected)}){end}")
    lines += ["    ]", "End Function", ""]
    (HERE / "data.fb").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(len(cases), "cases,", len(skipped), "left out")


if __name__ == "__main__":
    main()
