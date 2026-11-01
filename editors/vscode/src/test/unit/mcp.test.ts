// fb mcp for assistants: package.json's tools and provider, the client the language-model tools
// call fb mcp with, over a fake server and the real fb.

import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { describe, test } from "node:test";

import { LANGUAGE_MODEL_TOOLS_KEY, MCP_PROVIDER_ID, McpClient, TOOLS, mcpArguments } from "../../mcp";
import { EXTENSION, scratch, testFb } from "./repo";

const manifest = JSON.parse(fs.readFileSync(path.join(EXTENSION, "package.json"), "utf8"));
const fakeServer = path.join(EXTENSION, "test", "fixtures", "fake-mcp.js");

function fake(log?: string): McpClient {
    return new McpClient(() => ({ command: process.execPath, args: log === undefined ? [fakeServer] : [fakeServer, log] }));
}

describe("package.json", () => {
    test("names the MCP provider the extension registers", () => {
        assert.deepEqual(manifest.contributes.mcpServerDefinitionProviders, [{ id: MCP_PROVIDER_ID, label: "FutureBlitz" }]);
    });

    // Only where VS Code can't start fb mcp itself, so an assistant hasn't each tool twice.
    test("has a language-model tool for each of fb mcp's, shown by the extension's context key", () => {
        const tools = manifest.contributes.languageModelTools;
        assert.deepEqual(
            tools.map((tool: { name: string }) => tool.name),
            TOOLS.map((tool) => tool.name),
        );
        for (const [i, tool] of tools.entries()) {
            assert.equal(tool.when, LANGUAGE_MODEL_TOOLS_KEY, tool.name);
            assert.equal(tool.inputSchema.type, "object", tool.name);
            assert.deepEqual(Object.keys(tool.inputSchema.properties), TOOLS[i].properties, tool.name);
            assert.ok(tool.modelDescription.length > 40, tool.name);
        }
    });
});

test("fb mcp runs for the workspace folder's package, or the one it starts in", () => {
    assert.deepEqual(mcpArguments("/work/game"), ["mcp", "/work/game"]);
    assert.deepEqual(mcpArguments(undefined), ["mcp"]);
});

describe("McpClient", () => {
    test("calls a tool, from lines that come in pieces", async () => {
        const client = fake();
        try {
            assert.deepEqual(await client.callTool("echo", { text: "one" }), { text: "one", isError: false });
            assert.deepEqual(await client.callTool("echo", { text: "two", error: true }), { text: "two", isError: true });
            await assert.rejects(client.listTools(), /there's no method 'tools\/list'/);
        } finally {
            await client.stop();
        }
    });

    test("starts the server again after it stops, failing the call it stopped on", async () => {
        const folder = scratch("mcp");
        const log = path.join(folder, "log");
        const client = fake(log);
        try {
            await client.callTool("echo", { text: "first" });
            await assert.rejects(client.callTool("stop", {}), /fb mcp stopped, with exit code 3/);
            assert.deepEqual(await client.callTool("echo", { text: "again" }), { text: "again", isError: false });
            assert.equal(fs.readFileSync(log, "utf8").split("\n").filter((line) => line.startsWith("start ")).length, 2);
        } finally {
            await client.stop();
            fs.rmSync(folder, { recursive: true, force: true });
        }
    });

    test("fails a call when there's no fb, or it can't start", async () => {
        await assert.rejects(new McpClient(() => undefined).callTool("check", {}), /fb wasn't found/);
        const missing = new McpClient(() => ({ command: path.join(scratch("none"), "fb-missing"), args: ["mcp"] }));
        await assert.rejects(missing.callTool("check", {}));
    });

    const fb = testFb();

    // The tools package.json describes are fb mcp's, with the same inputs.
    test("calls the real fb mcp's tools", { skip: fb === undefined ? "no fb: set FUTUREBLITZ_FB" : false }, async () => {
        const folder = scratch("mcp-fb");
        const client = new McpClient(() => ({ command: fb as string, args: mcpArguments(folder), cwd: folder }));
        try {
            const listed = await client.listTools();
            assert.deepEqual(
                listed.map((tool) => tool.name),
                TOOLS.map((tool) => tool.tool),
            );
            for (const [i, tool] of listed.entries()) {
                assert.deepEqual(Object.keys(tool.inputSchema.properties ?? {}).sort(), [...TOOLS[i].properties].sort());
            }

            const checked = await client.callTool("check", { code: "Import std.core\n\nPrint(\"hi\")\n" });
            assert.deepEqual(checked, { text: "No errors or warnings.", isError: false });
            const formatted = await client.callTool("format", { code: "Local   x=1\n" });
            assert.deepEqual(formatted, { text: "Local x = 1\n", isError: false });
        } finally {
            await client.stop();
            fs.rmSync(folder, { recursive: true, force: true });
        }
    });
});
