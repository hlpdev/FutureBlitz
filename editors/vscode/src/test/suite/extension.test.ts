// The extension in VS Code, on a copy of test/fixtures/workspace: app/, a package, and files/, .fb
// files of their own. The tests that build need fb, from FUTUREBLITZ_FB; they're skipped without it.

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import type { Api } from "../../extension";
import { runTask } from "../../tasks";

const fb = process.env.FUTUREBLITZ_FB ?? "";
const node = process.env.FB_TEST_NODE ?? "";
const fakeServer = process.env.FB_TEST_FAKE_LSP ?? "";

function folder(name: string): vscode.WorkspaceFolder {
    const found = vscode.workspace.workspaceFolders?.find((f) => f.name === name);
    assert.ok(found !== undefined, `no workspace folder ${name}`);
    return found;
}

function file(folderName: string, name: string): vscode.Uri {
    return vscode.Uri.joinPath(folder(folderName).uri, name);
}

// Whether `task` is for the workspace folder `name`.
function inFolder(task: vscode.Task, name: string): boolean {
    return typeof task.scope === "object" && task.scope.name === name;
}

async function waitFor<T>(what: string, get: () => T | undefined | false, timeout = 20000): Promise<T> {
    const end = Date.now() + timeout;
    while (Date.now() < end) {
        const value = get();
        if (value !== undefined && value !== false) {
            return value;
        }

        await new Promise((resolve) => setTimeout(resolve, 100));
    }

    throw new Error(`timed out waiting for ${what}`);
}

async function open(uri: vscode.Uri): Promise<vscode.TextEditor> {
    const document = await vscode.workspace.openTextDocument(uri);
    return vscode.window.showTextDocument(document);
}

async function setting(name: string, value: unknown): Promise<void> {
    await vscode.workspace.getConfiguration("futureblitz").update(name, value, vscode.ConfigurationTarget.Workspace);
}

let api: Api;

suiteSetup(async () => {
    await open(file("files", "hello.fb"));
    const extension = vscode.extensions.getExtension<Api>("futureblitz.futureblitz");
    assert.ok(extension !== undefined);
    api = await extension.activate();
    if (fb !== "") {
        await setting("path", fb);
    }
});

suite("the language", () => {
    test(".fb files are FutureBlitz", async () => {
        const editor = await open(file("files", "hello.fb"));
        assert.equal(editor.document.languageId, "futureblitz");
    });

    test("the language configuration comments a line with //", async () => {
        const editor = await open(file("files", "format.fb"));
        editor.selection = new vscode.Selection(2, 0, 2, 0);
        await vscode.commands.executeCommand("editor.action.commentLine");
        assert.equal(editor.document.lineAt(2).text, '// Print("format me")');
        await vscode.commands.executeCommand("undo");
        assert.equal(editor.document.lineAt(2).text, 'Print("format me")');
        await vscode.commands.executeCommand("workbench.action.files.revert");
    });
});

suite("the language server", () => {
    teardown(async () => {
        await api.server.stop();
    });

    test("turned off, it says so", async () => {
        await api.server.start();
        assert.equal(api.server.state, "disabled");
    });

    test("missing, it says where it looked, and the rest goes on", async () => {
        await setting("lsp.enabled", true);
        await setting("lspPath", path.join(folder("files").uri.fsPath, "no_such_server"));
        try {
            await api.server.start();
            assert.equal(api.server.state, "missing");
            assert.match(api.server.detail, /no_such_server/);
            const tasks = await vscode.tasks.fetchTasks({ type: "futureblitz" });
            assert.ok(tasks.length > 0);
        } finally {
            await setting("lspPath", undefined);
            await setting("lsp.enabled", false);
        }
    });

    test("one that stops as it starts fails, and the rest goes on", async () => {
        await api.server.start({ command: node, args: [fakeServer, "exit"] });
        assert.equal(api.server.state, "failed");
        assert.equal(api.server.client, undefined);
        const tasks = await vscode.tasks.fetchTasks({ type: "futureblitz" });
        assert.ok(tasks.length > 0);
    });

    test("one that keeps stopping is started again, three times, then left", async () => {
        await api.server.start({ command: node, args: [fakeServer, "crash"] });
        assert.equal(api.server.state, "running");
        await waitFor("the server to be left stopped", () => api.server.state === "failed", 30000);
        assert.match(api.server.detail, /stopped 4 times in 3 minutes/);
    });

    test("format on save goes through it", async () => {
        await api.server.start({ command: node, args: [fakeServer] });
        assert.equal(api.server.state, "running");
        const editor = await open(file("files", "format.fb"));
        await editor.edit((edit) => edit.insert(new vscode.Position(3, 0), "\n"));
        await editor.document.save();
        assert.equal(editor.document.lineAt(0).text, "// formatted");
        assert.ok(!editor.document.isDirty);
    });
});

suite("tasks", () => {
    test("a package's commands, and the open file's", async () => {
        const tasks = await vscode.tasks.fetchTasks({ type: "futureblitz" });
        const names = (name: string): string[] =>
            tasks.filter((task) => inFolder(task, name)).map((task) => task.name);
        assert.deepEqual(names("app"), [
            "build",
            "run",
            "test",
            "fmt",
            "check",
            "build current file",
            "run current file",
            "fmt current file",
            "check current file",
        ]);
        assert.deepEqual(names("files"), [
            "build current file",
            "run current file",
            "fmt current file",
            "check current file",
        ]);
        const build = tasks.find((task) => inFolder(task, "app") && task.name === "build");
        assert.equal(build?.group?.id, vscode.TaskGroup.Build.id);
        assert.equal(tasks.find((task) => task.name === "test")?.group?.id, vscode.TaskGroup.Test.id);
    });

    test("without fb, a task says how to get it", async () => {
        await setting("path", path.join(folder("files").uri.fsPath, "no_such_fb"));
        try {
            const task = api.tasks.make({ type: "futureblitz", command: "build" }, folder("app"));
            assert.ok(task.execution instanceof vscode.CustomExecution);
            assert.equal(await runTask(task), 1);
        } finally {
            await setting("path", fb === "" ? undefined : fb);
        }
    });

    suite("with fb", function () {
        suiteSetup(function () {
            if (fb === "") {
                this.skip();
            }
        });

        test("a build's errors go to Problems, with color on or off", async () => {
            for (const color of ["never", "always"]) {
                const broken = file("files", "broken.fb");
                const task = api.tasks.make(
                    { type: "futureblitz", command: "build", target: "broken.fb", args: ["--color", color] },
                    folder("files"),
                );
                assert.equal(await runTask(task), 1);
                const [diagnostic] = await waitFor(
                    "the build's error",
                    () => vscode.languages.getDiagnostics(broken).length > 0 && vscode.languages.getDiagnostics(broken),
                );
                assert.equal(diagnostic.severity, vscode.DiagnosticSeverity.Error);
                assert.equal(diagnostic.range.start.line, 2);
                assert.equal(diagnostic.range.start.character, 15);
                assert.match(diagnostic.message, /expected Int32, found String/);
            }
        });

        test("the open file's task builds the open file", async () => {
            await open(file("files", "hello.fb"));
            const tasks = await vscode.tasks.fetchTasks({ type: "futureblitz" });
            const build = tasks.find((task) => inFolder(task, "files") && task.name === "build current file");
            assert.ok(build !== undefined);
            assert.equal(await runTask(build), 0);
            const executable = path.join(folder("files").uri.fsPath, process.platform === "win32" ? "hello.exe" : "hello");
            assert.ok(fs.existsSync(executable));
        });

        test("a package's tests, and Run on its file", async () => {
            const tasks = await vscode.tasks.fetchTasks({ type: "futureblitz" });
            const test = tasks.find((task) => inFolder(task, "app") && task.name === "test");
            assert.ok(test !== undefined);
            assert.equal(await runTask(test), 0);
            await open(file("app", "main.fb"));
            assert.equal(await vscode.commands.executeCommand("futureblitz.run"), 0);
        });
    });
});

suite("debugging", () => {
    function launches(): Record<string, unknown>[] {
        const stand_in = vscode.extensions.getExtension<{ launches: Record<string, unknown>[] }>("vadimcn.vscode-lldb");
        assert.ok(stand_in !== undefined);
        return stand_in.exports?.launches ?? [];
    }

    test("F5 with no launch.json debugs the open file's package, or the file", async () => {
        await open(file("app", "main.fb"));
        const empty = {} as vscode.DebugConfiguration;
        const inPackage = api.debug.resolveDebugConfiguration(folder("app"), empty);
        assert.equal(inPackage?.type, "futureblitz");
        assert.equal(inPackage?.request, "launch");
        assert.equal(inPackage?.program, folder("app").uri.fsPath);
        await open(file("files", "hello.fb"));
        const lone = api.debug.resolveDebugConfiguration(folder("files"), {} as vscode.DebugConfiguration);
        assert.equal(lone?.type, "futureblitz");
        assert.equal(lone?.program, file("files", "hello.fb").fsPath);
    });

    test("launch.json's first configuration", () => {
        assert.deepEqual(api.debug.provideDebugConfigurations(folder("app")), [
            { type: "futureblitz", request: "launch", name: "Debug", program: "${workspaceFolder}" },
        ]);
        assert.equal(api.debug.provideDebugConfigurations(folder("files"))[0].program, "${file}");
    });

    suite("with fb", function () {
        suiteSetup(function () {
            if (fb === "") {
                this.skip();
            }
        });

        test("builds in debug and launches CodeLLDB with fb's script", async () => {
            const before = launches().length;
            const started = await vscode.debug.startDebugging(folder("app"), {
                type: "futureblitz",
                request: "launch",
                name: "Debug app",
                program: "${workspaceFolder}",
                args: ["--level", "2"],
                release: false,
                stopOnEntry: true,
            });
            assert.ok(started);
            const launch = await waitFor("CodeLLDB's launch", () => launches()[before]);
            const executable = path.join(
                folder("app").uri.fsPath,
                "build",
                process.platform === "win32" ? "app.exe" : "app",
            );
            // VS Code's ${workspaceFolder} and a folder's fsPath can differ in the drive letter's case.
            const same = (a: unknown, b: string): void =>
                assert.equal(
                    process.platform === "win32" ? String(a).toLowerCase() : a,
                    process.platform === "win32" ? b.toLowerCase() : b,
                );
            assert.equal(launch.type, "lldb");
            same(launch.program, executable);
            assert.ok(fs.existsSync(executable));
            assert.deepEqual(launch.args, ["--level", "2"]);
            assert.equal(launch.stopOnEntry, true);
            assert.equal(launch.terminal, "integrated");
            same(launch.cwd, folder("app").uri.fsPath);
            assert.equal(launch.release, undefined);
            const commands = launch.initCommands as string[];
            const script = /^command script import "(.*)"$/.exec(commands.at(-1) ?? "");
            assert.ok(script !== null, commands.join("\n"));
            assert.ok(fs.existsSync(script[1]), script[1]);
        });

        test("a build that fails launches nothing", async () => {
            const before = launches().length;
            const started = await vscode.debug.startDebugging(folder("files"), {
                type: "futureblitz",
                request: "launch",
                name: "Debug broken",
                program: "${workspaceFolder}/broken.fb",
            });
            assert.equal(started, false);
            assert.equal(launches().length, before);
        });
    });
});
