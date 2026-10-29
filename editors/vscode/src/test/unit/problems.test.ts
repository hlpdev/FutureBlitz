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

    const fb = testFb();
    const skip = fb === undefined ? "no fb: set FUTUREBLITZ_FB" : false;

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
