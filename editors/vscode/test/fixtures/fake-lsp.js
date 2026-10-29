// A language server for the extension's tests, over stdio: it formats a document by putting
// "// formatted" at its top, each time it's asked, through textDocument/formatting, and through
// textDocument/willSaveWaitUntil when initializationOptions.formatOnSave is true, as fb-lsp does.
//
// node fake-lsp.js [mode] [log]: mode "exit" stops as it starts, as fb-lsp's placeholder did,
// "crash" soon after, and "hang" never answers. With a log file, it writes "start <pid>" there.
"use strict";

const fs = require("fs");

const [mode, log] = process.argv.slice(2);
if (log !== undefined) {
    fs.appendFileSync(log, `start ${process.pid}\n`);
}

if (mode === "exit") {
    process.exit(0);
}

const formatted = [
    {
        range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } },
        newText: "// formatted\n",
    },
];
let buffer = Buffer.alloc(0);

function send(message) {
    const body = Buffer.from(JSON.stringify({ jsonrpc: "2.0", ...message }), "utf8");
    process.stdout.write(`Content-Length: ${body.length}\r\n\r\n`);
    process.stdout.write(body);
}

function handle(message) {
    const { id, method, params } = message;
    if (mode === "hang") {
        return;
    }

    switch (method) {
        case "initialize": {
            const onSave = params.initializationOptions?.formatOnSave === true;
            send({
                id,
                result: {
                    capabilities: {
                        textDocumentSync: { openClose: true, change: 1, willSaveWaitUntil: onSave },
                        documentFormattingProvider: true,
                    },
                    serverInfo: { name: "fake-lsp" },
                },
            });
            break;
        }
        case "initialized":
            if (mode === "crash") {
                setTimeout(() => process.exit(3), 200);
            }
            break;
        case "textDocument/formatting":
        case "textDocument/willSaveWaitUntil":
            send({ id, result: formatted });
            break;
        case "shutdown":
            send({ id, result: null });
            break;
        case "exit":
            process.exit(0);
            break;
        default:
            if (id !== undefined) {
                send({ id, error: { code: -32601, message: `no ${method}` } });
            }
    }
}

process.stdin.on("data", (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    while (true) {
        const header = buffer.indexOf("\r\n\r\n");
        if (header < 0) {
            return;
        }

        const length = Number(/Content-Length: (\d+)/i.exec(buffer.subarray(0, header).toString("ascii"))[1]);
        const start = header + 4;
        if (buffer.length < start + length) {
            return;
        }

        const message = JSON.parse(buffer.subarray(start, start + length).toString("utf8"));
        buffer = buffer.subarray(start + length);
        handle(message);
    }
});
