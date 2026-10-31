// What the extension asks fb for: tasks' arguments, packages, debug builds' paths, and lldb's
// command.

import assert from "node:assert/strict";
import * as path from "node:path";
import { describe, test } from "node:test";

import {
    Owner,
    debugExecutable,
    findPackageRoot,
    lldbImportCommand,
    missingFbMessage,
    moduleFilter,
    packageName,
    projectOf,
    relativeTarget,
    taskArguments,
    taskEnvironment,
    taskLabel,
    testArguments,
    testOutcomes,
} from "../../fb";

describe("taskEnvironment", () => {
    test("UTF-16 columns, and with no folder open, whole paths", () => {
        assert.deepEqual(taskEnvironment(true), { FB_COLUMNS: "utf-16" });
        assert.deepEqual(taskEnvironment(false), { FB_COLUMNS: "utf-16", FB_PATHS: "absolute" });
    });
});

describe("taskArguments", () => {
    test("each command, with its own options", () => {
        assert.deepEqual(taskArguments({ type: "futureblitz", command: "build" }), ["build"]);
        assert.deepEqual(taskArguments({ type: "futureblitz", command: "build", target: "game", release: true }), [
            "build",
            "--release",
            "game",
        ]);
        assert.deepEqual(taskArguments({ type: "futureblitz", command: "test", filter: "Add" }), [
            "test",
            "--filter",
            "Add",
        ]);
        assert.deepEqual(taskArguments({ type: "futureblitz", command: "fmt", check: true, release: true }), [
            "fmt",
            "--check",
        ]);
    });

    test("run's program arguments go after --, and fb's own before the target", () => {
        assert.deepEqual(
            taskArguments({
                type: "futureblitz",
                command: "run",
                target: "main.fb",
                args: ["--color", "never"],
                programArgs: ["--level", "2"],
            }),
            ["run", "--color", "never", "main.fb", "--", "--level", "2"],
        );
        assert.deepEqual(taskArguments({ type: "futureblitz", command: "build", programArgs: ["x"] }), ["build"]);
    });

    test("labels", () => {
        assert.equal(taskLabel({ type: "futureblitz", command: "test" }), "test");
        assert.equal(taskLabel({ type: "futureblitz", command: "run", target: "${file}" }), "run current file");
        assert.equal(taskLabel({ type: "futureblitz", command: "build", target: "game", release: true }), "build game (release)");
    });
});

describe("packageName", () => {
    test("project.toml's [project] name, and no other section's", () => {
        const toml = [
            "# a comment",
            "[dependencies]",
            'name = "not this"',
            "",
            "[ project ]",
            'version = "0.1.0"',
            'name = "arena"   # the name',
        ].join("\r\n");
        assert.equal(packageName(toml), "arena");
        assert.equal(packageName("[project]\nname = 'single'\n"), "single");
        assert.equal(packageName('[dependencies]\nname = "x"\n'), undefined);
    });
});

describe("findPackageRoot", () => {
    const at = (...parts: string[]): string => path.resolve("/w", ...parts);
    const files = new Set([at("game", "project.toml"), at("project.toml")]);
    const exists = (file: string): boolean => files.has(file);

    test("the nearest project.toml up from the file", () => {
        assert.equal(findPackageRoot(at("game", "src", "ai.fb"), at(), exists), at("game"));
        assert.equal(findPackageRoot(at("tools", "x.fb"), at(), exists), at());
    });

    test("not past the workspace folder", () => {
        assert.equal(findPackageRoot(at("tools", "x.fb"), at("tools"), exists), undefined);
        assert.equal(findPackageRoot(path.resolve("/elsewhere", "x.fb"), at("tools"), exists), undefined);
    });
});

describe("debugExecutable", () => {
    const game = (root: string, main = "main.fb"): Owner => ({ root, project: { name: "game", type: "executable", main } });

    test("a package's in its build/, a file outside any package beside it", () => {
        assert.equal(debugExecutable("C:\\w\\game", game("C:\\w\\game"), "win32"), "C:\\w\\game\\build\\game.exe");
        assert.equal(debugExecutable("/w/game", game("/w/game"), "linux"), "/w/game/build/game");
        assert.equal(debugExecutable("C:\\w\\tools\\hello.fb", undefined, "win32"), "C:\\w\\tools\\hello.exe");
        assert.equal(debugExecutable("/w/tools/hello.fb", undefined, "darwin"), "/w/tools/hello");
    });

    // As fb builds them without -o: the main file is the package's program, and another file's
    // program is named for the file, both in build/.
    test("a package's file in its build/", () => {
        assert.equal(debugExecutable("/w/game/main.fb", game("/w/game"), "linux"), "/w/game/build/game");
        assert.equal(debugExecutable("/w/game/src/tool.fb", game("/w/game"), "linux"), "/w/game/build/tool");
        assert.equal(
            debugExecutable("C:\\w\\game\\src\\play.fb", game("C:\\w\\game", "src/play.fb"), "win32"),
            "C:\\w\\game\\build\\game.exe",
        );
        assert.equal(debugExecutable("/w/game/main.fb", game("/w/game", "src/play.fb"), "linux"), "/w/game/build/main");
    });
});

describe("projectOf", () => {
    test("the name, the type, and the main file, with fb's defaults", () => {
        assert.deepEqual(projectOf('[project]\nname = "game"\n'), { name: "game", type: "executable", main: "main.fb" });
        assert.deepEqual(projectOf('[project]\nname = "lib"\ntype = "library"\nmain = "./src/lib.fb"\n'), {
            name: "lib",
            type: "library",
            main: "src/lib.fb",
        });
    });
});

describe("missingFbMessage", () => {
    test("names a setting that names nothing", () => {
        assert.match(missingFbMessage("C:\\tools\\fb"), /futureblitz\.path is C:\\tools\\fb, which isn't there/);
        assert.match(missingFbMessage(undefined), /set futureblitz\.path to it, put it on PATH/);
    });
});

test("lldbImportCommand quotes the path with forward slashes", () => {
    assert.equal(
        lldbImportCommand("C:\\Users\\ann\\.vscode\\futureblitz.py"),
        'command script import "C:/Users/ann/.vscode/futureblitz.py"',
    );
    assert.equal(lldbImportCommand('/odd "name"/futureblitz.py'), 'command script import "/odd \\"name\\"/futureblitz.py"');
});

test("relativeTarget", () => {
    assert.equal(relativeTarget(path.join("/w", "game"), "/w"), "game");
    assert.equal(relativeTarget(path.resolve("/w"), "/w"), ".");
    assert.equal(relativeTarget(path.resolve("/other", "x.fb"), "/w"), path.resolve("/other", "x.fb"));
});

describe("testOutcomes", () => {
    test("fb test's line for each test, and what each failure said", () => {
        const output = [
            "   Compiling counted v0.1.0 (C:/counted)",
            "     Running 4 tests",
            "test counted.counting.Doubles ... ok",
            "test counted.counting.Breaks ... FAILED",
            "test counted.counting.Later ... skipped: not yet",
            "test counted.trouble.Panics ... FAILED",
            "",
            "failures:",
            "",
            "---- the tests' program ----",
            "leaked 1 object",
            "",
            "---- counted.counting.Breaks ----",
            "expect failed: Twice(2) == 5 at counting.fb:9:5",
            "  left: 4",
            "  right: 5",
            "",
            "---- counted.trouble.Panics ----",
            "panic: index 5 is out of range for 2 elements at trouble.fb:3:11",
            "",
            "test result: FAILED. 1 passed; 2 failed; 1 skipped; 0 filtered out; finished in 0.01s",
        ].join("\r\n");
        assert.deepEqual(
            testOutcomes(output),
            new Map([
                ["counted.counting.Doubles", { state: "passed", messages: [] }],
                [
                    "counted.counting.Breaks",
                    {
                        state: "failed",
                        messages: ["expect failed: Twice(2) == 5 at counting.fb:9:5", "  left: 4", "  right: 5"],
                    },
                ],
                ["counted.counting.Later", { state: "skipped", messages: ["not yet"] }],
                [
                    "counted.trouble.Panics",
                    { state: "failed", messages: ["panic: index 5 is out of range for 2 elements at trouble.fb:3:11"] },
                ],
            ]),
        );
    });

    test("a test executable's own run, whose failures follow their tests' lines", () => {
        const output = [
            "test counted.counting.Breaks ... FAILED",
            "    expect failed: Twice(2) == 5 at counting.fb:9:5",
            "test counted.counting.Doubles ... ok",
            "1 passed, 1 failed, 0 skipped",
        ].join("\n");
        assert.deepEqual(
            testOutcomes(output),
            new Map([
                ["counted.counting.Breaks", { state: "failed", messages: ["expect failed: Twice(2) == 5 at counting.fb:9:5"] }],
                ["counted.counting.Doubles", { state: "passed", messages: [] }],
            ]),
        );
    });
});

test("testArguments runs a filter's tests, or builds their executable", () => {
    assert.deepEqual(testArguments("C:/counted", undefined), ["test", "--color", "never", "C:/counted"]);
    assert.deepEqual(testArguments("C:/counted", "counted.counting.Doubles", "C:/t/tests.exe"), [
        "test",
        "--color",
        "never",
        "--filter",
        "counted.counting.Doubles",
        "-o",
        "C:/t/tests.exe",
        "C:/counted",
    ]);
});

test("moduleFilter picks a file's tests by their module", () => {
    assert.equal(moduleFilter("game.shapes.Area"), "game.shapes.");
    assert.equal(moduleFilter("Area"), "Area");
});
