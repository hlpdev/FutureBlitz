// fb mcp for VS Code's assistants, as Copilot's agent mode, without setup: VS Code starts it as
// an MCP server where it can, and where it only has language-model tools, the extension's tools
// call an fb mcp of its own. Both APIs are newer than the VS Code the extension runs on, so it
// looks for them as it starts, and their types are the few it uses.

import * as vscode from "vscode";

import { LANGUAGE_MODEL_TOOLS_KEY, MCP_PROVIDER_ID, McpClient, TOOLS, mcpArguments } from "./mcp";
import { Found } from "./paths";

interface McpStdioServerDefinition {
    cwd?: vscode.Uri;
}

interface McpApi {
    McpStdioServerDefinition: new (
        label: string,
        command: string,
        args?: string[],
        env?: Record<string, string | number | null>,
        version?: string,
    ) => McpStdioServerDefinition;
    lm: {
        registerMcpServerDefinitionProvider?: (
            id: string,
            provider: {
                onDidChangeMcpServerDefinitions?: vscode.Event<void>;
                provideMcpServerDefinitions(token: vscode.CancellationToken): McpStdioServerDefinition[];
            },
        ) => vscode.Disposable;
        registerTool?: (
            name: string,
            tool: {
                invoke(options: { input: Record<string, unknown> }, token: vscode.CancellationToken): Promise<unknown>;
            },
        ) => vscode.Disposable;
    };
    LanguageModelToolResult: new (content: unknown[]) => unknown;
    LanguageModelTextPart: new (value: string) => unknown;
}

// Offers fb mcp to VS Code's assistants for the first workspace folder's package, with the fb that
// `locateFb` finds, and says which way in the output: as an MCP server where VS Code takes one, or
// else as language-model tools. A VS Code that has an API but refuses it, as 1.100 has the MCP
// provider as a proposed API that throws, gets the other way, or neither, and the extension goes
// on without them.
export function registerAssistant(
    locateFb: (folder: vscode.WorkspaceFolder | undefined) => Found,
    version: string,
    output: vscode.LogOutputChannel,
): vscode.Disposable[] {
    const api = vscode as unknown as Partial<McpApi>;
    for (const [way, offer] of [
        ["an MCP server", offerServer],
        ["language-model tools", offerTools],
    ] as const) {
        try {
            const offered = offer(api, locateFb, version, output);
            if (offered !== undefined) {
                output.info(`fb mcp is offered to VS Code's assistants as ${way}`);
                return offered;
            }
        } catch (error) {
            output.warn(`fb mcp can't be offered as ${way}: ${error instanceof Error ? error.message : String(error)}`);
        }
    }

    return [];
}

function firstFolder(): vscode.WorkspaceFolder | undefined {
    return vscode.workspace.workspaceFolders?.[0];
}

// fb mcp as an MCP server definition provider, or undefined where VS Code has none.
function offerServer(
    api: Partial<McpApi>,
    locateFb: (folder: vscode.WorkspaceFolder | undefined) => Found,
    version: string,
): vscode.Disposable[] | undefined {
    const register = api.lm?.registerMcpServerDefinitionProvider;
    const Definition = api.McpStdioServerDefinition;
    if (register === undefined || Definition === undefined) {
        return undefined;
    }

    const changed = new vscode.EventEmitter<void>();
    let provider: vscode.Disposable;
    try {
        provider = register(MCP_PROVIDER_ID, {
            onDidChangeMcpServerDefinitions: changed.event,
            provideMcpServerDefinitions: () => {
                const fb = locateFb(firstFolder()).path;
                if (fb === undefined) {
                    return [];
                }

                const root = firstFolder()?.uri;
                const definition = new Definition("FutureBlitz", fb, mcpArguments(root?.fsPath), {}, version);
                definition.cwd = root;
                return [definition];
            },
        });
    } catch (error) {
        changed.dispose();
        throw error;
    }

    return [
        changed,
        provider,
        vscode.workspace.onDidChangeWorkspaceFolders(() => changed.fire()),
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration("futureblitz.path")) {
                changed.fire();
            }
        }),
    ];
}

// fb mcp's tools as language-model tools, which call an fb mcp of the extension's own, or
// undefined where VS Code has none.
function offerTools(
    api: Partial<McpApi>,
    locateFb: (folder: vscode.WorkspaceFolder | undefined) => Found,
    _version: string,
    output: vscode.LogOutputChannel,
): vscode.Disposable[] | undefined {
    const registerTool = api.lm?.registerTool;
    const { LanguageModelToolResult, LanguageModelTextPart } = api;
    if (registerTool === undefined || LanguageModelToolResult === undefined || LanguageModelTextPart === undefined) {
        return undefined;
    }

    const client = new McpClient(
        () => {
            const fb = locateFb(firstFolder()).path;
            const root = firstFolder()?.uri.fsPath;
            return fb === undefined ? undefined : { command: fb, args: mcpArguments(root), cwd: root };
        },
        (line) => output.info(`fb mcp: ${line}`),
    );
    const disposables: vscode.Disposable[] = [{ dispose: () => client.dispose() }];
    try {
        for (const { name, tool } of TOOLS) {
            disposables.push(
                registerTool(name, {
                    invoke: async (options) => {
                        const result = await client.callTool(tool, options.input);
                        // An error a tool gives is the model's to act on, as VS Code has a tool throw it.
                        if (result.isError) {
                            throw new Error(result.text);
                        }

                        return new LanguageModelToolResult([new LanguageModelTextPart(result.text)]);
                    },
                }),
            );
        }
    } catch (error) {
        for (const disposable of disposables) {
            disposable.dispose();
        }

        throw error;
    }

    void vscode.commands.executeCommand("setContext", LANGUAGE_MODEL_TOOLS_KEY, true);
    return disposables;
}
