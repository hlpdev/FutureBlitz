// An MCP server for the extension's tests, over stdio, a message a line, as fb mcp has it: its
// one tool, "echo", gives back its arguments' text, as an error when they say so, and "stop"
// ends the process before it answers.
//
// node fake-mcp.js [log]: with a log file, it writes "start <pid>" there.
"use strict";

const fs = require("fs");

const [log] = process.argv.slice(2);
if (log !== undefined) {
    fs.appendFileSync(log, `start ${process.pid}\n`);
}

let buffer = "";

function answer(id, result) {
    process.stdout.write(`${JSON.stringify({ jsonrpc: "2.0", id, result })}\n`);
}

process.stdin.setEncoding("utf8");
process.stdin.on("data", (data) => {
    buffer += data;
    for (let end = buffer.indexOf("\n"); end >= 0; end = buffer.indexOf("\n")) {
        const line = buffer.slice(0, end);
        buffer = buffer.slice(end + 1);
        const message = JSON.parse(line);
        if (message.id === undefined) {
            continue;
        }

        if (message.method === "initialize") {
            answer(message.id, { protocolVersion: "2025-06-18", capabilities: { tools: {} }, serverInfo: { name: "fake" } });
        } else if (message.method === "tools/call" && message.params.name === "stop") {
            process.exit(3);
        } else if (message.method === "tools/call" && message.params.name === "echo") {
            const args = message.params.arguments;
            // Two lines at once, the first split across writes, as a pipe may carry them.
            const text = JSON.stringify({
                jsonrpc: "2.0",
                id: message.id,
                result: { content: [{ type: "text", text: args.text }], isError: args.error === true },
            });
            process.stdout.write(text.slice(0, 10));
            setTimeout(() => process.stdout.write(`${text.slice(10)}\n\n`), 20);
        } else {
            process.stdout.write(
                `${JSON.stringify({ jsonrpc: "2.0", id: message.id, error: { code: -32601, message: `there's no method '${message.method}'` } })}\n`,
            );
        }
    }
});
