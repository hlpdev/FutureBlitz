// The Testing panel: each file's @Test Functions, as fb-lsp finds them (fb/tests), which fb test
// runs, and which a debug run builds with fb test -o and runs under CodeLLDB, with fb's lldb
// script. VS Code puts their run and debug buttons in the gutter too.

import { ChildProcess, spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import * as vscode from "vscode";
import { LanguageClient, State } from "vscode-languageclient/node";

import {
    TestOutcome,
    findPackageRoot,
    lldbImportCommand,
    missingFbMessage,
    moduleFilter,
    taskEnvironment,
    testArguments,
    testOutcomes,
} from "./fb";
import { Found } from "./paths";

// One file's tests, as fb-lsp sends them.
interface FileTests {
    uri: string;
    tests: {
        name: string;
        label: string;
        range: { start: { line: number; character: number }; end: { line: number; character: number } };
    }[];
}

export interface TestHelpers {
    locateFb: (folder: vscode.WorkspaceFolder | undefined) => Found;
    lldbScript: (folder: vscode.WorkspaceFolder | undefined) => string | undefined;
}

// What fb runs a test item's tests on: its package's folder, or a file outside any package, and
// the filter that picks them.
interface Target {
    folder: vscode.WorkspaceFolder | undefined;
    root: string;
    filter: string;
    tests: vscode.TestItem[];
}

export class FbTestController implements vscode.Disposable {
    private readonly m_controller: vscode.TestController;
    private readonly m_disposables: vscode.Disposable[] = [];
    private m_client: LanguageClient | undefined;

    // What the last run said of each test, by its name, for the extension's tests.
    readonly outcomes = new Map<string, TestOutcome>();

    constructor(private readonly m_helpers: TestHelpers) {
        this.m_controller = vscode.tests.createTestController("futureblitz", "FutureBlitz");
        this.m_controller.createRunProfile(
            "Run",
            vscode.TestRunProfileKind.Run,
            (request, token) => this.run(request, token, false),
            true,
        );
        this.m_controller.createRunProfile(
            "Debug",
            vscode.TestRunProfileKind.Debug,
            (request, token) => this.run(request, token, true),
            true,
        );
        this.m_controller.refreshHandler = () => this.refresh();
    }

    get controller(): vscode.TestController {
        return this.m_controller;
    }

    // Takes each file's tests from `client` as fb-lsp finds them, and all of them once it runs.
    attach(client: LanguageClient): void {
        this.m_client = client;
        client.onNotification("fb/tests", (file: FileTests) => this.update(file));
        this.m_disposables.push(
            client.onDidChangeState((change) => {
                if (change.newState === State.Running && this.m_client === client) {
                    void this.refresh();
                }
            }),
        );
    }

    // Asks fb-lsp for every file's tests.
    async refresh(): Promise<void> {
        const client = this.m_client;
        if (client === undefined || client.state !== State.Running) {
            return;
        }

        const files = await client.sendRequest<FileTests[]>("fb/tests", {});
        const known = new Set(files.map((file) => vscode.Uri.parse(file.uri).toString()));
        this.m_controller.items.forEach((item) => {
            if (!known.has(item.id)) {
                this.m_controller.items.delete(item.id);
            }
        });
        for (const file of files) {
            this.update(file);
        }
    }

    private update(file: FileTests): void {
        const uri = vscode.Uri.parse(file.uri);
        const id = uri.toString();
        if (file.tests.length === 0) {
            this.m_controller.items.delete(id);
            return;
        }

        let item = this.m_controller.items.get(id);
        if (item === undefined) {
            item = this.m_controller.createTestItem(id, path.basename(uri.fsPath), uri);
            this.m_controller.items.add(item);
        }

        item.children.replace(
            file.tests.map((test) => {
                const child = this.m_controller.createTestItem(test.name, test.label, uri);
                child.range = new vscode.Range(
                    test.range.start.line,
                    test.range.start.character,
                    test.range.end.line,
                    test.range.end.character,
                );
                return child;
            }),
        );
    }

    // Runs, or debugs, what the request asks for: each file's tests, or one test, in turn.
    async run(request: vscode.TestRunRequest, token: vscode.CancellationToken, debug: boolean): Promise<void> {
        const run = this.m_controller.createTestRun(request);
        const items: vscode.TestItem[] = [];
        if (request.include !== undefined) {
            items.push(...request.include);
        } else {
            this.m_controller.items.forEach((item) => items.push(item));
        }

        const excluded = new Set((request.exclude ?? []).map((item) => item.id));
        try {
            for (const item of items) {
                if (token.isCancellationRequested) {
                    break;
                }

                const target = this.targetOf(item, excluded);
                if (target === undefined) {
                    continue;
                }

                await (debug ? this.debug(target, run, token) : this.runTests(target, run, token));
            }
        } finally {
            run.end();
        }
    }

    private targetOf(item: vscode.TestItem, excluded: Set<string>): Target | undefined {
        const uri = item.uri;
        if (uri === undefined || excluded.has(item.id)) {
            return undefined;
        }

        const tests: vscode.TestItem[] = [];
        if (item.children.size > 0) {
            item.children.forEach((child) => {
                if (!excluded.has(child.id)) {
                    tests.push(child);
                }
            });
        } else {
            tests.push(item);
        }

        if (tests.length === 0) {
            return undefined;
        }

        const folder = vscode.workspace.getWorkspaceFolder(uri);
        const root = findPackageRoot(uri.fsPath, folder?.uri.fsPath, (file) => fs.existsSync(file)) ?? uri.fsPath;
        // A file's tests, or the one.
        const filter = item.children.size > 0 ? moduleFilter(tests[0].id) : item.id;
        return { folder, root, filter, tests };
    }

    // fb, or undefined after saying it's missing.
    private fbFor(folder: vscode.WorkspaceFolder | undefined): string | undefined {
        const found = this.m_helpers.locateFb(folder);
        if (found.path === undefined) {
            void vscode.window.showErrorMessage(`FutureBlitz: ${missingFbMessage(found.missingSetting)}`);
        }

        return found.path;
    }

    // Runs fb with `args` in `cwd`, putting what it prints in the run's output, until it ends or
    // the run is cancelled.
    private async fb(
        fb: string,
        args: string[],
        cwd: string,
        run: vscode.TestRun,
        token: vscode.CancellationToken,
    ): Promise<{ code: number | null; output: string }> {
        const child: ChildProcess = spawn(fb, args, {
            cwd,
            env: { ...process.env, ...taskEnvironment(true) },
        });
        let output = "";
        const take = (chunk: Buffer): void => {
            const text = chunk.toString("utf8");
            output += text;
            run.appendOutput(text.replace(/\r?\n/g, "\r\n"));
        };
        child.stdout?.on("data", take);
        child.stderr?.on("data", take);
        const cancelled = token.onCancellationRequested(() => child.kill());
        try {
            const code = await new Promise<number | null>((resolve) => {
                child.on("error", (error) => {
                    take(Buffer.from(`${error.message}\n`));
                    resolve(null);
                });
                child.on("close", (exit) => resolve(exit));
            });
            return { code, output };
        } finally {
            cancelled.dispose();
        }
    }

    private async runTests(target: Target, run: vscode.TestRun, token: vscode.CancellationToken): Promise<void> {
        const fb = this.fbFor(target.folder);
        if (fb === undefined) {
            return;
        }

        for (const test of target.tests) {
            run.started(test);
        }

        const cwd = fs.statSync(target.root).isDirectory() ? target.root : path.dirname(target.root);
        const { code, output } = await this.fb(fb, testArguments(target.root, target.filter), cwd, run, token);
        const outcomes = testOutcomes(output);
        for (const test of target.tests) {
            const outcome = outcomes.get(test.id);
            if (outcome !== undefined) {
                this.outcomes.set(test.id, outcome);
            }

            if (outcome?.state === "passed") {
                run.passed(test);
            } else if (outcome?.state === "skipped") {
                run.skipped(test);
            } else if (outcome?.state === "failed") {
                run.failed(test, new vscode.TestMessage(outcome.messages.join("\n")));
            } else if (token.isCancellationRequested) {
                run.skipped(test);
            } else {
                // Its build failed, or it never ran.
                run.errored(
                    test,
                    new vscode.TestMessage(
                        code === null ? "fb test didn't run; its output says why" : `fb test ended with ${code} before it ran`,
                    ),
                );
            }
        }
    }

    // Builds the tests' executable with fb test -o and runs it under CodeLLDB, until the session
    // ends; the executable says how each test did in the debugger's terminal.
    private async debug(target: Target, run: vscode.TestRun, token: vscode.CancellationToken): Promise<void> {
        const fb = this.fbFor(target.folder);
        if (fb === undefined) {
            return;
        }

        const scratch = fs.mkdtempSync(path.join(os.tmpdir(), "fb-tests-"));
        const executable = path.join(scratch, process.platform === "win32" ? "tests.exe" : "tests");
        const cwd = fs.statSync(target.root).isDirectory() ? target.root : path.dirname(target.root);
        for (const test of target.tests) {
            run.started(test);
        }

        const { code } = await this.fb(fb, testArguments(target.root, target.filter, executable), cwd, run, token);
        if (code !== 0 || !fs.existsSync(executable)) {
            for (const test of target.tests) {
                run.errored(test, new vscode.TestMessage("the tests didn't build; the output says why"));
            }

            return;
        }

        const script = this.m_helpers.lldbScript(target.folder);
        const name = `Debug ${target.tests.length === 1 ? target.tests[0].label : target.filter}`;
        const ended = new Promise<void>((resolve) => {
            const listener = vscode.debug.onDidTerminateDebugSession((session) => {
                if (session.name === name) {
                    listener.dispose();
                    resolve();
                }
            });
        });
        const started = await vscode.debug.startDebugging(
            target.folder,
            {
                type: "lldb",
                request: "launch",
                name,
                program: executable,
                cwd,
                terminal: "integrated",
                initCommands: script === undefined ? [] : [lldbImportCommand(script)],
            },
            { testRun: run },
        );
        if (started) {
            await ended;
        }

        fs.rmSync(scratch, { recursive: true, force: true });
    }

    dispose(): void {
        for (const disposable of this.m_disposables) {
            disposable.dispose();
        }

        this.m_controller.dispose();
    }
}
