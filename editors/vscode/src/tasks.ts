// Tasks for fb build, run, test, fmt, and check, with problem matchers for fb's diagnostics and
// for what a program reports as it runs: panics, uncaught errors, failed Expects, and tests that
// fail or throw.

import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import { COLUMNS, FbCommand, TaskDefinition, missingFbMessage, taskArguments, taskLabel } from "./fb";
import { Found } from "./paths";

export const TASK_TYPE = "futureblitz";
export const PROBLEM_MATCHERS = ["$fb", "$fb-note", "$fb-runtime"];

// How long a task has to start before runTask gives up on it, as when VS Code can't resolve what
// it needs.
const START_TIMEOUT = 30 * 1000;

// Where fb is for a workspace folder, or where it was looked for.
export type FbLocator = (folder: vscode.WorkspaceFolder | undefined) => Found;

function isDirectory(file: string): boolean {
    try {
        return fs.statSync(file).isDirectory();
    } catch {
        return false;
    }
}

// Where a task without a workspace folder runs: the target's folder, or the target itself when
// it's a package's folder.
function targetFolder(target: string | undefined): string | undefined {
    if (target === undefined || target === "" || target.includes("${") || !path.isAbsolute(target)) {
        return undefined;
    }

    return isDirectory(target) ? target : path.dirname(target);
}

export class FbTaskProvider implements vscode.TaskProvider {
    constructor(private readonly m_locate: FbLocator) {}

    provideTasks(): vscode.Task[] {
        const tasks: vscode.Task[] = [];
        for (const folder of vscode.workspace.workspaceFolders ?? []) {
            const commands: FbCommand[] = ["build", "run", "test", "fmt", "check"];
            if (fs.existsSync(path.join(folder.uri.fsPath, "project.toml"))) {
                for (const command of commands) {
                    tasks.push(this.make({ type: TASK_TYPE, command }, folder));
                }
            }

            for (const command of ["build", "run", "fmt", "check"] as FbCommand[]) {
                tasks.push(this.make({ type: TASK_TYPE, command, target: "${file}" }, folder));
            }
        }

        return tasks;
    }

    resolveTask(task: vscode.Task): vscode.Task | undefined {
        const definition = task.definition as TaskDefinition;
        if (definition.command === undefined) {
            return undefined;
        }

        const folder = typeof task.scope === "object" ? task.scope : vscode.workspace.workspaceFolders?.[0];
        return this.make(definition, folder, task);
    }

    // The task for `definition`, run in the workspace folder, so fb's paths are relative to it as
    // the problem matchers take them. Without a folder, it runs in the target's, which needs an
    // absolute target.
    make(definition: TaskDefinition, folder: vscode.WorkspaceFolder | undefined, from?: vscode.Task): vscode.Task {
        const scope = folder ?? vscode.TaskScope.Workspace;
        const name = from?.name ?? taskLabel(definition);
        const fb = this.m_locate(folder);
        const execution =
            fb.path === undefined
                ? missingFb(missingFbMessage(fb.missingSetting))
                : new vscode.ProcessExecution(fb.path, taskArguments(definition), {
                      cwd: folder?.uri.fsPath ?? targetFolder(definition.target),
                      // VS Code's columns are UTF-16 code units, which fb counts in when asked.
                      env: { [COLUMNS.name]: COLUMNS.value },
                  });
        const task = new vscode.Task(definition, scope, name, "fb", execution, PROBLEM_MATCHERS);
        if (definition.command === "build" || definition.command === "check") {
            task.group = vscode.TaskGroup.Build;
        } else if (definition.command === "test") {
            task.group = vscode.TaskGroup.Test;
        }

        if (definition.command === "run") {
            task.presentationOptions = { focus: true };
        }

        return task;
    }
}

// Runs a task to its end, giving its exit code, or undefined when it ended without one, or didn't
// start in START_TIMEOUT.
export async function runTask(task: vscode.Task, startTimeout = START_TIMEOUT): Promise<number | undefined> {
    // The events are listened for before the task starts, so one that ends at once isn't missed.
    let execution: vscode.TaskExecution | undefined;
    const started: vscode.TaskExecution[] = [];
    const early: { execution: vscode.TaskExecution; code: number | undefined }[] = [];
    let finish: (code: number | undefined) => void = () => {};
    const result = new Promise<number | undefined>((resolve) => {
        finish = resolve;
    });

    const subscriptions = [
        vscode.tasks.onDidStartTask((event) => {
            started.push(event.execution);
        }),
        vscode.tasks.onDidEndTaskProcess((event) => {
            if (execution === undefined) {
                early.push({ execution: event.execution, code: event.exitCode });
            } else if (event.execution === execution) {
                finish(event.exitCode);
            }
        }),
        // In case a task ends without a process end.
        vscode.tasks.onDidEndTask((event) => {
            if (event.execution === execution) {
                setTimeout(() => finish(undefined), 500);
            }
        }),
    ];

    try {
        execution = await vscode.tasks.executeTask(task);
        const ended = early.find((event) => event.execution === execution);
        if (ended !== undefined) {
            return ended.code;
        }

        const timer = setTimeout(() => {
            if (execution === undefined || !started.includes(execution)) {
                finish(undefined);
            }
        }, startTimeout);
        const code = await result;
        clearTimeout(timer);
        return code;
    } finally {
        for (const subscription of subscriptions) {
            subscription.dispose();
        }
    }
}

// Says what's wrong with finding fb, where the task's output goes, and fails.
function missingFb(message: string): vscode.CustomExecution {
    return new vscode.CustomExecution(async () => {
        const write = new vscode.EventEmitter<string>();
        const close = new vscode.EventEmitter<number>();
        return {
            onDidWrite: write.event,
            onDidClose: close.event,
            open: () => {
                write.fire(`${message}\r\n`);
                close.fire(1);
            },
            close: () => {},
        };
    });
}
