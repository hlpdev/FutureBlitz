// package.json's problem matchers against what fb prints: compiler/src/diagnostic_render.cpp's
// "path:line:col: severity[code]: message", with its snippet and notes after it, and a running
// program's panics, uncaught errors, and failed Expects.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { describe, test } from "node:test";

import { COLUMNS, PROBLEM_MATCHERS, WHOLE_PATH_MATCHERS, taskEnvironment } from "../../fb";
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
    owner?: string;
    severity?: string;
    pattern?: Pattern;
    base?: string;
    fileLocation?: string | string[];
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
const declared: Matcher[] = manifest.contributes.problemMatchers;
// The ones with patterns of their own; the others are the same with whole paths.
const matchers = declared.filter((matcher) => matcher.pattern !== undefined);

// What VS Code's matchers make of fb's output, a line at a time, as it reads a task's.
function problems(output: string): Problem[] {
    const found: Problem[] = [];
    for (const line of output.split(/\r?\n/)) {
        for (const matcher of matchers) {
            const match = new RegExp((matcher.pattern as Pattern).regexp).exec(line);
            if (match === null) {
                continue;
            }

            const pattern = matcher.pattern as Pattern;
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
        const names = declared.map((matcher) => `$${matcher.name}`);
        assert.deepEqual(names, [...PROBLEM_MATCHERS, ...WHOLE_PATH_MATCHERS]);
        // Their owner isn't the language client's diagnostic collection's, "futureblitz": VS Code
        // keeps markers whose owner an extension's collection has from the extension host, and
        // the collection's changes would replace or clear them.
        for (const matcher of matchers) {
            assert.equal(matcher.owner, "futureblitz-tasks", matcher.name);
        }

        // A whole path's is its twin's, with no folder to find a file from.
        for (const [i, name] of WHOLE_PATH_MATCHERS.entries()) {
            const whole = declared.find((matcher) => `$${matcher.name}` === name);
            assert.equal(whole?.base, PROBLEM_MATCHERS[i]);
            assert.equal(whole?.fileLocation, "absolute");
        }
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

    // VS Code's columns are UTF-16 code units, which the tasks ask fb for: after two U+1F600s, y is
    // at code point 18 but unit 20.
    test("fb build's columns in UTF-16 units, as the tasks ask", { skip }, () => {
        const folder = scratch("columns");
        fs.writeFileSync(path.join(folder, "emoji.fb"), 'Import std.core\nLocal s = "\u{1F600}\u{1F600}" + y\n');
        const build = (env: NodeJS.ProcessEnv): Problem[] =>
            problems(
                (({ stdout, stderr }) => stdout + stderr)(
                    spawnSync(fb as string, ["build", "--color", "never", "emoji.fb"], {
                        cwd: folder,
                        encoding: "utf8",
                        env,
                    }),
                ),
            );
        const points = build({ ...process.env, [COLUMNS.name]: undefined });
        const units = build({ ...process.env, [COLUMNS.name]: COLUMNS.value });
        fs.rmSync(folder, { recursive: true, force: true });
        assert.deepEqual(points.map((p) => [p.line, p.column]), [[2, 18]]);
        assert.deepEqual(units.map((p) => [p.line, p.column]), [[2, 20]]);
        assert.equal("Local s = \"\u{1F600}\u{1F600}\" + y".indexOf("y") + 1, units[0].column);
    });

    // With no folder open, VS Code finds a problem's relative path from its own process's folder,
    // not the task's, so a task there asks fb for whole paths, a program's places among them.
    test("fb build's and fb run's paths with no folder open are whole", { skip }, () => {
        const folder = scratch("lone");
        fs.writeFileSync(path.join(folder, "bad.fb"), 'Import std.core\nLocal x: Int = "a"\n');
        fs.writeFileSync(path.join(folder, "panic.fb"), "Import std.core\nLocal a = [1]\nPrint(a[3])\n");
        const run = (args: string[], inFolder: boolean): Problem[] =>
            problems(
                (({ stdout, stderr }) => stdout + stderr)(
                    spawnSync(fb as string, [...args, "--color", "never"], {
                        cwd: folder,
                        encoding: "utf8",
                        env: { ...process.env, ...taskEnvironment(inFolder) },
                    }),
                ),
            );
        try {
            assert.deepEqual(run(["build", "bad.fb"], true).map((p) => p.file), ["bad.fb"]);
            for (const [args, file] of [
                [["build", "bad.fb"], "bad.fb"],
                [["run", "panic.fb"], "panic.fb"],
            ] as const) {
                const found = run([...args], false);
                assert.equal(found.length, 1, args.join(" "));
                assert.ok(path.isAbsolute(found[0].file), found[0].file);
                assert.equal(fs.realpathSync(found[0].file), fs.realpathSync(path.join(folder, file)));
            }
        } finally {
            fs.rmSync(folder, { recursive: true, force: true });
        }
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
