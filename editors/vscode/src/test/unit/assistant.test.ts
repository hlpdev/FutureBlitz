// registerAssistant() over VS Code's APIs as each version has them, with a stand-in for the vscode
// module: one that takes an MCP server, 1.100's, whose MCP provider is a proposed API that throws,
// and one without either.

import assert from "node:assert/strict";
import Module from "node:module";
import { beforeEach, test } from "node:test";

interface Registered {
    providers: string[];
    tools: string[];
    contexts: unknown[][];
}

const registered: Registered = { providers: [], tools: [], contexts: [] };
const warnings: string[] = [];

class FakeDefinition {
    cwd: unknown;
    constructor(
        readonly label: string,
        readonly command: string,
        readonly args: string[],
    ) {}
}

// Each key is there from the start, as TypeScript's import takes only those the module has then.
const fake: Record<string, unknown> = {
    lm: undefined,
    McpStdioServerDefinition: undefined,
    EventEmitter: class {
        event = () => ({ dispose() {} });
        fire() {}
        dispose() {}
    },
    workspace: {
        workspaceFolders: [{ uri: { fsPath: "/work/game" } }],
        onDidChangeWorkspaceFolders: () => ({ dispose() {} }),
        onDidChangeConfiguration: () => ({ dispose() {} }),
    },
    commands: {
        executeCommand: (...args: unknown[]) => {
            registered.contexts.push(args);
            return Promise.resolve();
        },
    },
    LanguageModelToolResult: class {},
    LanguageModelTextPart: class {},
};

// `require("vscode")` in the extension's code gives the stand-in.
const loader = Module as unknown as { _resolveFilename: (request: string, ...rest: unknown[]) => string };
const resolve = loader._resolveFilename;
loader._resolveFilename = (request, ...rest) => (request === "vscode" ? "vscode" : resolve(request, ...rest));
require.cache.vscode = { id: "vscode", filename: "vscode", loaded: true, exports: fake } as unknown as NodeJS.Module;

// eslint-disable-next-line @typescript-eslint/no-require-imports
const { registerAssistant } = require("../../assistant") as typeof import("../../assistant");

const output = {
    info: () => {},
    warn: (line: string) => warnings.push(line),
} as unknown as Parameters<typeof registerAssistant>[2];
const locate = () => ({ path: "/bin/fb", tried: [] });

const provider = (id: string, given: { provideMcpServerDefinitions: () => FakeDefinition[] }) => {
    registered.providers.push(id);
    const [definition] = given.provideMcpServerDefinitions();
    assert.equal(definition.command, "/bin/fb");
    assert.deepEqual(definition.args, ["mcp", "/work/game"]);
    return { dispose() {} };
};
const tool = (name: string) => {
    registered.tools.push(name);
    return { dispose() {} };
};
const refused = () => {
    throw new Error("Extension 'futureblitz.futureblitz' CANNOT use API proposal: mcpConfigurationProvider.");
};

beforeEach(() => {
    registered.providers = [];
    registered.tools = [];
    registered.contexts = [];
    warnings.length = 0;
});

test("a VS Code that takes MCP servers gets fb mcp as one, and no tools", () => {
    fake.McpStdioServerDefinition = FakeDefinition;
    fake.lm = { registerMcpServerDefinitionProvider: provider, registerTool: tool };
    assert.equal(registerAssistant(locate, "1.0.0", output).length, 4);
    assert.deepEqual(registered.providers, ["futureblitz.mcp"]);
    assert.deepEqual(registered.tools, []);
});

// VS Code 1.100 has the provider, but as a proposed API, which throws for an extension.
test("a provider that throws gets the language-model tools instead", () => {
    fake.McpStdioServerDefinition = FakeDefinition;
    fake.lm = { registerMcpServerDefinitionProvider: refused, registerTool: tool };
    assert.equal(registerAssistant(locate, "1.0.0", output).length, 5);
    assert.deepEqual(registered.tools, ["futureblitz_searchDocs", "futureblitz_lookup", "futureblitz_check", "futureblitz_format"]);
    assert.deepEqual(registered.contexts, [["setContext", "futureblitz.languageModelTools", true]]);
    assert.equal(warnings.length, 1);
    assert.ok(warnings[0].includes("CANNOT use API proposal"));
});

test("neither, when both throw or neither's there, and nothing throws", () => {
    fake.lm = { registerMcpServerDefinitionProvider: refused, registerTool: refused };
    assert.deepEqual(registerAssistant(locate, "1.0.0", output), []);
    assert.equal(warnings.length, 2);
    assert.deepEqual(registered.contexts, []);

    fake.lm = undefined;
    fake.McpStdioServerDefinition = undefined;
    assert.deepEqual(registerAssistant(locate, "1.0.0", output), []);
});
