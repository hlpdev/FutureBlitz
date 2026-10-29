// What the extension asks fb to do, without VS Code, so the unit tests can run it under plain
// Node.

import * as path from "node:path";

export type FbCommand = "build" | "run" | "test" | "fmt" | "check";

// A futureblitz task's definition, as tasks.json and package.json's taskDefinitions have it.
export interface TaskDefinition {
    type: "futureblitz";
    command: FbCommand;
    target?: string;
    release?: boolean;
    filter?: string;
    check?: boolean;
    args?: string[];
    programArgs?: string[];
}

// fb's arguments for a task: the command, its options, the target, and a run's program arguments
// after "--".
export function taskArguments(definition: TaskDefinition): string[] {
    const args: string[] = [definition.command];
    if (definition.release === true && definition.command !== "fmt") {
        args.push("--release");
    }

    if (definition.command === "test" && definition.filter !== undefined && definition.filter !== "") {
        args.push("--filter", definition.filter);
    }

    if (definition.command === "fmt" && definition.check === true) {
        args.push("--check");
    }

    args.push(...(definition.args ?? []));
    if (definition.target !== undefined && definition.target !== "") {
        args.push(definition.target);
    }

    if (definition.command === "run" && (definition.programArgs ?? []).length > 0) {
        args.push("--", ...(definition.programArgs ?? []));
    }

    return args;
}

// A task's name in the Run Task list: the command, and what it's on.
export function taskLabel(definition: TaskDefinition): string {
    const on =
        definition.target === undefined || definition.target === ""
            ? ""
            : definition.target === "${file}"
              ? " current file"
              : ` ${definition.target}`;
    return `${definition.command}${on}${definition.release === true ? " (release)" : ""}`;
}

// The package's name from project.toml's [project] section, or undefined without one.
export function packageName(projectToml: string): string | undefined {
    let inProject = false;
    for (const raw of projectToml.split(/\r\n|\r|\n/)) {
        const line = raw.replace(/#.*$/, "").trim();
        const section = /^\[\s*([^\]]+?)\s*\]$/.exec(line);
        if (section !== null) {
            inProject = section[1] === "project";
            continue;
        }

        const name = /^name\s*=\s*"([^"]*)"$/.exec(line) ?? /^name\s*=\s*'([^']*)'$/.exec(line);
        if (inProject && name !== null) {
            return name[1];
        }
    }

    return undefined;
}

// The folder with the project.toml that `file` is in, from its folder up to `stop`, or undefined
// when there's none.
export function findPackageRoot(
    file: string,
    stop: string | undefined,
    exists: (file: string) => boolean,
    paths: path.PlatformPath = path,
): string | undefined {
    let folder = paths.dirname(paths.resolve(file));
    const end = stop === undefined ? undefined : paths.resolve(stop);
    while (true) {
        if (exists(paths.join(folder, "project.toml"))) {
            return folder;
        }

        // A package around the workspace folder isn't the file's to build.
        const relative = end === undefined ? "." : paths.relative(end, folder);
        if (relative === "" || relative.startsWith("..") || paths.isAbsolute(relative)) {
            return undefined;
        }

        const parent = paths.dirname(folder);
        if (parent === folder) {
            return undefined;
        }

        folder = parent;
    }
}

// Where a debug build goes, as fb puts it without -o: a package's in its build/ folder, named
// for the package, and a file's beside it, named for the file.
export function debugExecutable(
    program: string,
    name: string | undefined,
    platform: NodeJS.Platform,
): string {
    const paths = platform === "win32" ? path.win32 : path.posix;
    const extension = platform === "win32" ? ".exe" : "";
    if (name !== undefined) {
        return paths.join(program, "build", name + extension);
    }

    const parsed = paths.parse(program);
    return paths.join(parsed.dir, parsed.name + extension);
}

// lldb's command that loads fb's script. lldb reads a backslash in quotes as an escape, so the
// path goes with forward slashes, which Windows takes too.
export function lldbImportCommand(script: string): string {
    const forward = script.replaceAll("\\", "/").replaceAll('"', '\\"');
    return `command script import "${forward}"`;
}

// A path as fb's task gets it: relative to the workspace folder when it's inside, so fb's
// messages are too, else as it is.
export function relativeTarget(target: string, folder: string, paths: path.PlatformPath = path): string {
    const relative = paths.relative(folder, target);
    if (relative === "") {
        return ".";
    }

    return relative.startsWith("..") || paths.isAbsolute(relative) ? target : relative;
}
