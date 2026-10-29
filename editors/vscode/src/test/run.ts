// Runs the extension tests in VS Code, downloaded by @vscode/test-electron into .vscode-test/, with
// CodeLLDB's stand-in beside the extension: the workspace suite on a copy of
// test/fixtures/workspace, then the lone suite, with no folder open, on a copy of
// test/fixtures/lone.
//
// FUTUREBLITZ_FB names the fb the tests that build run with; without it they're skipped.
// FB_VSCODE_VERSION picks VS Code's version, the oldest package.json takes by default, and
// FB_VSCODE_EXECUTABLE runs a VS Code that's installed instead.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

import { runTests } from "@vscode/test-electron";

async function main(): Promise<void> {
    const extension = path.resolve(__dirname, "..", "..");
    const fixtures = path.join(extension, "test", "fixtures");
    const work = fs.mkdtempSync(path.join(os.tmpdir(), "futureblitz-vscode-"));
    for (const fixture of ["workspace", "lone"]) {
        fs.cpSync(path.join(fixtures, fixture), path.join(work, fixture), { recursive: true });
    }

    const manifest = JSON.parse(fs.readFileSync(path.join(extension, "package.json"), "utf8"));
    const oldest = String(manifest.engines.vscode).replace(/^\^/, "");
    const executable = process.env.FB_VSCODE_EXECUTABLE;
    const suites: { name: string; open: string[] }[] = [
        { name: "workspace", open: [path.join(work, "workspace", "fixtures.code-workspace")] },
        { name: "lone", open: [] },
    ];

    try {
        for (const suite of suites) {
            await runTests({
                ...(executable === undefined || executable === ""
                    ? { version: process.env.FB_VSCODE_VERSION ?? oldest }
                    : { vscodeExecutablePath: executable }),
                extensionDevelopmentPath: [extension, path.join(fixtures, "codelldb")],
                extensionTestsPath: path.join(__dirname, "suite", "index"),
                launchArgs: [
                    ...suite.open,
                    "--disable-extensions",
                    "--disable-workspace-trust",
                    `--user-data-dir=${path.join(work, `user-${suite.name}`)}`,
                ],
                extensionTestsEnv: {
                    FB_TEST_SUITE: suite.name,
                    FB_TEST_LONE: path.join(work, "lone", "hello.fb"),
                    FB_TEST_NODE: process.execPath,
                    FB_TEST_FAKE_LSP: path.join(fixtures, "fake-lsp.js"),
                    FUTUREBLITZ_FB: process.env.FUTUREBLITZ_FB ?? "",
                },
            });
        }
    } finally {
        fs.rmSync(work, { recursive: true, force: true });
    }
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
