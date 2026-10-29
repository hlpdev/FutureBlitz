// The extension tests' entry, which VS Code's extension host runs: FB_TEST_SUITE's file, lone.test.js
// with no folder open, or each other *.test.js here, in Mocha.

import * as fs from "node:fs";
import * as path from "node:path";

import Mocha = require("mocha");

export async function run(): Promise<void> {
    const lone = process.env.FB_TEST_SUITE === "lone";
    const mocha = new Mocha({ ui: "tdd", timeout: 120000, color: false });
    for (const file of fs.readdirSync(__dirname).filter((name) => name.endsWith(".test.js"))) {
        if ((file === "lone.test.js") === lone) {
            mocha.addFile(path.join(__dirname, file));
        }
    }

    await new Promise<void>((resolve, reject) => {
        mocha.run((failures) => (failures > 0 ? reject(new Error(`${failures} tests failed`)) : resolve()));
    });
}
