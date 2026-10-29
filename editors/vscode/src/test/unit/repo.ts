// Where the tests find the extension, the repo it's in, fb, and lldb.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

import { currentEnvironment, executableName, findFb, findOnPath } from "../../paths";

// editors/vscode, from out/test/unit.
export const EXTENSION = path.resolve(__dirname, "..", "..", "..");
export const REPO = path.resolve(EXTENSION, "..", "..");

export function* fbFiles(folder: string): Generator<string> {
    for (const entry of fs.readdirSync(folder, { withFileTypes: true })) {
        const file = path.join(folder, entry.name);
        if (entry.isDirectory()) {
            yield* fbFiles(file);
        } else if (entry.name.endsWith(".fb")) {
            yield file;
        }
    }
}

// fb for the tests that run it: FUTUREBLITZ_FB, else where the extension looks. Undefined skips
// those tests.
export function testFb(): string | undefined {
    const configured = process.env.FUTUREBLITZ_FB;
    if (configured !== undefined && configured !== "") {
        return configured;
    }

    return findFb("", undefined, currentEnvironment()).path;
}

// lldb for the debugger's test: FUTUREBLITZ_LLDB, else lldb on PATH, else LLVM's on Windows.
export function testLldb(): string | undefined {
    const configured = process.env.FUTUREBLITZ_LLDB;
    if (configured !== undefined && configured !== "") {
        return configured;
    }

    const environment = currentEnvironment();
    const onPath = findOnPath("lldb", environment).path;
    if (onPath !== undefined) {
        return onPath;
    }

    const llvm = path.join("C:", "Program Files", "LLVM", "bin", executableName("lldb", process.platform));
    return process.platform === "win32" && environment.exists(llvm) ? llvm : undefined;
}

// A new, empty folder for one test's files.
export function scratch(name: string): string {
    return fs.mkdtempSync(path.join(os.tmpdir(), `futureblitz-${name}-`));
}
