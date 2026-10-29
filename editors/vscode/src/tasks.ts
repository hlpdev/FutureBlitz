// Tasks for fb build, run, test, fmt, and check, with problem matchers for fb's diagnostics and
// for what a program reports as it runs: panics, uncaught errors, and failed Expects.

import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import { FbCommand, TaskDefinition, taskArguments, taskLabel } from "./fb";

export const TASK_TYPE = "futureblitz";
export const PROBLEM_MATCHERS = ["$fb", "$fb-note", "$fb-runtime"];

// fb's path for a workspace folder, or undefined when it can't be found.
export type FbLocator = (folder: vscode.WorkspaceFolder | undefined) => string | undefined;

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
    // the problem matchers take them.
    make(definition: TaskDefinition, folder: vscode.WorkspaceFolder | undefined, from?: vscode.Task): vscode.Task {
        const scope = folder ?? vscode.TaskScope.Workspace;
        const name = from?.name ?? taskLabel(definition);
        const fb = this.m_locate(folder);
        const execution =
            fb === undefined
                ? missingFb()
                : new vscode.ProcessExecution(fb, taskArguments(definition), {
                      cwd: folder?.uri.fsPath,
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

// Runs a task to its end, giving its exit code, or undefined when it ended without one.
export async function runTask(task: vscode.Task): Promise<number | undefined> {
    const execution = await vscode.tasks.executeTask(task);
    return new Promise((resolve) => {
        const ended = vscode.tasks.onDidEndTaskProcess((event) => {
            if (event.execution === execution) {
                ended.dispose();
                finished.dispose();
                resolve(event.exitCode);
            }
        });
        // In case a task ends without a process end, which a CustomExecution's close gives too.
        const finished = vscode.tasks.onDidEndTask((event) => {
            if (event.execution === execution) {
                setTimeout(() => {
                    ended.dispose();
                    finished.dispose();
                    resolve(undefined);
                }, 500);
            }
        });
    });
}

// Says that fb wasn't found, where the task's output goes, and fails.
function missingFb(): vscode.CustomExecution {
    return new vscode.CustomExecution(async () => {
        const write = new vscode.EventEmitter<string>();
        const close = new vscode.EventEmitter<number>();
        return {
            onDidWrite: write.event,
            onDidClose: close.event,
            open: () => {
                write.fire(
                    "fb isn't installed where FutureBlitz looks: set futureblitz.path to it, put it on " +
                        "PATH, or install it into ~/.futureblitz.\r\n",
                );
                close.fire(1);
            },
            close: () => {},
        };
    });
}
