// The extension tests' entry, which VS Code's extension host runs: each *.test.js here, in Mocha.

import * as fs from "node:fs";
import * as path from "node:path";

import Mocha = require("mocha");

export async function run(): Promise<void> {
    const mocha = new Mocha({ ui: "tdd", timeout: 120000, color: false });
    for (const file of fs.readdirSync(__dirname).filter((name) => name.endsWith(".test.js"))) {
        mocha.addFile(path.join(__dirname, file));
    }

    await new Promise<void>((resolve, reject) => {
        mocha.run((failures) => (failures > 0 ? reject(new Error(`${failures} tests failed`)) : resolve()));
    });
}
