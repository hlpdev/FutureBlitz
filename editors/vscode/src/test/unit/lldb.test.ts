// The debugger's launch as CodeLLDB runs it: a debug build, and lldb with the command the
// extension puts in initCommands, which loads fb's script, so a String shows as its text. CodeLLDB
// bundles its own lldb; this runs the one on this machine, and is skipped without it.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "node:test";

import { debugExecutable, lldbImportCommand } from "../../fb";
import { EXTENSION, scratch, testFb, testLldb } from "./repo";

const fb = testFb();
const lldb = testLldb();
const skip = fb === undefined ? "no fb: set FUTUREBLITZ_FB" : lldb === undefined ? "no lldb: set FUTUREBLITZ_LLDB" : false;

test("lldb loads fb's script with the extension's command and shows a String's text", { skip }, (t) => {
    // An lldb that can't run its Python can't load the script, as debugger_tests.cpp finds too.
    const probe = spawnSync(lldb as string, ["--batch", "-o", "script print('fb-python-' + str(6 * 7))"], {
        encoding: "utf8",
        timeout: 60000,
    });
    if (!(probe.stdout + probe.stderr).includes("fb-python-42")) {
        t.skip("this lldb can't run Python");
        return;
    }

    const folder = scratch("lldb");
    const source = path.join(folder, "main.fb");
    fs.writeFileSync(source, 'Import std.core\n\nLocal name = "Orc \\"big\\""\nPrint(name)\n');
    const executable = debugExecutable(source, undefined, process.platform);
    const build = spawnSync(fb as string, ["build", "--color", "never", "-o", executable, source], { encoding: "utf8" });
    assert.equal(build.status, 0, build.stdout + build.stderr);

    const script = path.join(EXTENSION, "lldb", "futureblitz.py");
    assert.ok(fs.existsSync(script), "the bundled script: npm run compile copies it");
    const result = spawnSync(
        lldb as string,
        [
            "--batch",
            "-o",
            lldbImportCommand(script),
            "-o",
            "breakpoint set --file main.fb --line 4",
            "-o",
            "run",
            "-o",
            "frame variable name",
            "-o",
            "kill",
            executable,
        ],
        { cwd: folder, encoding: "utf8", timeout: 60000 },
    );
    fs.rmSync(folder, { recursive: true, force: true });
    const output = result.stdout + result.stderr;
    assert.ok(!/error:.*futureblitz\.py/i.test(output), output);
    assert.match(output, /name = "Orc \\"big\\""/);
});
