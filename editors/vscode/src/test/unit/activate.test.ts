// activate() with a stand-in for the vscode module, where each property and call gives another
// stand-in: it loads none of vscode-languageclient, which takes longer to load than the rest of
// the extension, and starts the server once the extension is active, but not once it's
// deactivated.

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

// `require("vscode")` in the extension's code gives the stand-in.
const loader = Module as unknown as { _resolveFilename: (request: string, ...rest: unknown[]) => string };
const resolve = loader._resolveFilename;
loader._resolveFilename = (request, ...rest) => (request === "vscode" ? "vscode" : resolve(request, ...rest));
require.cache.vscode = { id: "vscode", filename: "vscode", loaded: true, exports: stand } as unknown as NodeJS.Module;

// eslint-disable-next-line @typescript-eslint/no-require-imports
const { activate } = require("../../extension") as typeof import("../../extension");

const languageClient = () => Object.keys(require.cache).filter((file) => file.includes("vscode-languageclient"));

const contextOf = (subscriptions: { dispose(): unknown }[]) =>
    ({
        subscriptions,
        extension: { packageJSON: { version: "1.0.0" } },
        asAbsolutePath: (file: string) => file,
    }) as unknown as Parameters<typeof activate>[0];

test("activating loads none of vscode-languageclient, and the server starts after", async () => {
    const api = activate(contextOf([]));
    assert.deepEqual(languageClient(), []);
    assert.equal(api.server.detail, "");

    await new Promise((resolve) => setTimeout(resolve, 10));
    assert.equal(api.server.state, "disabled");
    assert.equal(api.server.detail, "turned off by futureblitz.lsp.enabled");
    assert.deepEqual(languageClient(), []);
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
