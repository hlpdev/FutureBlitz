// The TextMate grammar: vscode-tmgrammar-test's assertion files (test/grammar/*.fb) and snapshot
// files over real std and tests/programs code (test/grammar/snap/*.fb and their .snap files), a
// sweep over every .fb in the repo, and every keyword the lexer has.
//
// UPDATE_SNAPSHOTS=1 writes the snapshots again; read what changed before committing them.

import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { describe, test } from "node:test";

import * as textmate from "vscode-textmate";
import * as oniguruma from "vscode-oniguruma";
import { createRegistry } from "vscode-tmgrammar-test/dist/common/index";
import { getVSCodeTokens, renderSnap } from "vscode-tmgrammar-test/dist/snapshot/index";
import { parseGrammarTestCase, runGrammarTestCase } from "vscode-tmgrammar-test/dist/unit/index";

import { EXTENSION, REPO, fbFiles } from "./repo";

const SCOPE = "source.futureblitz";
const GRAMMAR = path.join(EXTENSION, "syntaxes", "futureblitz.tmLanguage.json");
const TESTS = path.join(EXTENSION, "test", "grammar");

const registry = createRegistry([{ scopeName: SCOPE, path: GRAMMAR }]);

// The grammar under vscode-textmate 9 and its own Oniguruma, as VS Code and Shiki load it.
async function loadGrammar(): Promise<textmate.IGrammar> {
    const wasm = fs.readFileSync(require.resolve("vscode-oniguruma/release/onig.wasm"));
    await oniguruma.loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));
    const own = new textmate.Registry({
        onigLib: Promise.resolve({
            createOnigScanner: (patterns: string[]) => new oniguruma.OnigScanner(patterns),
            createOnigString: (text: string) => new oniguruma.OnigString(text),
        }),
        loadGrammar: async () => textmate.parseRawGrammar(fs.readFileSync(GRAMMAR, "utf8"), GRAMMAR),
    });
    const grammar = await own.loadGrammar(SCOPE);
    assert.ok(grammar !== null);
    return grammar;
}

const grammar = loadGrammar();

function testFiles(folder: string): string[] {
    return fs
        .readdirSync(folder)
        .filter((name) => name.endsWith(".fb"))
        .map((name) => path.join(folder, name));
}

describe("the grammar's assertion files", () => {
    for (const file of testFiles(TESTS)) {
        test(path.basename(file), { timeout: 60000 }, async () => {
            const testCase = parseGrammarTestCase(fs.readFileSync(file, "utf8"));
            const failures = await runGrammarTestCase(registry, testCase);
            const shown = failures.map(
                (failure) =>
                    `line ${failure.srcLine + 1}, columns ${failure.start} to ${failure.end}: ` +
                    `missing [${failure.missing.join(" ")}], unexpected [${failure.unexpected.join(" ")}], ` +
                    `actual [${failure.actual.join(" ")}]\n    ${testCase.source[failure.srcLine]}`,
            );

            // vscode-tmgrammar-test excludes a scope only by its whole name, so `- keyword` would
            // never fail; here an excluded scope excludes the scopes under it too, as a theme's
            // selector does.
            const loaded = await grammar;
            let stack = textmate.INITIAL;
            const tokens = testCase.source.map((line) => {
                const result = loaded.tokenizeLine(line, stack);
                stack = result.ruleStack;
                return result.tokens;
            });
            for (const line of testCase.assertions) {
                for (const { from, to, exclude } of line.scopeAssertions) {
                    for (const token of tokens[line.sourceLineNumber]) {
                        if (token.endIndex <= from || token.startIndex >= to) {
                            continue;
                        }

                        const found = token.scopes.filter((scope) =>
                            exclude.some((excluded) => scope === excluded || scope.startsWith(`${excluded}.`)),
                        );
                        if (found.length > 0) {
                            shown.push(
                                `line ${line.sourceLineNumber + 1}, columns ${from} to ${to}: excluded ` +
                                    `[${found.join(" ")}]\n    ${testCase.source[line.sourceLineNumber]}`,
                            );
                        }
                    }
                }
            }

            assert.deepEqual(shown, []);
        });
    }
});

describe("the grammar's snapshots", () => {
    for (const file of testFiles(path.join(TESTS, "snap"))) {
        test(path.basename(file), { timeout: 60000 }, async () => {
            const tokens = await getVSCodeTokens(registry, SCOPE, fs.readFileSync(file, "utf8"));
            const rendered = renderSnap(tokens);
            const snapshot = `${file}.snap`;
            if (process.env.UPDATE_SNAPSHOTS === "1") {
                fs.writeFileSync(snapshot, rendered, "utf8");
                return;
            }

            assert.ok(fs.existsSync(snapshot), `${snapshot} is missing; run with UPDATE_SNAPSHOTS=1`);
            assert.equal(rendered, fs.readFileSync(snapshot, "utf8").replaceAll("\r\n", "\n"));
        });
    }
});

describe("the grammar over the repo's code", () => {
    // Valid code is never invalid.illegal, and nothing stays open at the end of a file, as a string or
    // a bracket the grammar didn't see closed would.
    test("every .fb in std, tests/programs, and tests/packages", async () => {
        const loaded = await grammar;
        const problems: string[] = [];
        let files = 0;
        for (const folder of ["std", path.join("tests", "programs"), path.join("tests", "packages")]) {
            for (const file of fbFiles(path.join(REPO, folder))) {
                ++files;
                let stack = textmate.INITIAL;
                let opened = 0;
                const lines = fs.readFileSync(file, "utf8").split(/\r\n|\r|\n/);
                lines.forEach((line, index) => {
                    const result = loaded.tokenizeLine(line, stack);
                    // A depth of 1 is the file's own scope, with nothing open in it.
                    if (result.ruleStack.depth > 1 && stack.depth <= 1) {
                        opened = index + 1;
                    }

                    for (const token of result.tokens) {
                        if (token.scopes.some((scope) => scope.startsWith("invalid"))) {
                            problems.push(`${file}:${index + 1}: invalid at ${token.startIndex}: ${line}`);
                        }
                    }

                    stack = result.ruleStack;
                });
                if (stack.depth > 1) {
                    problems.push(`${file}: still open at its end, from line ${opened}`);
                }
            }
        }

        assert.ok(files > 1000, `only ${files} files`);
        assert.deepEqual(problems, []);
    });

    // compiler/include/fb/token_kinds.def's keywords, each a keyword as it's written and none of
    // them in lower case, which is an identifier.
    test("every keyword the lexer has", async () => {
        const loaded = await grammar;
        const definitions = fs.readFileSync(
            path.join(REPO, "compiler", "include", "fb", "token_kinds.def"),
            "utf8",
        );
        const keywords = [...definitions.matchAll(/^FB_KEYWORD\(\w+, "(\w+)"\)/gm)].map((match) => match[1]);
        assert.ok(keywords.length > 60, `only ${keywords.length} keywords`);

        const keywordScope = /^(keyword|storage|constant\.language|variable\.language)\./;
        const scopesOf = (line: string, word: string): string[] => {
            const result = loaded.tokenizeLine(line, textmate.INITIAL);
            const at = line.lastIndexOf(word);
            const token = result.tokens.find((t) => t.startIndex <= at && at < t.endIndex);
            return token?.scopes ?? [];
        };

        const wrong: string[] = [];
        for (const keyword of keywords) {
            const scopes = scopesOf(`x = ${keyword}`, keyword);
            if (!scopes.some((scope) => keywordScope.test(scope))) {
                wrong.push(`${keyword}: [${scopes.join(" ")}]`);
            }

            const lower = keyword.toLowerCase();
            const lowerScopes = scopesOf(`x = ${lower}`, lower);
            if (lowerScopes.some((scope) => keywordScope.test(scope))) {
                wrong.push(`${lower}: [${lowerScopes.join(" ")}]`);
            }
        }

        assert.deepEqual(wrong, []);
    });
});
