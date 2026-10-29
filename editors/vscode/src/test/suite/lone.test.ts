// The extension with no folder open, on a copy of test/fixtures/lone/hello.fb: F5 and FutureBlitz:
// Debug build the file beside itself and run it in its folder. The tests need fb, from
// FUTUREBLITZ_FB; they're skipped without it.

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import type { Api } from "../../extension";
import { runTask } from "../../tasks";

const fb = process.env.FUTUREBLITZ_FB ?? "";
const lone = process.env.FB_TEST_LONE ?? "";

function launches(): Record<string, unknown>[] {
    const standIn = vscode.extensions.getExtension<{ launches: Record<string, unknown>[] }>("vadimcn.vscode-lldb");
    assert.ok(standIn !== undefined);
    return standIn.exports?.launches ?? [];
}

async function waitFor<T>(what: string, get: () => T | undefined, timeout = 30000): Promise<T> {
    const end = Date.now() + timeout;
    while (Date.now() < end) {
        const value = get();
        if (value !== undefined) {
            return value;
        }

        await new Promise((resolve) => setTimeout(resolve, 100));
    }

    throw new Error(`timed out waiting for ${what}`);
}

// Paths compared as Windows has them, whatever their case.
function same(a: unknown, b: string): void {
    const fold = (text: string): string => (process.platform === "win32" ? text.toLowerCase() : text);
    assert.equal(fold(String(a)), fold(b));
}

suite("with no folder open", function () {
    let api: Api;

    suiteSetup(async function () {
        if (fb === "") {
            this.skip();
        }

        assert.equal(vscode.workspace.workspaceFolders, undefined);
        await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(lone));
        const extension = vscode.extensions.getExtension<Api>("futureblitz.futureblitz");
        assert.ok(extension !== undefined);
        api = await extension.activate();
        await vscode.workspace.getConfiguration("futureblitz").update("path", fb, vscode.ConfigurationTarget.Global);
    });

    test("F5 with no launch.json takes the open file", () => {
        const config = api.debug.resolveDebugConfiguration(undefined, {} as vscode.DebugConfiguration);
        same(config?.program, lone);
    });

    test("debugging the file builds it beside itself and runs it in its folder", async () => {
        const before = launches().length;
        const started = await vscode.debug.startDebugging(undefined, {
            type: "futureblitz",
            request: "launch",
            name: "Debug the file",
            program: lone,
        });
        assert.ok(started);
        const launch = await waitFor("CodeLLDB's launch", () => launches()[before]);
        const executable = path.join(path.dirname(lone), process.platform === "win32" ? "hello.exe" : "hello");
        same(launch.program, executable);
        assert.ok(fs.existsSync(executable));
        same(launch.cwd, path.dirname(lone));
    });

    // The problem matchers take paths from the task's own folder, which a window with no folder
    // still has.
    test("a lone file's build errors go to Problems", async () => {
        const broken = path.join(path.dirname(lone), "broken.fb");
        const task = api.tasks.make({ type: "futureblitz", command: "build", target: broken }, undefined);
        assert.equal(await runTask(task), 1);
        const uri = vscode.Uri.file(broken);
        const [diagnostic] = await waitFor("the build's error", () => {
            const found = vscode.languages.getDiagnostics(uri);
            return found.length > 0 ? found : undefined;
        });
        assert.equal(diagnostic.range.start.line, 2);
        assert.match(diagnostic.message, /expected Int32, found String/);
    });

    test("FutureBlitz: Debug does too", async () => {
        const before = launches().length;
        await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(lone));
        assert.ok(await vscode.commands.executeCommand("futureblitz.debug"));
        const launch = await waitFor("CodeLLDB's launch", () => launches()[before]);
        same(launch.cwd, path.dirname(lone));
    });
});
