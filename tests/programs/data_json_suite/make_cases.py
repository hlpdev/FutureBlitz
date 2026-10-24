"""Writes cases.fb from JSONTestSuite's parsing cases in test_parsing/: each file's name and its
bytes in hex, which data_json_suite.fb reads, since std reads no files until step 14. The cases are
github.com/nst/JSONTestSuite's test_parsing/ at 1ef36fa01286573e846ac449e8683f8833c5b26a, under
its MIT license (LICENSE): y_ ones are JSON, n_ ones aren't, and i_ ones are either.

    python make_cases.py
"""

from pathlib import Path

HERE = Path(__file__).resolve().parent

lines = [
    "// JSONTestSuite's parsing cases (github.com/nst/JSONTestSuite at 1ef36fa, under its MIT",
    "// license, in LICENSE), each its file's name and its bytes in hex, which make_cases.py wrote",
    "// from test_parsing/.",
    "",
    "Public Function Cases(): (String, String)[]",
    "    Return [",
]
for path in sorted((HERE / "test_parsing").iterdir()):
    lines.append(f'        ("{path.name}", "{path.read_bytes().hex()}"),')
lines += ["    ]", "End Function", ""]
(HERE / "cases.fb").write_text("\n".join(lines), encoding="utf-8", newline="\n")
