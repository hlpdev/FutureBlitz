// package.json's launch schema and the README that goes in the .vsix.

import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "node:test";

import { EXTENSION } from "./repo";

const manifest = JSON.parse(fs.readFileSync(path.join(EXTENSION, "package.json"), "utf8"));
const readme = fs.readFileSync(path.join(EXTENSION, "README.md"), "utf8");

// VS Code warns about a launch.json property the schema doesn't list, so it lists CodeLLDB's that
// the README tells people to use and debug.ts passes on.
test("the launch schema has CodeLLDB's properties the README names", () => {
    const attributes = Object.keys(manifest.contributes.debuggers[0].configurationAttributes.launch.properties);
    for (const name of ["program", "args", "cwd", "env", "release", "stopOnEntry", "terminal", "initCommands", "preRunCommands"]) {
        assert.ok(attributes.includes(name), name);
    }

    const debugging = /- \*\*Debugging\*\*[\s\S]*?(?=\n## )/.exec(readme)?.[0] ?? "";
    for (const [, name] of debugging.matchAll(/`([a-z][A-Za-z]+)`/g)) {
        assert.ok(attributes.includes(name), name);
    }
});

// vsce makes a relative link absolute against the repository's root, not the extension's folder,
// so one that leaves the folder breaks on the Marketplace.
test("the README's links don't leave the extension's folder", () => {
    assert.deepEqual([...readme.matchAll(/\]\((\.\.\/[^)]*)\)/g)].map((match) => match[1]), []);
});
