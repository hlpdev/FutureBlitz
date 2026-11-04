// Noticing a language server that's stopped answering, and not waiting for it.

import assert from "node:assert/strict";
import { describe, test } from "node:test";

import { Watchdog, isSourceAction, withinTime } from "../../watchdog";

function sleep(milliseconds: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

describe("Watchdog", () => {
    test("a server that answers is left alone, an error being an answer", async () => {
        let asked = 0;
        let stuck = 0;
        const watchdog = new Watchdog({
            ping: () => (++asked % 2 === 0 ? Promise.resolve(null) : Promise.reject(new Error("no fb/ping"))),
            every: 5,
            patience: 50,
            stuck: () => ++stuck,
        });
        await sleep(150);
        watchdog.stop();
        assert.ok(asked > 3);
        assert.equal(stuck, 0);
    });

    test("one that doesn't answer is stuck, once", async () => {
        let asked = 0;
        let stuck = 0;
        const watchdog = new Watchdog({
            ping: () => {
                ++asked;
                return new Promise(() => undefined);
            },
            every: 5,
            patience: 20,
            stuck: () => ++stuck,
        });
        await sleep(150);
        watchdog.stop();
        assert.equal(asked, 1);
        assert.equal(stuck, 1);
    });

    test("ask doesn't wait for the next ping, and stop ends it", async () => {
        let stuck = 0;
        const watchdog = new Watchdog({
            ping: () => new Promise(() => undefined),
            every: 100000,
            patience: 10,
            stuck: () => ++stuck,
        });
        watchdog.ask();
        await sleep(60);
        assert.equal(stuck, 1);

        const stopped = new Watchdog({
            ping: () => new Promise(() => undefined),
            every: 1,
            patience: 10,
            stuck: () => ++stuck,
        });
        stopped.stop();
        await sleep(40);
        assert.equal(stuck, 1);
    });

    test("this process stalling isn't the server being stuck", async () => {
        let stuck = 0;
        const watchdog = new Watchdog({
            // The answer's there to read as soon as the stall's over, after the timers go off.
            ping: () =>
                new Promise((resolve) => {
                    setImmediate(resolve);
                    const until = Date.now() + 100;
                    while (Date.now() < until) {
                        // Blocked, as an extension host can be.
                    }
                }),
            every: 100000,
            patience: 20,
            stalled: 40,
            stuck: () => ++stuck,
        });
        watchdog.ask();
        await sleep(60);
        watchdog.stop();
        assert.equal(stuck, 0);
    });

    test("a client that can't send isn't a server that's stuck", async () => {
        let stuck = 0;
        const watchdog = new Watchdog({
            ping: () => {
                throw new Error("the client isn't running");
            },
            every: 5,
            patience: 10,
            stuck: () => ++stuck,
        });
        await sleep(60);
        watchdog.stop();
        assert.equal(stuck, 0);
    });
});

describe("isSourceAction", () => {
    test("is what editor.codeActionsOnSave asks for", () => {
        assert.ok(isSourceAction("source"));
        assert.ok(isSourceAction("source.organizeImports"));
        assert.ok(!isSourceAction("quickfix"));
        assert.ok(!isSourceAction("sourcerer"));
        assert.ok(!isSourceAction(undefined));
    });
});

describe("withinTime", () => {
    test("gives what's answered in time", async () => {
        let late = 0;
        assert.equal(await withinTime(Promise.resolve(3), 50, () => ++late), 3);
        assert.equal(await withinTime(4, 50, () => ++late), 4);
        await sleep(70);
        assert.equal(late, 0);
    });

    test("gives nothing once its time is up, and says so", async () => {
        let late = 0;
        assert.equal(await withinTime(new Promise(() => undefined), 10, () => ++late), undefined);
        assert.equal(late, 1);
    });

    test("a failure is still one", async () => {
        await assert.rejects(withinTime(Promise.reject(new Error("no")), 50, () => undefined), /no/);
    });
});
