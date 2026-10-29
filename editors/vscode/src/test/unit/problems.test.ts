// package.json's problem matchers against what fb prints: compiler/src/diagnostic_render.cpp's
// "path:line:col: severity[code]: message", with its snippet and notes after it, and a running
// program's panics, uncaught errors, and failed Expects.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { describe, test } from "node:test";

import { EXTENSION, scratch, testFb } from "./repo";

interface Pattern {
    regexp: string;
    file: number;
    line: number;
    column: number;
    severity?: number;
    code?: number;
    message: number;
}

interface Matcher {
    name: string;
    severity?: string;
    pattern: Pattern;
}

interface Problem {
    matcher: string;
    file: string;
    line: number;
    column: number;
    severity: string;
    code?: string;
    message: string;
}

const manifest = JSON.parse(fs.readFileSync(path.join(EXTENSION, "package.json"), "utf8"));
const matchers: Matcher[] = manifest.contributes.problemMatchers;

// What VS Code's matchers make of fb's output, a line at a time, as it reads a task's.
function problems(output: string): Problem[] {
    const found: Problem[] = [];
    for (const line of output.split(/\r?\n/)) {
        for (const matcher of matchers) {
            const match = new RegExp(matcher.pattern.regexp).exec(line);
            if (match === null) {
                continue;
            }

            const pattern = matcher.pattern;
            found.push({
                matcher: matcher.name,
                file: match[pattern.file],
                line: Number(match[pattern.line]),
                column: Number(match[pattern.column]),
                severity: pattern.severity === undefined ? (matcher.severity ?? "error") : match[pattern.severity],
                ...(pattern.code !== undefined && match[pattern.code] !== undefined ? { code: match[pattern.code] } : {}),
                message: match[pattern.message],
            });
        }
    }

    return found;
}

describe("problem matchers", () => {
    test("each task names matchers package.json has", () => {
        const names = matchers.map((matcher) => `$${matcher.name}`);
        assert.deepEqual(names, ["$fb", "$fb-note", "$fb-runtime"]);
    });

    test("an error, a warning with a code, and a note, but not their snippets", () => {
        const output = [
            "   Compiling game v0.1.0 (C:/work/game)",
            "src/ai.fb:2:16: error: expected Int32, found String",
            "  |",
            '2 | Local x: Int = "a"',
            "  |                ^^^",
            "C:/work/game/main.fb:10:1: warning[unused]: 'y' isn't used",
            "C:/work/game/main.fb:4:5: note: declared here",
            "  = note: remove it, or use it",
            "error: couldn't build game v0.1.0 (C:/work/game) because of 1 error",
        ].join("\n");
        assert.deepEqual(problems(output), [
            {
                matcher: "fb",
                file: "src/ai.fb",
                line: 2,
                column: 16,
                severity: "error",
                message: "expected Int32, found String",
            },
            {
                matcher: "fb",
                file: "C:/work/game/main.fb",
                line: 10,
                column: 1,
                severity: "warning",
                code: "unused",
                message: "'y' isn't used",
            },
            { matcher: "fb-note", file: "C:/work/game/main.fb", line: 4, column: 5, severity: "info", message: "declared here" },
        ]);
    });

    test("a Windows path with a drive and backslashes", () => {
        const [problem] = problems("C:\\work\\game\\main.fb:3:7: error: 'y' isn't declared");
        assert.equal(problem.file, "C:\\work\\game\\main.fb");
        assert.equal(problem.line, 3);
        assert.equal(problem.column, 7);
    });

    test("a panic, an uncaught error, and a failed Expect, at their places", () => {
        const output = [
            "panic: index 3 is out of range for 1 element at panic.fb:3:7",
            "    at top-level code (panic.fb:3)",
            "uncaught std.io.IOError: no file at a.txt at io/load.fb:12:9",
            "expect failed: Add(1, 2) == 4 at main.fb:6:5",
            "  left: 3",
        ].join("\n");
        assert.deepEqual(
            problems(output).map((p) => [p.matcher, p.file, p.line, p.column, p.message]),
            [
                ["fb-runtime", "panic.fb", 3, 7, "panic: index 3 is out of range for 1 element"],
                ["fb-runtime", "io/load.fb", 12, 9, "uncaught std.io.IOError: no file at a.txt"],
                ["fb-runtime", "main.fb", 6, 5, "expect failed: Add(1, 2) == 4"],
            ],
        );
    });

    // runtime.cpp writes an uncaught error's ": message" only when it has one, and runtime/test.cpp
    // and lower.cpp write a test's Fail and a thrown error as "failed[: message] at" and
    // "threw Class[: message] at".
    test("an uncaught error with no message, and tests that fail or throw", () => {
        const output = [
            "uncaught Oops at C:/work/u.fb:6:1",
            "failed: by hand at t/src/math.fb:10:5",
            "failed at t/src/math.fb:12:5",
            "threw Oops at throws.fb:8:5",
            "threw std.io.IOError: no file at throws.fb:9:5",
        ].join("\n");
        assert.deepEqual(
            problems(output).map((p) => [p.file, p.line, p.column, p.message]),
            [
                ["C:/work/u.fb", 6, 1, "uncaught Oops"],
                ["t/src/math.fb", 10, 5, "failed: by hand"],
                ["t/src/math.fb", 12, 5, "failed"],
                ["throws.fb", 8, 5, "threw Oops"],
                ["throws.fb", 9, 5, "threw std.io.IOError: no file"],
            ],
        );
    });

    const fb = testFb();
    const skip = fb === undefined ? "no fb: set FUTUREBLITZ_FB" : false;

    test("fb run's uncaught error with no message", { skip }, () => {
        const folder = scratch("uncaught");
        fs.writeFileSync(path.join(folder, "uncaught.fb"), "Import std.core\n\nClass Oops Extends Error\nEnd Class\n\nThrow Oops()\n");
        const result = spawnSync(fb as string, ["run", "--color", "never", "uncaught.fb"], { cwd: folder, encoding: "utf8" });
        fs.rmSync(folder, { recursive: true, force: true });
        assert.equal(result.status, 1);
        assert.deepEqual(
            problems(result.stdout + result.stderr).map((p) => [p.matcher, p.file, p.line, p.message]),
            [["fb-runtime", "uncaught.fb", 6, "uncaught Oops"]],
        );
    });

    test("fb test's failed Expect, Fail, and thrown error", { skip }, () => {
        const folder = scratch("tests");
        fs.writeFileSync(
            path.join(folder, "throws.fb"),
            [
                "Import std.core",
                "Import std.test",
                "",
                "Class Oops Extends Error",
                "End Class",
                "",
                "Function Boom() Throws Oops",
                "    Throw Oops()",
                "End Function",
                "",
                "@Test",
                "Function LetsItOut() Throws Oops",
                "    Try Boom()",
                "End Function",
                "",
                "@Test",
                "Function FailsByHand()",
                '    Fail("by hand")',
                "End Function",
                "",
                "@Test",
                "Function Expects()",
                "    Expect(1 == 2)",
                "End Function",
                "",
            ].join("\n"),
        );
        const result = spawnSync(fb as string, ["test", "--color", "never", "throws.fb"], { cwd: folder, encoding: "utf8" });
        fs.rmSync(folder, { recursive: true, force: true });
        assert.equal(result.status, 1);
        assert.deepEqual(
            problems(result.stdout + result.stderr)
                .map((p) => [p.file, p.line, p.message])
                .sort((a, b) => Number(a[1]) - Number(b[1])),
            [
                ["throws.fb", 8, "threw Oops"],
                ["throws.fb", 18, "failed: by hand"],
                ["throws.fb", 23, "expect failed: 1 == 2"],
            ],
        );
    });

    // fb's own output, colored or not: VS Code takes the color codes out before matching.
    test("fb build's errors", { skip }, () => {
        const folder = scratch("problems");
        fs.writeFileSync(path.join(folder, "bad.fb"), 'Import std.core\nLocal x: Int = "a"\nPrint(y)\n');
        const result = spawnSync(fb as string, ["build", "--color", "never", "bad.fb"], { cwd: folder, encoding: "utf8" });
        fs.rmSync(folder, { recursive: true, force: true });
        assert.equal(result.status, 1);
        assert.deepEqual(
            problems(result.stdout + result.stderr).map((p) => [p.file, p.line, p.column, p.severity]),
            [
                ["bad.fb", 2, 16, "error"],
                ["bad.fb", 3, 7, "error"],
            ],
        );
    });

    test("fb run's panic", { skip }, () => {
        const folder = scratch("panic");
        fs.writeFileSync(path.join(folder, "panic.fb"), "Import std.core\nLocal a = [1]\nPrint(a[3])\n");
        const result = spawnSync(fb as string, ["run", "--color", "never", "panic.fb"], { cwd: folder, encoding: "utf8" });
        fs.rmSync(folder, { recursive: true, force: true });
        assert.equal(result.status, 101);
        assert.deepEqual(
            problems(result.stdout + result.stderr).map((p) => [p.matcher, p.file, p.line, p.column]),
            [["fb-runtime", "panic.fb", 3, 7]],
        );
    });
});
