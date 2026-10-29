// Debugging: a futureblitz launch configuration builds the program in debug with fb and runs it
// under CodeLLDB, which loads fb's lldb script so Strings, arrays, Maps, nullables, and objects
// show as their values. CodeLLDB debugs on every OS, Windows too.

import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import {
    Owner,
    debugExecutable,
    findPackageRoot,
    lldbImportCommand,
    missingFbMessage,
    projectOf,
    relativeTarget,
} from "./fb";
import { Found } from "./paths";
import { FbTaskProvider, TASK_TYPE, runTask } from "./tasks";

export const DEBUG_TYPE = "futureblitz";
export const CODELLDB = "vadimcn.vscode-lldb";

export interface DebugHelpers {
    tasks: FbTaskProvider;
    locateFb: (folder: vscode.WorkspaceFolder | undefined) => Found;
    lldbScript: (folder: vscode.WorkspaceFolder | undefined) => string | undefined;
}

function exists(file: string): boolean {
    try {
        fs.statSync(file);
        return true;
    } catch {
        return false;
    }
}

// Whether `file` was written since `since`, a time from Date.now(), give or take the file system's
// clock.
function built(file: string, since: number): boolean {
    try {
        return fs.statSync(file).mtimeMs >= since - 1000;
    } catch {
        return false;
    }
}

function isDirectory(file: string): boolean {
    try {
        return fs.statSync(file).isDirectory();
    } catch {
        return false;
    }
}

// What a launch without a program builds: the open .fb file's package, or the file itself, or the
// workspace folder's package.
export function defaultProgram(folder: vscode.WorkspaceFolder | undefined): string | undefined {
    const editor = vscode.window.activeTextEditor;
    if (editor !== undefined && editor.document.languageId === "futureblitz" && editor.document.uri.scheme === "file") {
        const file = editor.document.uri.fsPath;
        const owner = vscode.workspace.getWorkspaceFolder(editor.document.uri) ?? folder;
        return findPackageRoot(file, owner?.uri.fsPath, exists) ?? file;
    }

    if (folder !== undefined && exists(path.join(folder.uri.fsPath, "project.toml"))) {
        return folder.uri.fsPath;
    }

    return undefined;
}

export class FbDebugConfigurationProvider implements vscode.DebugConfigurationProvider {
    constructor(private readonly m_helpers: DebugHelpers) {}

    provideDebugConfigurations(folder: vscode.WorkspaceFolder | undefined): vscode.DebugConfiguration[] {
        const hasPackage = folder !== undefined && exists(path.join(folder.uri.fsPath, "project.toml"));
        return [
            {
                type: DEBUG_TYPE,
                request: "launch",
                name: "Debug",
                program: hasPackage ? "${workspaceFolder}" : "${file}",
            },
        ];
    }

    // F5 with no launch.json gives an empty configuration: it debugs the open file's program.
    resolveDebugConfiguration(
        folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration,
    ): vscode.DebugConfiguration | undefined {
        if (config.type === undefined && config.request === undefined && config.name === undefined) {
            config.type = DEBUG_TYPE;
            config.request = "launch";
            config.name = "Debug";
        }

        if (config.program === undefined || config.program === "") {
            const program = defaultProgram(folder);
            if (program === undefined) {
                void vscode.window.showErrorMessage(
                    "FutureBlitz: open a .fb file, or give the launch configuration a program, to debug.",
                );
                return undefined;
            }

            config.program = program;
        }

        return config;
    }

    // Builds the program in debug, then hands CodeLLDB the executable.
    async resolveDebugConfigurationWithSubstitutedVariables(
        folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration,
    ): Promise<vscode.DebugConfiguration | undefined> {
        if (vscode.extensions.getExtension(CODELLDB) === undefined) {
            const choice = await vscode.window.showErrorMessage(
                "FutureBlitz debugs through CodeLLDB, which isn't installed.",
                "Install CodeLLDB",
            );
            if (choice === "Install CodeLLDB") {
                await vscode.commands.executeCommand("workbench.extensions.installExtension", CODELLDB);
            }

            return undefined;
        }

        const fb = this.m_helpers.locateFb(folder);
        if (fb.path === undefined) {
            void vscode.window.showErrorMessage(`FutureBlitz: ${missingFbMessage(fb.missingSetting)}`);
            return undefined;
        }

        // A relative program is the workspace folder's, or with none the open file's folder's.
        const editor = vscode.window.activeTextEditor;
        const from =
            folder?.uri.fsPath ??
            (editor?.document.uri.scheme === "file" ? path.dirname(editor.document.uri.fsPath) : process.cwd());
        const program = path.resolve(from, String(config.program));
        if (!exists(program)) {
            void vscode.window.showErrorMessage(`FutureBlitz: there's no ${program} to debug.`);
            return undefined;
        }

        // The program's own workspace folder, which in a workspace of several isn't always the one
        // VS Code passes, else none, when the program's folder is where it runs.
        const own = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(program)) ?? folder;
        const ownPath = own?.uri.fsPath;
        const root = isDirectory(program) ? program : findPackageRoot(program, ownPath, exists);
        let owner: Owner | undefined;
        if (root !== undefined) {
            const manifest = path.join(root, "project.toml");
            const project = projectOf(exists(manifest) ? fs.readFileSync(manifest, "utf8") : "");
            owner = { root, project };
            if (project.name === undefined) {
                void vscode.window.showErrorMessage(
                    `FutureBlitz: ${root} isn't a package; it needs a project.toml with a name.`,
                );
                return undefined;
            }

            if (project.type === "library") {
                void vscode.window.showErrorMessage(
                    `FutureBlitz: ${project.name} is a library, so it has no program to debug.`,
                );
                return undefined;
            }
        }

        const base = ownPath ?? (isDirectory(program) ? program : path.dirname(program));
        const executable = debugExecutable(program, owner, process.platform);
        const build = this.m_helpers.tasks.make(
            {
                type: TASK_TYPE,
                command: "build",
                target: own === undefined ? program : relativeTarget(program, own.uri.fsPath),
                release: config.release === true,
                args: ["-o", executable],
            },
            own,
        );
        // VS Code 1.143 can end the first task process in a window with no folder before it runs,
        // with no exit code. A build that ends so and wrote nothing runs once more.
        let started = Date.now();
        let code = await runTask(build);
        if (code === undefined && !built(executable, started)) {
            started = Date.now();
            code = await runTask(build);
        }

        if (code === undefined && built(executable, started)) {
            code = 0;
        }

        if (code === undefined) {
            void vscode.window.showErrorMessage(
                "FutureBlitz: the build didn't run, so there's nothing to debug; the Terminal and the " +
                    "FutureBlitz output say why.",
            );
            return undefined;
        }

        if (code !== 0) {
            void vscode.window.showErrorMessage(
                "FutureBlitz: the build failed, so there's nothing to debug; its errors are in Problems " +
                    "and the build's terminal.",
            );
            return undefined;
        }

        const launch: vscode.DebugConfiguration = {
            terminal: "integrated",
            cwd: base,
            ...config,
            type: "lldb",
            request: "launch",
            program: executable,
            args: config.args ?? [],
            initCommands: [...(config.initCommands ?? [])],
        };
        // Only fb reads it; CodeLLDB gets the program it built.
        delete launch.release;
        const script = this.m_helpers.lldbScript(folder);
        if (script !== undefined) {
            launch.initCommands.push(lldbImportCommand(script));
        }

        return launch;
    }
}
