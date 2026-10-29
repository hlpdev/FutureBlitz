// Finding fb, the language server, and fb's lldb script, without VS Code, so the unit tests can
// run them under plain Node.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

export interface Environment {
    platform: NodeJS.Platform;
    env: NodeJS.ProcessEnv;
    home: string;
    exists: (file: string) => boolean;
}

// Where something was found, or every place that was looked in when it wasn't.
export interface Found {
    path?: string;
    tried: string[];
    // The setting named a file that isn't there.
    missingSetting?: string;
}

export const LSP_NAME = "fb_lsp";

export function currentEnvironment(): Environment {
    return {
        platform: process.platform,
        env: process.env,
        home: os.homedir(),
        exists: (file) => {
            try {
                return fs.statSync(file).isFile();
            } catch {
                return false;
            }
        },
    };
}

// `name` as the OS names an executable: with ".exe" on Windows.
export function executableName(name: string, platform: NodeJS.Platform): string {
    return platform === "win32" && path.extname(name) === "" ? `${name}.exe` : name;
}

function pathModule(platform: NodeJS.Platform): path.PlatformPath {
    return platform === "win32" ? path.win32 : path.posix;
}

// A path setting with `${workspaceFolder}`, `${userHome}`, and a leading `~` expanded. An empty
// one stays empty.
export function expandPath(
    value: string,
    workspaceFolder: string | undefined,
    environment: Environment,
): string {
    let expanded = value.trim();
    if (expanded === "") {
        return "";
    }

    expanded = expanded.replaceAll("${userHome}", environment.home);
    if (workspaceFolder !== undefined) {
        expanded = expanded.replaceAll("${workspaceFolder}", workspaceFolder);
    }

    if (expanded === "~" || expanded.startsWith("~/") || expanded.startsWith("~\\")) {
        expanded = environment.home + expanded.slice(1);
    }

    return pathModule(environment.platform).normalize(expanded);
}

// The first `name` on PATH, trying each of PATHEXT's extensions on Windows.
export function findOnPath(name: string, environment: Environment): Found {
    const paths = pathModule(environment.platform);
    const separator = environment.platform === "win32" ? ";" : ":";
    // Windows keeps "Path", whatever its case.
    const key = Object.keys(environment.env).find((k) => k.toUpperCase() === "PATH") ?? "PATH";
    const folders = (environment.env[key] ?? "").split(separator).filter((f) => f !== "");
    const extensions =
        environment.platform === "win32"
            ? (environment.env.PATHEXT ?? ".EXE;.CMD;.BAT")
                  .split(";")
                  .filter((e) => e !== "")
                  .map((e) => e.toLowerCase())
            : [""];

    const tried: string[] = [];
    for (const folder of folders) {
        for (const extension of extensions) {
            const candidate = paths.join(folder.replace(/^"(.*)"$/, "$1"), name + extension);
            tried.push(candidate);
            if (environment.exists(candidate)) {
                return { path: candidate, tried };
            }
        }
    }

    return { tried };
}

// The first of `candidates` that exists.
function firstExisting(candidates: string[], environment: Environment): Found {
    const found = candidates.find((candidate) => environment.exists(candidate));
    return found === undefined ? { tried: candidates } : { path: found, tried: candidates };
}

// A setting's file: found when it's there, and missingSetting when it isn't. Undefined when the
// setting is empty.
function fromSetting(
    setting: string,
    workspaceFolder: string | undefined,
    environment: Environment,
): Found | undefined {
    const expanded = expandPath(setting, workspaceFolder, environment);
    if (expanded === "") {
        return undefined;
    }

    return environment.exists(expanded)
        ? { path: expanded, tried: [expanded] }
        : { tried: [expanded], missingSetting: expanded };
}

// fb: futureblitz.path, else fb on PATH, else ~/.futureblitz/bin, where the install scripts put
// it.
export function findFb(
    setting: string,
    workspaceFolder: string | undefined,
    environment: Environment,
): Found {
    const configured = fromSetting(setting, workspaceFolder, environment);
    if (configured !== undefined) {
        return configured;
    }

    const onPath = findOnPath("fb", environment);
    if (onPath.path !== undefined) {
        return onPath;
    }

    const paths = pathModule(environment.platform);
    const installed = paths.join(
        environment.home,
        ".futureblitz",
        "bin",
        executableName("fb", environment.platform),
    );
    const found = firstExisting([installed], environment);
    return { ...found, tried: [...onPath.tried, installed] };
}

// The language server: futureblitz.lspPath, else fb_lsp beside fb (a release's bin/) or in the
// lsp/ folder beside fb's cli/ (a CMake build folder), else fb_lsp on PATH.
export function findLsp(
    setting: string,
    fb: string | undefined,
    workspaceFolder: string | undefined,
    environment: Environment,
): Found {
    const configured = fromSetting(setting, workspaceFolder, environment);
    if (configured !== undefined) {
        return configured;
    }

    const paths = pathModule(environment.platform);
    const name = executableName(LSP_NAME, environment.platform);
    const beside =
        fb === undefined
            ? []
            : [
                  paths.join(paths.dirname(fb), name),
                  paths.join(paths.dirname(fb), "..", "lsp", name),
              ];
    const found = firstExisting(beside, environment);
    if (found.path !== undefined) {
        return found;
    }

    const onPath = findOnPath(LSP_NAME, environment);
    return { ...onPath, tried: [...beside, ...onPath.tried] };
}

// fb's lldb script: futureblitz.debug.lldbScript, else the one beside fb (a release's
// lldb/futureblitz.py, or the source tree's tools/lldb/ for a build folder in it), else the
// extension's own copy.
export function findLldbScript(
    setting: string,
    fb: string | undefined,
    bundled: string,
    workspaceFolder: string | undefined,
    environment: Environment,
): Found {
    const configured = fromSetting(setting, workspaceFolder, environment);
    if (configured !== undefined) {
        return configured;
    }

    const paths = pathModule(environment.platform);
    const beside =
        fb === undefined
            ? []
            : [
                  paths.join(paths.dirname(fb), "..", "lldb", "futureblitz.py"),
                  paths.join(paths.dirname(fb), "..", "..", "tools", "lldb", "futureblitz.py"),
              ];
    return firstExisting([...beside, bundled], environment);
}
