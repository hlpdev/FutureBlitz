// FutureBlitz for VS Code: the grammar, language configuration, and snippets come from
// package.json; this starts the language server's client and adds the tasks, the debugger's
// configurations, and the commands.

import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import { ServerClient } from "./client";
import { DEBUG_TYPE, FbDebugConfigurationProvider, defaultProgram } from "./debug";
import { FbCommand, relativeTarget } from "./fb";
import { Found, currentEnvironment, findFb, findLldbScript, findLsp } from "./paths";
import { FbTaskProvider, TASK_TYPE, runTask } from "./tasks";
import { FbTestController } from "./tests";

// What the extension tests reach through the extension's exports.
export interface Api {
    server: ServerClient;
    tasks: FbTaskProvider;
    debug: FbDebugConfigurationProvider;
    tests: FbTestController;
    locateFb: (folder: vscode.WorkspaceFolder | undefined) => Found;
    lldbScript: (folder: vscode.WorkspaceFolder | undefined) => string | undefined;
}

function settings(folder?: vscode.WorkspaceFolder): vscode.WorkspaceConfiguration {
    return vscode.workspace.getConfiguration("futureblitz", folder);
}

// An LSP Location, as a command's arguments have it.
interface LspLocation {
    uri: string;
    range: { start: { line: number; character: number }; end: { line: number; character: number } };
}

function firstFolder(): vscode.WorkspaceFolder | undefined {
    return vscode.workspace.workspaceFolders?.[0];
}

export function activate(context: vscode.ExtensionContext): Api {
    const output = vscode.window.createOutputChannel("FutureBlitz", { log: true });
    const environment = currentEnvironment();
    const bundledScript = context.asAbsolutePath(path.join("lldb", "futureblitz.py"));

    const locateFb = (folder: vscode.WorkspaceFolder | undefined): Found =>
        findFb(settings(folder).get<string>("path", ""), (folder ?? firstFolder())?.uri.fsPath, environment);
    const lldbScript = (folder: vscode.WorkspaceFolder | undefined): string | undefined =>
        findLldbScript(
            settings(folder).get<string>("debug.lldbScript", ""),
            locateFb(folder).path,
            bundledScript,
            (folder ?? firstFolder())?.uri.fsPath,
            environment,
        ).path;

    const server = new ServerClient(context, output, () => {
        const folder = firstFolder()?.uri.fsPath;
        const fb = findFb(settings().get<string>("path", ""), folder, environment).path;
        return findLsp(settings().get<string>("lspPath", ""), fb, folder, environment);
    });
    const tasks = new FbTaskProvider(locateFb);
    const debug = new FbDebugConfigurationProvider({ tasks, locateFb, lldbScript });
    const tests = new FbTestController({ locateFb, lldbScript });
    server.onDidMakeClient((client) => tests.attach(client));

    context.subscriptions.push(
        output,
        server,
        tests,
        vscode.tasks.registerTaskProvider(TASK_TYPE, tasks),
        vscode.debug.registerDebugConfigurationProvider(DEBUG_TYPE, debug),
        vscode.debug.registerDebugConfigurationProvider(
            DEBUG_TYPE,
            {
                provideDebugConfigurations: (folder) => debug.provideDebugConfigurations(folder),
            },
            vscode.DebugConfigurationProviderTriggerKind.Dynamic,
        ),
        vscode.commands.registerCommand("futureblitz.restartServer", () => server.start()),
        vscode.commands.registerCommand("futureblitz.showOutput", () => output.show(true)),
        vscode.commands.registerCommand("futureblitz.run", () => runCommand("run", tasks)),
        vscode.commands.registerCommand("futureblitz.test", () => runCommand("test", tasks)),
        // A code lens's count of references, which fb-lsp's codeLens/resolve gives as JSON.
        vscode.commands.registerCommand(
            "futureblitz.showReferences",
            (uri: string, position: { line: number; character: number }, locations: LspLocation[]) =>
                vscode.commands.executeCommand(
                    "editor.action.showReferences",
                    vscode.Uri.parse(uri),
                    new vscode.Position(position.line, position.character),
                    locations.map(
                        (location) =>
                            new vscode.Location(
                                vscode.Uri.parse(location.uri),
                                new vscode.Range(
                                    location.range.start.line,
                                    location.range.start.character,
                                    location.range.end.line,
                                    location.range.end.character,
                                ),
                            ),
                    ),
                ),
        ),
        vscode.commands.registerCommand("futureblitz.debug", () => {
            const folder = activeFolder();
            const program = defaultProgram(folder);
            return vscode.debug.startDebugging(folder, {
                type: DEBUG_TYPE,
                request: "launch",
                name: "Debug",
                ...(program === undefined ? {} : { program }),
            });
        }),
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (
                event.affectsConfiguration("futureblitz.path") ||
                event.affectsConfiguration("futureblitz.lspPath") ||
                event.affectsConfiguration("futureblitz.lsp.enabled")
            ) {
                void server.start();
            }
        }),
    );

    void server.start();
    return { server, tasks, debug, tests, locateFb, lldbScript };
}

export function deactivate(): void {}

// The workspace folder of the open file, or the first one.
function activeFolder(): vscode.WorkspaceFolder | undefined {
    const editor = vscode.window.activeTextEditor;
    return (editor === undefined ? undefined : vscode.workspace.getWorkspaceFolder(editor.document.uri)) ?? firstFolder();
}

// Runs fb on the open file's package, or the file, or the workspace folder's package.
async function runCommand(command: FbCommand, tasks: FbTaskProvider): Promise<number | undefined> {
    const folder = activeFolder();
    const program = defaultProgram(folder);
    if (program === undefined) {
        void vscode.window.showErrorMessage(`FutureBlitz: open a .fb file to ${command}.`);
        return undefined;
    }

    const editor = vscode.window.activeTextEditor;
    if (editor !== undefined && editor.document.isDirty) {
        await editor.document.save();
    }

    const target =
        folder !== undefined && fs.existsSync(program) ? relativeTarget(program, folder.uri.fsPath) : program;
    return runTask(tasks.make({ type: TASK_TYPE, command, target }, folder));
}
