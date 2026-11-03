// activate() of the bundle that ships, dist/extension.js, with a stand-in for the vscode module,
// where each property and call gives another stand-in: it runs none of vscode-languageclient,
// which takes longer than the rest of the extension, until a server starts, and starts the server
// once the extension is active, but not once it's deactivated.

import assert from "node:assert/strict";
import Module from "node:module";
import { test } from "node:test";

const handler: ProxyHandler<object> = {
    get: (_target, key) => {
        if (key === "then") {
            return undefined;
        }

        if (key === Symbol.toPrimitive) {
            return () => "";
        }

        // A setting's default, but for the server, which stays off.
        if (key === "get") {
            return (name: string, fallback: unknown) => (name === "lsp.enabled" ? false : fallback);
        }

        return key === "workspaceFolders" ? [] : stand;
    },
    apply: () => stand,
    construct: () => stand,
    set: () => true,
};
// A function, so that it can be called and constructed.
const stand: object = new Proxy(function () {}, handler);

// A bundle copies the module's own properties, so the stand-in has the names the extension uses.
const fake = Object.fromEntries(
    [
        "commands",
        "CustomExecution",
        "debug",
        "DebugConfigurationProviderTriggerKind",
        "EventEmitter",
        "extensions",
        "languages",
        "LanguageStatusSeverity",
        "Location",
        "Position",
        "ProcessExecution",
        "Range",
        "Task",
        "TaskGroup",
        "TaskScope",
        "tasks",
        "TestMessage",
        "TestRunProfileKind",
        "TestRunRequest",
        "tests",
        "Uri",
        "window",
        "workspace",
    ].map((name) => [name, stand]),
);

// `require("vscode")` in the bundle gives the stand-in, and what it asks for is noted: the bundle
// has vscode-languageclient's code, which asks for "readline" as it runs, and nothing else does.
const asked: string[] = [];
const loader = Module as unknown as {
    _resolveFilename: (request: string, ...rest: unknown[]) => string;
    _load: (request: string, ...rest: unknown[]) => unknown;
};
const resolve = loader._resolveFilename;
loader._resolveFilename = (request, ...rest) => (request === "vscode" ? "vscode" : resolve(request, ...rest));
const load = loader._load;
loader._load = (request, ...rest) => {
    asked.push(request);
    return load(request, ...rest);
};
require.cache.vscode = { id: "vscode", filename: "vscode", loaded: true, exports: fake } as unknown as NodeJS.Module;

// eslint-disable-next-line @typescript-eslint/no-require-imports
const { activate } = require("../../../dist/extension.js") as typeof import("../../extension");

const clientRan = () => asked.includes("readline");

const contextOf = (subscriptions: { dispose(): unknown }[]) =>
    ({
        subscriptions,
        extension: { packageJSON: { version: "1.0.0" } },
        asAbsolutePath: (file: string) => file,
    }) as unknown as Parameters<typeof activate>[0];

test("activating runs none of vscode-languageclient, and the server starts after", async () => {
    const api = activate(contextOf([]));
    assert.equal(clientRan(), false);
    assert.equal(api.server.detail, "");

    await new Promise((resolve) => setTimeout(resolve, 10));
    assert.equal(api.server.state, "disabled");
    assert.equal(api.server.detail, "turned off by futureblitz.lsp.enabled");
    assert.equal(clientRan(), false);
});

test("the bundle has vscode-languageclient, which runs when a server starts", async () => {
    const api = activate(contextOf([]));
    // Its code stops at a class of VS Code's the stand-in doesn't have: what counts is that it's
    // there, and runs now.
    await api.server.start({ command: "no-such-fb-lsp", args: [] }).catch(() => {});
    assert.equal(clientRan(), true);
});

test("an extension deactivated before its server starts doesn't start it", async () => {
    const subscriptions: { dispose(): unknown }[] = [];
    const api = activate(contextOf(subscriptions));
    for (const subscription of subscriptions) {
        subscription.dispose();
    }

    await new Promise((resolve) => setTimeout(resolve, 10));
    assert.equal(api.server.detail, "");
});
