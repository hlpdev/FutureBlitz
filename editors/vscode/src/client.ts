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

// The inlay hints fb-lsp gives, as futureblitz.inlayHints says.
export function inlayHints(settings: vscode.WorkspaceConfiguration): { types: boolean; parameterNames: boolean } {
    return {
        types: settings.get<boolean>("inlayHints.types", true),
        parameterNames: settings.get<boolean>("inlayHints.parameterNames", true),
    };
}

// How many times a server that stopped is started again in RESTART_WINDOW before the client
// gives up on it.
const MAX_RESTARTS = 3;
const RESTART_WINDOW = 3 * 60 * 1000;

const MISSING_HIDDEN = "futureblitz.missingServerHidden";

// How long a client that's still starting gets to finish before it's stopped anyway.
const START_SETTLE = 2000;

export class ServerClient implements vscode.Disposable {
    private m_client: LanguageClient | undefined;
    // The client's start, which settles when it's running with its features or has failed.
    private m_started: Promise<void> | undefined;
    private m_state: ServerState = "disabled";
    private m_detail = "";
    private m_restarts: number[] = [];
    // Each start and stop takes the next one, so one that's superseded can tell.
    private m_generation = 0;
    private readonly m_status: vscode.LanguageStatusItem;
    private readonly m_made = new vscode.EventEmitter<LanguageClient>();

    // Each client as it's made, before it starts, for the tests to watch its states from the first.
    readonly onDidMakeClient = this.m_made.event;

    // How long a server has to answer initialize before it's taken to have failed; the tests
    // shorten it.
    startTimeout = 20 * 1000;

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

    // Starts the server the settings find, or, for the tests, `server` with its arguments. A start
    // or stop that comes while one is under way supersedes it: the earlier one's client is
    // disposed of, and it changes nothing once it goes on.
    async start(server?: { command: string; args: string[] }): Promise<void> {
        const generation = ++this.m_generation;
        await this.disposeClient();
        if (generation !== this.m_generation) {
            return;
        }

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
            // Saving formats through the editor's own editor.formatOnSave, which [futureblitz]
            // turns on and which asks textDocument/formatting; fb-lsp's willSaveWaitUntil, which
            // this would turn on, would format each save a second time. fb-lsp sends each file's
            // tests for the Testing panel, and the inlay hints the settings ask for, which it's
            // told again as they change.
            initializationOptions: {
                formatOnSave: false,
                tests: true,
                inlayHints: inlayHints(vscode.workspace.getConfiguration("futureblitz")),
            },
            synchronize: { configurationSection: "futureblitz" },
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
        this.m_made.fire(client);
        this.setState("starting", command);
        // vscode-languageclient waits for initialize's answer for as long as it takes.
        let timer: NodeJS.Timeout | undefined;
        const seconds = Math.round(this.startTimeout / 1000);
        const timeout = new Promise<never>((_, reject) => {
            timer = setTimeout(() => reject(new Error(`it didn't answer in ${seconds} seconds`)), this.startTimeout);
        });
        try {
            const started = client.start();
            this.m_started = started.then(
                () => undefined,
                () => undefined,
            );
            await Promise.race([started, timeout]);
            if (generation === this.m_generation && client.state === State.Running) {
                this.setState("running", command);
            }
        } catch (error) {
            if (generation === this.m_generation) {
                await this.disposeClient();
                this.reportFailed(command, error instanceof Error ? error.message : String(error));
            }
        } finally {
            clearTimeout(timer);
        }
    }

    async stop(): Promise<void> {
        ++this.m_generation;
        await this.disposeClient();
    }

    // Stops the client. vscode-languageclient says a client is running before it sets up its
    // features, its document listeners and formatting among them, after one more message; one
    // stopped in between sets them up anyway, stopped, so it fails the edits and saves it gets.
    // So a client that's still starting gets to finish first, for a while, and one that finishes
    // later than that, a slow server's, is stopped then, as it would otherwise stay.
    private async disposeClient(): Promise<void> {
        const client = this.m_client;
        const started = this.m_started;
        this.m_client = undefined;
        this.m_started = undefined;
        if (client === undefined) {
            return;
        }

        let settled = started === undefined;
        if (started !== undefined) {
            let timer: NodeJS.Timeout | undefined;
            const waited = new Promise<void>((resolve) => {
                timer = setTimeout(resolve, START_SETTLE);
            });
            settled = await Promise.race([started.then(() => true), waited.then(() => false)]);
            clearTimeout(timer);
        }

        await this.stopClient(client);
        if (!settled && started !== undefined) {
            void started.then(async () => {
                if (client.state === State.Running) {
                    await this.stopClient(client);
                }
            });
        }
    }

    private async stopClient(client: LanguageClient): Promise<void> {
        try {
            await client.dispose(2000);
        } catch (error) {
            this.m_output.appendLine(`Stopping the language server: ${String(error)}`);
        }
    }

    dispose(): void {
        void this.stop();
        this.m_status.dispose();
        this.m_made.dispose();
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
                : "there's no fb-lsp beside fb or on PATH";
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
                status.command = { title: "Restart", command: "futureblitz.restartServer" };
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
