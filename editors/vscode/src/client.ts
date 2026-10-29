// The language server's client. The server may be missing, or stop: then the client says so, once,
// and the rest of the extension, the grammar, tasks, and debugging, goes on without it.

import * as vscode from "vscode";
import {
    CloseAction,
    ErrorAction,
    LanguageClient,
    LanguageClientOptions,
    RevealOutputChannelOn,
    ServerOptions,
    State,
    TransportKind,
} from "vscode-languageclient/node";

import { Found } from "./paths";

export type ServerState = "disabled" | "missing" | "starting" | "running" | "failed";

// How many times a server that stopped is started again in RESTART_WINDOW before the client
// gives up on it.
const MAX_RESTARTS = 3;
const RESTART_WINDOW = 3 * 60 * 1000;

const MISSING_HIDDEN = "futureblitz.missingServerHidden";

export class ServerClient implements vscode.Disposable {
    private m_client: LanguageClient | undefined;
    private m_state: ServerState = "disabled";
    private m_detail = "";
    private m_restarts: number[] = [];
    private readonly m_status: vscode.LanguageStatusItem;

    constructor(
        private readonly m_context: vscode.ExtensionContext,
        private readonly m_output: vscode.LogOutputChannel,
        private readonly m_find: () => Found,
    ) {
        this.m_status = vscode.languages.createLanguageStatusItem("futureblitz.server", {
            language: "futureblitz",
        });
        this.m_status.name = "FutureBlitz Language Server";
        this.show();
    }

    get state(): ServerState {
        return this.m_state;
    }

    // The server's path, or what went wrong, for the status item and the tests.
    get detail(): string {
        return this.m_detail;
    }

    get client(): LanguageClient | undefined {
        return this.m_client;
    }

    // Starts the server the settings find, or, for the tests, `server` with its arguments.
    async start(server?: { command: string; args: string[] }): Promise<void> {
        await this.stop();
        this.m_restarts = [];
        if (server === undefined) {
            const settings = vscode.workspace.getConfiguration("futureblitz");
            if (!settings.get<boolean>("lsp.enabled", true)) {
                this.setState("disabled", "turned off by futureblitz.lsp.enabled");
                return;
            }

            const found = this.m_find();
            if (found.path === undefined) {
                this.reportMissing(found);
                return;
            }

            server = { command: found.path, args: [] };
        }

        const { command, args } = server;
        this.m_output.appendLine(`Starting the language server: ${[command, ...args].join(" ")}`);
        const serverOptions: ServerOptions = {
            run: { command, args, transport: TransportKind.stdio },
            debug: { command, args, transport: TransportKind.stdio },
        };
        const clientOptions: LanguageClientOptions = {
            documentSelector: [
                { scheme: "file", language: "futureblitz" },
                { scheme: "untitled", language: "futureblitz" },
            ],
            outputChannel: this.m_output,
            revealOutputChannelOn: RevealOutputChannelOn.Never,
            initializationFailedHandler: (error) => {
                this.m_output.appendLine(`The language server failed to initialize: ${String(error)}`);
                return false;
            },
            errorHandler: {
                error: () => ({ action: ErrorAction.Continue, handled: true }),
                closed: () => this.closed(command),
            },
        };

        const client = new LanguageClient("futureblitz", "FutureBlitz", serverOptions, clientOptions);
        this.m_client = client;
        this.setState("starting", command);
        try {
            await client.start();
            if (this.m_client === client && client.state === State.Running) {
                this.setState("running", command);
            }
        } catch (error) {
            if (this.m_client === client) {
                this.m_client = undefined;
                this.reportFailed(command, error instanceof Error ? error.message : String(error));
            }
        }
    }

    async stop(): Promise<void> {
        const client = this.m_client;
        this.m_client = undefined;
        if (client !== undefined) {
            try {
                await client.dispose(2000);
            } catch (error) {
                this.m_output.appendLine(`Stopping the language server: ${String(error)}`);
            }
        }
    }

    dispose(): void {
        void this.stop();
        this.m_status.dispose();
    }

    // The connection closed: start it again, unless it stopped too often or never started.
    private closed(server: string): { action: CloseAction; handled: boolean } {
        if (this.m_state !== "running") {
            return { action: CloseAction.DoNotRestart, handled: true };
        }

        const now = Date.now();
        this.m_restarts = this.m_restarts.filter((at) => now - at < RESTART_WINDOW);
        if (this.m_restarts.length < MAX_RESTARTS) {
            this.m_restarts.push(now);
            this.m_output.appendLine("The language server stopped; starting it again.");
            return { action: CloseAction.Restart, handled: true };
        }

        // The client stays, stopped, for the next start or stop to dispose of.
        this.reportFailed(server, `it stopped ${MAX_RESTARTS + 1} times in 3 minutes`);
        return { action: CloseAction.DoNotRestart, handled: true };
    }

    private reportMissing(found: Found): void {
        const where =
            found.missingSetting !== undefined
                ? `futureblitz.lspPath is ${found.missingSetting}, which isn't there`
                : "there's no fb_lsp beside fb or on PATH";
        this.setState("missing", where);
        this.m_output.appendLine(`No language server: ${where}. Looked in:`);
        for (const tried of found.tried) {
            this.m_output.appendLine(`    ${tried}`);
        }

        // A server that's missing because none was set up says so once, until it's asked for again;
        // one the setting names but isn't there always does.
        if (found.missingSetting === undefined && this.m_context.globalState.get<boolean>(MISSING_HIDDEN)) {
            return;
        }

        const message =
            `FutureBlitz: no language server, as ${where}. Highlighting, tasks, and debugging still ` +
            "work; errors as you type, hovers, and formatting wait for one.";
        void vscode.window
            .showInformationMessage(message, "Open Settings", "Don't Show Again")
            .then((choice) => {
                if (choice === "Open Settings") {
                    void vscode.commands.executeCommand("workbench.action.openSettings", "futureblitz.lspPath");
                } else if (choice === "Don't Show Again") {
                    void this.m_context.globalState.update(MISSING_HIDDEN, true);
                }
            });
    }

    private reportFailed(server: string, reason: string): void {
        this.setState("failed", `${server}: ${reason}`);
        this.m_output.appendLine(`The language server ${server} stopped: ${reason}`);
        const message =
            `FutureBlitz: the language server ${server} stopped (${reason}). Highlighting, tasks, ` +
            "and debugging still work.";
        void vscode.window
            .showWarningMessage(message, "Show Output", "Restart", "Open Settings")
            .then((choice) => {
                if (choice === "Show Output") {
                    this.m_output.show(true);
                } else if (choice === "Restart") {
                    void this.start();
                } else if (choice === "Open Settings") {
                    void vscode.commands.executeCommand("workbench.action.openSettings", "futureblitz.lspPath");
                }
            });
    }

    private setState(state: ServerState, detail: string): void {
        this.m_state = state;
        this.m_detail = detail;
        this.show();
    }

    private show(): void {
        const status = this.m_status;
        status.detail = this.m_detail;
        switch (this.m_state) {
            case "disabled":
                status.text = "Language server off";
                status.severity = vscode.LanguageStatusSeverity.Information;
                status.command = { title: "Settings", command: "workbench.action.openSettings", arguments: ["futureblitz.lsp"] };
                break;
            case "missing":
                status.text = "No language server";
                status.severity = vscode.LanguageStatusSeverity.Warning;
                status.command = { title: "Settings", command: "workbench.action.openSettings", arguments: ["futureblitz.lspPath"] };
                break;
            case "starting":
                status.text = "Language server starting";
                status.severity = vscode.LanguageStatusSeverity.Information;
                status.command = undefined;
                break;
            case "running":
                status.text = "Language server";
                status.severity = vscode.LanguageStatusSeverity.Information;
                status.command = { title: "Restart", command: "futureblitz.restartServer" };
                break;
            case "failed":
                status.text = "Language server stopped";
                status.severity = vscode.LanguageStatusSeverity.Error;
                status.command = { title: "Restart", command: "futureblitz.restartServer" };
                break;
        }
    }
}
