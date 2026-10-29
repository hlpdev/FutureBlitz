// A language server for the extension's tests, over stdio: it formats a document by putting
// "// formatted" at its top. Run with "exit", it stops as it starts, as fb_lsp's placeholder does,
// and with "crash", soon after.
"use strict";

if (process.argv[2] === "exit") {
    process.exit(0);
}

const documents = new Map();
let buffer = Buffer.alloc(0);

function send(message) {
    const body = Buffer.from(JSON.stringify({ jsonrpc: "2.0", ...message }), "utf8");
    process.stdout.write(`Content-Length: ${body.length}\r\n\r\n`);
    process.stdout.write(body);
}

function handle(message) {
    const { id, method, params } = message;
    switch (method) {
        case "initialize":
            send({
                id,
                result: {
                    capabilities: { textDocumentSync: 1, documentFormattingProvider: true },
                    serverInfo: { name: "fake-lsp" },
                },
            });
            break;
        case "initialized":
            // Run with "crash", it stops each time soon after it starts.
            if (process.argv[2] === "crash") {
                setTimeout(() => process.exit(3), 200);
            }
            break;
        case "textDocument/didOpen":
            documents.set(params.textDocument.uri, params.textDocument.text);
            break;
        case "textDocument/didChange":
            documents.set(params.textDocument.uri, params.contentChanges.at(-1).text);
            break;
        case "textDocument/formatting": {
            const text = documents.get(params.textDocument.uri) ?? "";
            send({
                id,
                result: text.startsWith("// formatted")
                    ? []
                    : [
                          {
                              range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } },
                              newText: "// formatted\n",
                          },
                      ],
            });
            break;
        }
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
