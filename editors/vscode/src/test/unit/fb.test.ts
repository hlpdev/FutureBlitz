// What the extension asks fb for: tasks' arguments, packages, debug builds' paths, and lldb's
// command.

import assert from "node:assert/strict";
import * as path from "node:path";
import { describe, test } from "node:test";

import {
    debugExecutable,
    findPackageRoot,
    lldbImportCommand,
    packageName,
    relativeTarget,
    taskArguments,
    taskLabel,
} from "../../fb";

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
    test("a package's in its build/, a file's beside it", () => {
        assert.equal(debugExecutable("C:\\w\\game", "game", "win32"), "C:\\w\\game\\build\\game.exe");
        assert.equal(debugExecutable("/w/game", "game", "linux"), "/w/game/build/game");
        assert.equal(debugExecutable("C:\\w\\tools\\hello.fb", undefined, "win32"), "C:\\w\\tools\\hello.exe");
        assert.equal(debugExecutable("/w/tools/hello.fb", undefined, "darwin"), "/w/tools/hello");
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
