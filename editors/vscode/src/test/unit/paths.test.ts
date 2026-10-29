// Finding fb, the language server, and the lldb script, over a made-up file system.

import assert from "node:assert/strict";
import { describe, test } from "node:test";

import { Environment, executableName, expandPath, findFb, findLldbScript, findLsp, findOnPath } from "../../paths";

function windows(files: string[], path = "C:\\tools;C:\\fb\\bin"): Environment {
    const present = new Set(files.map((file) => file.toLowerCase()));
    return {
        platform: "win32",
        env: { Path: path, PATHEXT: ".COM;.EXE;.BAT" },
        home: "C:\\Users\\ann",
        exists: (file) => present.has(file.toLowerCase()),
    };
}

function linux(files: string[], path = "/usr/bin:/opt/fb/bin"): Environment {
    const present = new Set(files);
    return { platform: "linux", env: { PATH: path }, home: "/home/ann", exists: (file) => present.has(file) };
}

describe("expandPath", () => {
    test("expands the workspace folder, the home folder, and ~", () => {
        const environment = linux([]);
        assert.equal(expandPath("${workspaceFolder}/build/cli/fb", "/work/game", environment), "/work/game/build/cli/fb");
        assert.equal(expandPath("~/fb/bin/fb", undefined, environment), "/home/ann/fb/bin/fb");
        assert.equal(expandPath("${userHome}/x", undefined, environment), "/home/ann/x");
        assert.equal(expandPath("  ", undefined, environment), "");
    });

    test("on Windows, with either slash after ~", () => {
        const environment = windows([]);
        assert.equal(expandPath("~\\fb\\fb.exe", undefined, environment), "C:\\Users\\ann\\fb\\fb.exe");
        assert.equal(expandPath("~/fb/fb.exe", undefined, environment), "C:\\Users\\ann\\fb\\fb.exe");
    });
});

describe("findOnPath", () => {
    test("Windows tries PATHEXT's extensions in each folder, whatever Path's case", () => {
        const found = findOnPath("fb", windows(["C:\\fb\\bin\\fb.exe"]));
        assert.equal(found.path, "C:\\fb\\bin\\fb.exe");
        assert.deepEqual(found.tried, [
            "C:\\tools\\fb.com",
            "C:\\tools\\fb.exe",
            "C:\\tools\\fb.bat",
            "C:\\fb\\bin\\fb.com",
            "C:\\fb\\bin\\fb.exe",
        ]);
    });

    test("a quoted folder on Windows' Path", () => {
        const found = findOnPath("fb", windows(["C:\\Program Files\\fb\\fb.exe"], '"C:\\Program Files\\fb"'));
        assert.equal(found.path, "C:\\Program Files\\fb\\fb.exe");
    });

    test("elsewhere the name as it is", () => {
        assert.equal(findOnPath("fb", linux(["/opt/fb/bin/fb"])).path, "/opt/fb/bin/fb");
        assert.equal(findOnPath("fb", linux([])).path, undefined);
    });
});

describe("findFb", () => {
    test("the setting first, even when fb is on PATH", () => {
        const environment = linux(["/work/build/cli/fb", "/usr/bin/fb"]);
        assert.equal(findFb("${workspaceFolder}/build/cli/fb", "/work", environment).path, "/work/build/cli/fb");
    });

    test("a setting that names nothing says so, and doesn't fall back", () => {
        const found = findFb("/nowhere/fb", undefined, linux(["/usr/bin/fb"]));
        assert.equal(found.path, undefined);
        assert.equal(found.missingSetting, "/nowhere/fb");
    });

    test("then PATH, then ~/.futureblitz/bin", () => {
        assert.equal(findFb("", undefined, linux(["/usr/bin/fb"])).path, "/usr/bin/fb");
        assert.equal(findFb("", undefined, linux(["/home/ann/.futureblitz/bin/fb"])).path, "/home/ann/.futureblitz/bin/fb");
        assert.equal(
            findFb("", undefined, windows(["C:\\Users\\ann\\.futureblitz\\bin\\fb.exe"])).path,
            "C:\\Users\\ann\\.futureblitz\\bin\\fb.exe",
        );
        const missing = findFb("", undefined, linux([]));
        assert.equal(missing.path, undefined);
        assert.equal(missing.missingSetting, undefined);
        assert.equal(missing.tried.at(-1), "/home/ann/.futureblitz/bin/fb");
    });
});

describe("findLsp", () => {
    test("beside fb, as a release has it", () => {
        const environment = linux(["/opt/fb/bin/fb", "/opt/fb/bin/fb_lsp", "/usr/bin/fb_lsp"]);
        assert.equal(findLsp("", "/opt/fb/bin/fb", undefined, environment).path, "/opt/fb/bin/fb_lsp");
    });

    test("in a CMake build folder's lsp/, beside cli/", () => {
        const environment = windows(["C:\\fb\\build\\cli\\fb.exe", "C:\\fb\\build\\lsp\\fb_lsp.exe"]);
        assert.equal(
            findLsp("", "C:\\fb\\build\\cli\\fb.exe", undefined, environment).path,
            "C:\\fb\\build\\lsp\\fb_lsp.exe",
        );
    });

    test("then PATH, and the setting before all of them", () => {
        assert.equal(findLsp("", undefined, undefined, linux(["/usr/bin/fb_lsp"])).path, "/usr/bin/fb_lsp");
        const environment = linux(["/opt/fb/bin/fb_lsp", "/srv/lsp"]);
        assert.equal(findLsp("/srv/lsp", "/opt/fb/bin/fb", undefined, environment).path, "/srv/lsp");
    });

    test("missing, it lists where it looked", () => {
        const found = findLsp("", "/opt/fb/bin/fb", undefined, linux([]));
        assert.equal(found.path, undefined);
        assert.deepEqual(found.tried.slice(0, 2), ["/opt/fb/bin/fb_lsp", "/opt/fb/lsp/fb_lsp"]);
        assert.ok(found.tried.includes("/usr/bin/fb_lsp"));
    });
});

describe("findLldbScript", () => {
    test("a release's lldb/ beside bin/, the source tree's for a build folder in it, else the bundled one", () => {
        const bundled = "/ext/lldb/futureblitz.py";
        assert.equal(
            findLldbScript("", "/opt/fb/bin/fb", bundled, undefined, linux(["/opt/fb/lldb/futureblitz.py", bundled])).path,
            "/opt/fb/lldb/futureblitz.py",
        );
        assert.equal(
            findLldbScript("", "/src/fb/build/cli/fb", bundled, undefined, linux(["/src/fb/tools/lldb/futureblitz.py", bundled]))
                .path,
            "/src/fb/tools/lldb/futureblitz.py",
        );
        assert.equal(findLldbScript("", "/opt/fb/bin/fb", bundled, undefined, linux([bundled])).path, bundled);
        assert.equal(findLldbScript("", undefined, bundled, undefined, linux([bundled])).path, bundled);
    });
});

test("executableName", () => {
    assert.equal(executableName("fb", "win32"), "fb.exe");
    assert.equal(executableName("fb.exe", "win32"), "fb.exe");
    assert.equal(executableName("fb", "darwin"), "fb");
});
