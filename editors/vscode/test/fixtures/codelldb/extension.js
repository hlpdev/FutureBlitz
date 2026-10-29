// CodeLLDB's stand-in for the extension tests: an "lldb" debug adapter that records each launch's
// configuration, which the tests read from its exports, and ends the session.
"use strict";

const vscode = require("vscode");

const launches = [];

class Adapter {
    constructor() {
        this.emitter = new vscode.EventEmitter();
        this.onDidSendMessage = this.emitter.event;
        this.seq = 1;
    }

    handleMessage(message) {
        if (message.type !== "request") {
            return;
        }

        const respond = (body) =>
            this.emitter.fire({
                type: "response",
                seq: this.seq++,
                request_seq: message.seq,
                success: true,
                command: message.command,
                body,
            });
        if (message.command === "initialize") {
            respond({});
            this.emitter.fire({ type: "event", seq: this.seq++, event: "initialized" });
        } else if (message.command === "launch") {
            launches.push(message.arguments);
            respond();
            this.emitter.fire({ type: "event", seq: this.seq++, event: "terminated" });
        } else {
            respond();
        }
    }

    dispose() {
        this.emitter.dispose();
    }
}

exports.activate = (context) => {
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory("lldb", {
            createDebugAdapterDescriptor: () => new vscode.DebugAdapterInlineImplementation(new Adapter()),
        }),
    );
    return { launches };
};

exports.deactivate = () => {};
