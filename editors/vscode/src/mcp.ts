// `fb mcp`, whose tools know FutureBlitz, for an assistant: what VS Code starts it with, and a
// client of its own for VS Code's language-model tools, without VS Code, so the unit tests can
// run them under plain Node.

import { ChildProcess, spawn } from "node:child_process";

// package.json's mcpServerDefinitionProviders id.
export const MCP_PROVIDER_ID = "futureblitz.mcp";

// The context key that shows the language-model tools, which the extension sets only where VS
// Code can't start fb mcp itself, so an assistant doesn't have each tool twice.
export const LANGUAGE_MODEL_TOOLS_KEY = "futureblitz.languageModelTools";

// fb's arguments for the server of the package in `folder`, or of the folder it runs in.
export function mcpArguments(folder: string | undefined): string[] {
    return folder === undefined ? ["mcp"] : ["mcp", folder];
}

// Each language-model tool, as package.json's languageModelTools names it, and fb mcp's tool it
// calls, with the input's properties they both take.
export const TOOLS = [
    { name: "futureblitz_searchDocs", tool: "search_docs", properties: ["query", "limit"] },
    { name: "futureblitz_lookup", tool: "lookup", properties: ["name"] },
    { name: "futureblitz_check", tool: "check", properties: ["code", "file"] },
    { name: "futureblitz_format", tool: "format", properties: ["code", "file", "width"] },
] as const;

// What a tool gave: its text, and whether it's an error.
export interface ToolResult {
    text: string;
    isError: boolean;
}

// How long a server gets to stop after its stdin ends, before it's killed.
const STOP_TIMEOUT = 2000;

interface Pending {
    resolve: (result: unknown) => void;
    reject: (error: Error) => void;
}

// A client of one fb mcp, which it starts at its first call and again at the next call after it
// stops, a message a line each way.
export class McpClient {
    private m_process: ChildProcess | undefined;
    private m_ready: Promise<void> | undefined;
    private m_buffer = "";
    private m_next = 1;
    private readonly m_pending = new Map<number, Pending>();

    constructor(
        private readonly m_command: () => { command: string; args: string[]; cwd?: string } | undefined,
        private readonly m_log: (line: string) => void = () => {},
    ) {}

    // Calls one of fb mcp's tools. Fails when fb can't be found or started, or stops.
    async callTool(name: string, args: Record<string, unknown>): Promise<ToolResult> {
        await this.start();
        const result = (await this.request("tools/call", { name, arguments: args })) as {
            content?: { type: string; text?: string }[];
            isError?: boolean;
        };
        const text = (result.content ?? [])
            .filter((part) => part.type === "text")
            .map((part) => part.text ?? "")
            .join("\n");
        return { text, isError: result.isError === true };
    }

    // fb mcp's tools, each with its input's schema.
    async listTools(): Promise<{ name: string; inputSchema: { properties?: Record<string, unknown> } }[]> {
        await this.start();
        const result = (await this.request("tools/list", {})) as {
            tools: { name: string; inputSchema: { properties?: Record<string, unknown> } }[];
        };
        return result.tools;
    }

    // Ends the server as the end of its stdin does, or after a while, as a kill does, and settles
    // when it's gone.
    stop(): Promise<void> {
        const child = this.m_process;
        this.stopped(new Error("the client was stopped"));
        if (child === undefined || child.exitCode !== null || child.signalCode !== null) {
            return Promise.resolve();
        }

        return new Promise((resolve) => {
            const timer = setTimeout(() => child.kill(), STOP_TIMEOUT);
            child.once("exit", () => {
                clearTimeout(timer);
                resolve();
            });
            child.stdin?.end();
        });
    }

    dispose(): void {
        void this.stop();
    }

    private start(): Promise<void> {
        if (this.m_ready !== undefined) {
            return this.m_ready;
        }

        const found = this.m_command();
        if (found === undefined) {
            return Promise.reject(new Error("FutureBlitz's fb wasn't found; set futureblitz.path"));
        }

        const child = spawn(found.command, found.args, { cwd: found.cwd, stdio: ["pipe", "pipe", "pipe"] });
        this.m_process = child;
        child.stdout?.setEncoding("utf8");
        child.stdout?.on("data", (data: string) => this.received(data));
        child.stderr?.setEncoding("utf8");
        child.stderr?.on("data", (data: string) => this.m_log(data.trimEnd()));
        child.on("error", (error) => this.ended(child, error));
        child.on("exit", (code) => this.ended(child, new Error(`fb mcp stopped, with exit code ${code}`)));
        this.m_ready = this.request("initialize", {
            protocolVersion: "2025-06-18",
            capabilities: {},
            clientInfo: { name: "futureblitz-vscode", version: "1" },
        }).then(() => this.send({ jsonrpc: "2.0", method: "notifications/initialized" }));
        return this.m_ready;
    }

    private request(method: string, params: unknown): Promise<unknown> {
        const id = this.m_next++;
        return new Promise((resolve, reject) => {
            this.m_pending.set(id, { resolve, reject });
            this.send({ jsonrpc: "2.0", id, method, params });
        });
    }

    private send(message: unknown): void {
        this.m_process?.stdin?.write(`${JSON.stringify(message)}\n`);
    }

    private received(data: string): void {
        this.m_buffer += data;
        for (let end = this.m_buffer.indexOf("\n"); end >= 0; end = this.m_buffer.indexOf("\n")) {
            const line = this.m_buffer.slice(0, end).trim();
            this.m_buffer = this.m_buffer.slice(end + 1);
            if (line === "") {
                continue;
            }

            let message: { id?: number; result?: unknown; error?: { message?: string } };
            try {
                message = JSON.parse(line);
            } catch {
                this.m_log(`fb mcp sent a line that isn't JSON: ${line}`);
                continue;
            }

            const pending = message.id === undefined ? undefined : this.m_pending.get(message.id);
            if (pending === undefined) {
                continue;
            }

            this.m_pending.delete(message.id as number);
            if (message.error !== undefined) {
                pending.reject(new Error(message.error.message ?? "fb mcp gave an error"));
            } else {
                pending.resolve(message.result);
            }
        }
    }

    // A process that stopped fails what waits on it, and the next call starts another.
    private ended(child: ChildProcess, error: Error): void {
        if (this.m_process === child) {
            this.stopped(error);
        }
    }

    private stopped(error: Error): void {
        this.m_process = undefined;
        this.m_ready = undefined;
        this.m_buffer = "";
        for (const pending of this.m_pending.values()) {
            pending.reject(error);
        }

        this.m_pending.clear();
    }
}
