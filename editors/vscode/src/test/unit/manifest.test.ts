// package.json's launch schema and Marketplace fields, and the README that goes in the .vsix.

import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "node:test";

import { EXTENSION, REPO } from "./repo";

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

// The Marketplace and Open VSX list it by these, and vsce refuses a package without a license or a
// repository; the icon is the logo's tile, which the build copies in.
test("the Marketplace's fields are FutureBlitz's", () => {
    assert.equal(manifest.publisher, "futureblitz");
    assert.equal(manifest.name, "futureblitz");
    assert.equal(manifest.license, "Apache-2.0");
    assert.equal(manifest.repository.url, "https://github.com/hlpdev/FutureBlitz.git");
    assert.equal(manifest.icon, "images/icon.png");
    // The one sentence every package manager describes FutureBlitz with (tools/release).
    assert.ok(manifest.description.startsWith(
        "A compiled, general-purpose language with BASIC's readability and native speed. "));
    assert.deepEqual(fs.readFileSync(path.join(EXTENSION, manifest.icon)),
        fs.readFileSync(path.join(REPO, "assets", "brand", "mark-tile-128.png")));
    assert.ok(fs.existsSync(path.join(EXTENSION, "LICENSE")));
});

// vsce makes a relative link absolute against the repository's root, not the extension's folder,
// so one that leaves the folder breaks on the Marketplace.
test("the README's links don't leave the extension's folder", () => {
    assert.deepEqual([...readme.matchAll(/\]\((\.\.\/[^)]*)\)/g)].map((match) => match[1]), []);
});
