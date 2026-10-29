// The language configuration's indentation rules, and the snippets, which fb has to read.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { describe, test } from "node:test";

import { EXTENSION, REPO, fbFiles, scratch, testFb } from "./repo";

const configuration = JSON.parse(fs.readFileSync(path.join(EXTENSION, "language-configuration.json"), "utf8"));
const increase = new RegExp(configuration.indentationRules.increaseIndentPattern);
const decrease = new RegExp(configuration.indentationRules.decreaseIndentPattern);

describe("indentation", () => {
    test("a block's first line indents the next", () => {
        for (const line of [
            "If hp <= 0",
            "    ElseIf hp < 20",
            "Else",
            "For i = 0 Until 10",
            "Parallel For i = 0 Until n",
            "For Method m In T Where m Has Get",
            "While running",
            "Repeat",
            "Loop",
            "Select state",
            "    Case 1 To 5",
            "    Default",
            "Try",
            "Catch e As FileError",
            "Finally",
            "Lock part.Entries As entries",
            "Unsafe",
            'Extern "C" Lib "joltc"',
            "Extend String",
            "@DebugElements Public Class Stack<T> Implements Iterable<T>",
            "Public Shared Class Hub",
            "Abstract Class Shape",
            "Struct Point",
            "Interface Shape",
            "Enum Color: UInt8",
            "Public Property Count: Int",
            "        Get",
            "        Set(value)",
            "Public Static Async Method Load(path: String): Task<String> Throws IOError",
            "Public Unsafe Function Copy(to: Ptr<UInt8>)",
            "Function Sum<T: Numeric>(xs: T[]): T",
            "    New(name: String)",
            "Operator +(a: Money, b: Money): Money",
            "Spawn(Function()",
            "Local double = items.Map(Function(x: Int): Int",
            "OnOpen = Function[Weak Self]()",
            // A Then or => in a comment or a string isn't the line's.
            "If ready // Then go",
            'If name == "Then"',
            "Function Foo() // => comment",
        ]) {
            assert.ok(increase.test(line), line);
        }
    });

    test("a line that's whole doesn't", () => {
        for (const line of [
            "If hp <= 0 Then Die()",
            "If hp <= 0 Then Die() Else Regen()",
            "Public Method Count(): Int => Items.Count",
            "Get => hp",
            "Operator ==(a: Money, b: Money): Bool => a.cents == b.cents",
            "Local f = Function(x) => x * 2",
            "Public Abstract Method Area(): Float64",
            "ReadOnly Property Health: Int",
            "End If",
            "End Function)",
            "Print(Format(value))",
            "Local formatted = Format(value)",
            "Return Try Load(path)",
            "Try Save(path)",
            "Default(x)",
            "Ifs += 1",
            // A call to a Method named Set, which an accessor's one parameter can't be told from.
            '        Set(key, "{value}")',
            "Set(a, b)",
            'Print("Function(")',
        ]) {
            assert.ok(!increase.test(line), line);
        }
    });

    test("a block's later parts and its end go back", () => {
        for (const line of ["End If", "    End Function)", "Else", "ElseIf x", "Case 2", "Default", "Catch e", "Finally", "Until done"]) {
            assert.ok(decrease.test(line), line);
        }

        for (const line of ["Ending = 1", "Cases.Add(x)", "Local x = 0 Until 10", "Defaults<T>()"]) {
            assert.ok(!decrease.test(line), line);
        }
    });

    test("a number is one word, as the lexer reads it", () => {
        const word = new RegExp(configuration.wordPattern, "g");
        for (const literal of ["0xFF_00", "1.5e-3", "0b1010", "0o755", "1_000", "x1"]) {
            assert.deepEqual(literal.match(word), [literal]);
        }

        assert.deepEqual("2.Seconds".match(word), ["2", "Seconds"]);
    });

    // VS Code's rules see a line at a time, so some lines miss: a Select's first Case, which goes a
    // block deeper than Select as Cases do, and an Interface's Methods without bodies. Over std and
    // the test programs, what the rules give a statement's first line, from the one before it, is
    // its indentation in all but a few.
    test("std and tests/programs as fb fmt lays them out", () => {
        let total = 0;
        let wrong = 0;
        for (const folder of ["std", path.join("tests", "programs")]) {
            for (const file of fbFiles(path.join(REPO, folder))) {
                let previous: { indent: number; text: string } | undefined;
                let inString = false;
                for (const line of fs.readFileSync(file, "utf8").split(/\r?\n/)) {
                    const trimmed = line.trim();
                    if ((line.match(/"""/g) ?? []).length % 2 === 1) {
                        inString = !inString;
                        previous = undefined;
                        continue;
                    }

                    if (inString || trimmed === "" || trimmed.startsWith("//") || trimmed.startsWith("#")) {
                        continue;
                    }

                    const spaces = /^ */.exec(line)?.[0].length ?? 0;
                    // Lines that go on line up with brackets, not blocks.
                    if (spaces % 4 !== 0) {
                        continue;
                    }

                    const indent = spaces / 4;
                    if (previous !== undefined && !/[,([{=+\-*/&|]\s*$/.test(previous.text)) {
                        const expected =
                            previous.indent + (increase.test(previous.text) ? 1 : 0) - (decrease.test(line) ? 1 : 0);
                        ++total;
                        wrong += expected === indent ? 0 : 1;
                    }

                    previous = { indent, text: line };
                }
            }
        }

        assert.ok(total > 50000, `only ${total} lines`);
        assert.ok(wrong / total < 0.025, `${wrong} of ${total} lines`);
    });
});

// The snippets, with their placeholders' defaults, each in a file of its own, read by fb fmt,
// which parses them as the compiler does. A Method, a Property, and an Operator go in a Class, and
// a lambda in a Local.
describe("snippets", () => {
    const snippets: Record<string, { prefix: string; body: string[] }> = JSON.parse(
        fs.readFileSync(path.join(EXTENSION, "snippets", "futureblitz.json"), "utf8"),
    );

    // A snippet's text, with each placeholder's first choice or default, a tab stop that repeats
    // one with what it holds, and tabs as 4 spaces.
    function expand(body: string[]): string {
        let text = body.join("\n").replaceAll("\t", "    ");
        const values = new Map<string, string>();
        let before = "";
        while (before !== text) {
            before = text;
            text = text
                .replace(/\$\{(\d+)\|([^,|}]*)[^}]*\|\}/g, (_, stop: string, value: string) => {
                    values.set(stop, value);
                    return value;
                })
                .replace(/\$\{(\d+):([^${}]*)\}/g, (_, stop: string, value: string) => {
                    values.set(stop, value);
                    return value;
                });
        }

        return text.replace(/\$\{\d+\}/g, "").replace(/\$(\d+)/g, (_, stop: string) => values.get(stop) ?? "");
    }

    test("expand as written", () => {
        assert.equal(expand(snippets["For To"].body), "For i = 0 Until count\n    \nEnd For");
        assert.equal(expand(snippets["Function"].body), "Function Name(): Int\n    \nEnd Function");
        assert.equal(expand(snippets["Operator"].body).split("\n")[0], "Operator +(a: Name, b: Name): Name");
    });

    const fb = testFb();

    // Its defaults compile in the type they're written in, as the description says they're for.
    test("the Operator's defaults compile in a Struct", { skip: fb === undefined ? "no fb: set FUTUREBLITZ_FB" : false }, () => {
        const folder = scratch("operator");
        const file = path.join(folder, "operator.fb");
        const operator = expand(snippets["Operator"].body).replace(/^/gm, "    ");
        fs.writeFileSync(file, `Import std.core\n\nStruct Name\n    Field V: Int\n\n${operator}\nEnd Struct\n`);
        const result = spawnSync(fb as string, ["check", "--color", "never", file], { encoding: "utf8" });
        fs.rmSync(folder, { recursive: true, force: true });
        assert.equal(result.status, 0, result.stdout + result.stderr);
    });
    test("fb reads each", { skip: fb === undefined ? "no fb: set FUTUREBLITZ_FB" : false }, () => {
        const folder = scratch("snippets");
        const inClass = new Set(["Method", "Property", "Operator"]);
        const failed: string[] = [];
        for (const [name, snippet] of Object.entries(snippets)) {
            let text = expand(snippet.body);
            if (inClass.has(name)) {
                text = `Class Holder\n${text.replace(/^/gm, "    ")}\nEnd Class`;
            } else if (snippet.prefix.startsWith("lambda")) {
                text = `Local f = ${text}`;
            }

            const file = path.join(folder, `${name.replace(/\W/g, "_")}.fb`);
            fs.writeFileSync(file, `Import std.core\n\n${text}\n`);
            const result = spawnSync(fb as string, ["fmt", "--check", "--color", "never", file], { encoding: "utf8" });
            // 0 is formatted and 1 isn't; either way, an error at a place is a parse error.
            if (result.status !== 0 && result.status !== 1) {
                failed.push(`${name}: exit ${result.status}: ${result.stderr}`);
            } else if (/\.fb:\d+:\d+: error/.test(result.stderr + result.stdout)) {
                failed.push(`${name}: ${result.stderr}${result.stdout}\n${text}`);
            }
        }

        fs.rmSync(folder, { recursive: true, force: true });
        assert.deepEqual(failed, []);
    });
});
