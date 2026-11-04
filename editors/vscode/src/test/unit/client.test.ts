// ServerClient, with a stand-in for the vscode module and vscode-languageclient: what it does as a
// server stops too often.

import assert from "node:assert/strict";
import Module from "node:module";
import { test } from "node:test";

import { Watchdog } from "../../watchdog";

const handler: ProxyHandler<object> = {
    get: (_target, key) => {
        if (key === "then") {
            return undefined;
        }

        // What it does with a notice's answer.
        if (key === "showWarningMessage") {
            return () => Promise.resolve(undefined);
        }

        return stand;
    },
    apply: () => stand,
    construct: () => stand,
    set: () => true,
};
// A function, so that it can be called and constructed.
const stand: object = new Proxy(function () {}, handler);

const CloseAction = { DoNotRestart: 1, Restart: 2 };
const loader = Module as unknown as {
    _resolveFilename: (request: string, ...rest: unknown[]) => string;
    _load: (request: string, ...rest: unknown[]) => unknown;
};
const resolve = loader._resolveFilename;
loader._resolveFilename = (request, ...rest) => (request === "vscode" ? "vscode" : resolve(request, ...rest));
const load = loader._load;
loader._load = (request, ...rest) => {
    if (request === "vscode") {
        return stand;
    }

    return request === "vscode-languageclient/node" ? { CloseAction } : load(request, ...rest);
};

// eslint-disable-next-line @typescript-eslint/no-require-imports
const { ServerClient } = require("../../client") as typeof import("../../client");

// What the test reaches of it: a client's own close, which only vscode-languageclient calls.
interface Inside {
    m_state: string;
    m_watchdog: Watchdog | undefined;
    closed(server: string): { action: number; handled: boolean };
}

function sleep(milliseconds: number): Promise<void> {
    return new Promise((done) => setTimeout(done, milliseconds));
}

test("the watchdog stops when the client gives up on a server that keeps stopping", async () => {
    const server = new ServerClient(stand as never, stand as never, () => stand as never);
    const inside = server as unknown as Inside;
    let asked = 0;
    let stuck = 0;
    const watchdog = new Watchdog({
        ping: () => {
            ++asked;
            return Promise.resolve();
        },
        every: 5,
        patience: 1000,
        stuck: () => ++stuck,
    });
    inside.m_state = "running";
    inside.m_watchdog = watchdog;
    try {
        // It's started again three times, the watchdog going on.
        for (let i = 0; i < 3; ++i) {
            assert.equal(inside.closed("fb-lsp").action, CloseAction.Restart);
            assert.equal(inside.m_watchdog, watchdog);
        }

        await sleep(40);
        assert.ok(asked > 0);

        // The fourth is the end of it.
        assert.equal(inside.closed("fb-lsp").action, CloseAction.DoNotRestart);
        assert.equal(server.state, "failed");
        assert.match(server.detail, /stopped 4 times/);
        assert.equal(inside.m_watchdog, undefined);

        const before = asked;
        await sleep(40);
        assert.equal(asked, before);
        assert.equal(stuck, 0);
    } finally {
        // Or a failure here would leave its timer keeping the test's process going.
        watchdog.stop();
    }
});
