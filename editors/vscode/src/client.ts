// The language server's client. The server may be missing, or stop: then the client says so, once,
// and the rest of the extension, the grammar, tasks, and debugging, goes on without it.

import * as vscode from "vscode";
import type { CloseAction, LanguageClient, LanguageClientOptions, ServerOptions } from "vscode-languageclient/node";

import { MANIFEST_SERVER, isManifest, serverAtLeast } from "./fb";
import { Found } from "./paths";
import { Watchdog, withinTime } from "./watchdog";

export type ServerState = "disabled" | "missing" | "starting" | "running" | "failed";

type ClientModule = typeof import("vscode-languageclient/node");
let loaded: ClientModule | undefined;

// vscode-languageclient, which takes longer to load than the rest of the extension, loaded when a
// server starts, so that activating the extension doesn't wait for it.
export function clientModule(): ClientModule {
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    loaded ??= require("vscode-languageclient/node") as ClientModule;
    return loaded;
}

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
    // The server start was given, which one that's stuck is started as again.
    private m_given: { command: string; args: string[] } | undefined;
    private m_watchdog: Watchdog | undefined;
    private readonly m_status: vscode.LanguageStatusItem;
    private readonly m_made = new vscode.EventEmitter<LanguageClient>();

    // Each client as it's made, before it starts, for the tests to watch its states from the first.
    readonly onDidMakeClient = this.m_made.event;

    // How long a server has to answer initialize before it's taken to have failed; the tests
    // shorten it.
    startTimeout = 20 * 1000;

    // How often a running server is asked if it's answering, how long it has before it's taken to
    // be stuck, and how long a save waits for its formatting, which takes milliseconds, before it
    // goes on without it. The tests shorten them.
    pingEvery = 10 * 1000;
    pingPatience = 15 * 1000;
    formatPatience = 2 * 1000;

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
    async start(server?: { command: string; args: string[] }, again = false): Promise<void> {
        const generation = ++this.m_generation;
        await this.disposeClient();
        if (generation !== this.m_generation) {
            return;
        }

        if (!again) {
            this.m_restarts = [];
        }

        this.m_given = server;
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
        const node = clientModule();
        this.m_output.appendLine(`Starting the language server: ${[command, ...args].join(" ")}`);
        const serverOptions: ServerOptions = {
            run: { command, args, transport: node.TransportKind.stdio },
            debug: { command, args, transport: node.TransportKind.stdio },
        };
        const clientOptions: LanguageClientOptions = {
            documentSelector: [
                { scheme: "file", language: "futureblitz" },
                { scheme: "untitled", language: "futureblitz" },
                // A package's project.toml, in whatever language an extension gives TOML.
                { scheme: "file", pattern: "**/project.toml" },
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
            // project.toml goes to fb-lsp only when it serves it: an older one would check it as
            // FutureBlitz code.
            middleware: {
                didOpen: (document, next) => (this.serves(document.uri) ? next(document) : Promise.resolve()),
                didChange: (event, next) => (this.serves(event.document.uri) ? next(event) : Promise.resolve()),
                didSave: (document, next) => (this.serves(document.uri) ? next(document) : Promise.resolve()),
                didClose: (document, next) => (this.serves(document.uri) ? next(document) : Promise.resolve()),
                provideCompletionItem: (document, position, context, token, next) =>
                    this.serves(document.uri) ? next(document, position, context, token) : undefined,
                provideHover: (document, position, token, next) =>
                    this.serves(document.uri) ? next(document, position, token) : undefined,
                provideCodeActions: (document, range, context, token, next) =>
                    this.serves(document.uri) ? next(document, range, context, token) : undefined,
                // A save waits for this, so it goes on without a server that isn't answering.
                provideDocumentFormattingEdits: (document, options, token, next) =>
                    withinTime(next(document, options, token), this.formatPatience, () => {
                        this.m_output.appendLine(
                            `The language server didn't format ${document.uri.fsPath} in ` +
                                `${this.formatPatience} ms; going on without it.`,
                        );
                        this.m_watchdog?.ask();
                    }),
            },
            outputChannel: this.m_output,
            revealOutputChannelOn: node.RevealOutputChannelOn.Never,
            initializationFailedHandler: (error) => {
                this.m_output.appendLine(`The language server failed to initialize: ${String(error)}`);
                return false;
            },
            errorHandler: {
                // A reader that's lost its place in the stream never finds it again, so every
                // answer after it would be lost, in silence, a save's formatting among them.
                error: () => {
                    if (this.m_state === "running") {
                        void this.stuck(client, command, "its connection broke");
                    }

                    return { action: node.ErrorAction.Continue, handled: true };
                },
                closed: () => this.closed(command),
            },
        };

        const client = new node.LanguageClient("futureblitz", "FutureBlitz", serverOptions, clientOptions);
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
            if (generation === this.m_generation && client.state === clientModule().State.Running) {
                this.setState("running", command);
                this.warnIfOld(client);
                // An older fb-lsp says it doesn't know fb/ping, which is an answer.
                this.m_watchdog = new Watchdog({
                    ping: () => client.sendRequest("fb/ping"),
                    every: this.pingEvery,
                    patience: this.pingPatience,
                    stuck: () => void this.stuck(client, command, "it stopped answering"),
                });
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

    // Whether a document goes to fb-lsp: every FutureBlitz file, and project.toml once fb-lsp
    // says it's a version that serves it.
    private serves(uri: vscode.Uri): boolean {
        if (!isManifest(uri.fsPath)) {
            return true;
        }

        return serverAtLeast(this.m_client?.initializeResult?.serverInfo?.version, MANIFEST_SERVER);
    }

    // Says once, in the output and a notification, when fb-lsp is older than what project.toml
    // and the newest editor features need, so the user updates fb.
    private warnIfOld(client: LanguageClient): void {
        const version = client.initializeResult?.serverInfo?.version;
        if (serverAtLeast(version, MANIFEST_SERVER)) {
            return;
        }

        const message =
            `fb-lsp ${version ?? "(an unknown version)"} is older than this extension needs: ` +
            `project.toml's support and the newest editor features need fb ${MANIFEST_SERVER} or later. ` +
            "Update fb (see futureblitz.dev/download).";
        this.m_output.appendLine(message);
        void vscode.window.showWarningMessage(message);
    }

    // Stops the client. vscode-languageclient says a client is running before it sets up its
    // features, its document listeners and formatting among them, after one more message; one
    // stopped in between sets them up anyway, stopped, so it fails the edits and saves it gets.
    // So a client that's still starting gets to finish first, for a while, and one that finishes
    // later than that, a slow server's, is stopped then, as it would otherwise stay.
    private async disposeClient(): Promise<void> {
        this.m_watchdog?.stop();
        this.m_watchdog = undefined;
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
                if (client.state === clientModule().State.Running) {
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
            return { action: clientModule().CloseAction.DoNotRestart, handled: true };
        }

        const now = Date.now();
        this.m_restarts = this.m_restarts.filter((at) => now - at < RESTART_WINDOW);
        if (this.m_restarts.length < MAX_RESTARTS) {
            this.m_restarts.push(now);
            this.m_output.appendLine("The language server stopped; starting it again.");
            return { action: clientModule().CloseAction.Restart, handled: true };
        }

        // The client stays, stopped, for the next start or stop to dispose of.
        this.reportFailed(server, `it stopped ${MAX_RESTARTS + 1} times in 3 minutes`);
        return { action: clientModule().CloseAction.DoNotRestart, handled: true };
    }

    // The server's running, and doesn't answer, or its messages can't be read: start it again and
    // say so, unless that's happened too often, as the client does for one that stops.
    private async stuck(client: LanguageClient, server: string, reason: string): Promise<void> {
        if (client !== this.m_client) {
            return;
        }

        const now = Date.now();
        this.m_restarts = this.m_restarts.filter((at) => now - at < RESTART_WINDOW);
        if (this.m_restarts.length >= MAX_RESTARTS) {
            ++this.m_generation;
            await this.disposeClient();
            this.reportFailed(server, `${reason}, ${MAX_RESTARTS + 1} times in 3 minutes`);
            return;
        }

        this.m_restarts.push(now);
        const message = `FutureBlitz: the language server was restarted, as ${reason}.`;
        this.m_output.appendLine(message);
        void vscode.window.showWarningMessage(message, "Show Output").then((choice) => {
            if (choice === "Show Output") {
                this.m_output.show(true);
            }
        });
        await this.start(this.m_given, true);
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
