// Prints how the grammar scopes a .fb file, a token a line, for working on the grammar:
// node scripts/tokens.mjs <file.fb>
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const textmate = require("vscode-textmate");
const oniguruma = require("vscode-oniguruma");

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const wasm = readFileSync(require.resolve("vscode-oniguruma/release/onig.wasm"));
await oniguruma.loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));

const registry = new textmate.Registry({
    onigLib: Promise.resolve({
        createOnigScanner: (patterns) => new oniguruma.OnigScanner(patterns),
        createOnigString: (text) => new oniguruma.OnigString(text),
    }),
    loadGrammar: async () =>
        textmate.parseRawGrammar(
            readFileSync(join(root, "syntaxes", "futureblitz.tmLanguage.json"), "utf8"),
            "futureblitz.tmLanguage.json",
        ),
});

const grammar = await registry.loadGrammar("source.futureblitz");
let stack = textmate.INITIAL;
for (const line of readFileSync(process.argv[2], "utf8").split(/\r\n|\r|\n/)) {
    const result = grammar.tokenizeLine(line, stack);
    console.log(line);
    for (const token of result.tokens) {
        const text = line.slice(token.startIndex, token.endIndex);
        if (text.trim() !== "") {
            console.log(`    ${JSON.stringify(text)} ${token.scopes.slice(1).join(" ")}`);
        }
    }

    stack = result.ruleStack;
}
