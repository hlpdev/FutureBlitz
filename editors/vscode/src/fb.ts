// What the extension asks fb to do, without VS Code, so the unit tests can run it under plain
// Node.

import * as path from "node:path";

// What asks fb (cli/src/terminal.cpp's diagnostic_columns) for its diagnostics' columns in UTF-16
// code units, as VS Code counts them, not code points.
export const COLUMNS = { name: "FB_COLUMNS", value: "utf-16" };

// What asks fb (compiler/src/driver.cpp's shown_disk_path) to name files by their whole paths,
// as a task with no folder open does, whose problem matchers have no folder to find a file from.
export const PATHS = { name: "FB_PATHS", value: "absolute" };

// The problem matchers of a task in a workspace folder, which find a file from the folder.
export const PROBLEM_MATCHERS = ["$fb", "$fb-note", "$fb-runtime"];
// With no folder open, VS Code drops a problem matcher whose file location names ${cwd}, which it
// can't resolve there, so a task finds its problems by the whole paths it asks fb for.
export const WHOLE_PATH_MATCHERS = ["$fb-whole", "$fb-note-whole", "$fb-runtime-whole"];

// The environment fb runs in for a task, in a workspace folder or with none.
export function taskEnvironment(inFolder: boolean): Record<string, string> {
    const env: Record<string, string> = { [COLUMNS.name]: COLUMNS.value };
    if (!inFolder) {
        env[PATHS.name] = PATHS.value;
    }

    return env;
}

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

// What project.toml's [project] section says, as compiler/src/manifest.cpp reads it: the name, the
// type, "executable" without one, and the main file, "main.fb" without one.
export interface Project {
    name?: string;
    type: string;
    main: string;
}

export function projectOf(projectToml: string): Project {
    const project: Project = { type: "executable", main: "main.fb" };
    let inProject = false;
    for (const raw of projectToml.split(/\r\n|\r|\n/)) {
        const line = raw.replace(/#.*$/, "").trim();
        const section = /^\[\s*([^\]]+?)\s*\]$/.exec(line);
        if (section !== null) {
            inProject = section[1] === "project";
            continue;
        }

        const field = /^(name|type|main)\s*=\s*(?:"([^"]*)"|'([^']*)')$/.exec(line);
        if (inProject && field !== null) {
            const value = field[2] ?? field[3];
            if (field[1] === "name") {
                project.name = value;
            } else if (field[1] === "type") {
                project.type = value;
            } else {
                project.main = value.replaceAll("\\", "/").replace(/^\.\//, "");
            }
        }
    }

    return project;
}

// The package's name from project.toml's [project] section, or undefined without one.
export function packageName(projectToml: string): string | undefined {
    return projectOf(projectToml).name;
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

// The package a program is in: its folder and its project.toml.
export interface Owner {
    root: string;
    project: Project;
}

// Where a debug build goes, as fb puts it without -o (cli/src/commands.cpp's build and
// compiler/src/driver.cpp's program name): a package's, or its main file's, in its build/ folder,
// named for the package; another of its files in build/ too, named for the file; and a file
// outside any package beside itself.
export function debugExecutable(program: string, owner: Owner | undefined, platform: NodeJS.Platform): string {
    const paths = platform === "win32" ? path.win32 : path.posix;
    const extension = platform === "win32" ? ".exe" : "";
    const parsed = paths.parse(program);
    if (owner === undefined) {
        return paths.join(parsed.dir, parsed.name + extension);
    }

    const relative = paths.relative(owner.root, program).replaceAll("\\", "/");
    const isPackage = relative === "" || relative === owner.project.main;
    const name = isPackage && owner.project.type !== "library" ? owner.project.name : parsed.name;
    return paths.join(owner.root, "build", (name ?? parsed.name) + extension);
}

// Says that fb wasn't found, naming the setting when it names a file that isn't there.
export function missingFbMessage(missingSetting: string | undefined): string {
    if (missingSetting !== undefined) {
        return `futureblitz.path is ${missingSetting}, which isn't there; set it to fb, or clear it to find fb on PATH.`;
    }

    return "fb isn't installed where FutureBlitz looks: set futureblitz.path to it, put it on PATH, or install it into ~/.futureblitz.";
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
