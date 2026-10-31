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
// `locateFb` finds, and says which way in the output.
export function registerAssistant(
    locateFb: (folder: vscode.WorkspaceFolder | undefined) => Found,
    version: string,
    output: vscode.LogOutputChannel,
): vscode.Disposable[] {
    const api = vscode as unknown as Partial<McpApi>;
    const folder = (): vscode.WorkspaceFolder | undefined => vscode.workspace.workspaceFolders?.[0];
    if (api.lm?.registerMcpServerDefinitionProvider !== undefined && api.McpStdioServerDefinition !== undefined) {
        const Definition = api.McpStdioServerDefinition;
        const changed = new vscode.EventEmitter<void>();
        const provider = api.lm.registerMcpServerDefinitionProvider(MCP_PROVIDER_ID, {
            onDidChangeMcpServerDefinitions: changed.event,
            provideMcpServerDefinitions: () => {
                const fb = locateFb(folder()).path;
                if (fb === undefined) {
                    return [];
                }

                const root = folder()?.uri;
                const definition = new Definition("FutureBlitz", fb, mcpArguments(root?.fsPath), {}, version);
                definition.cwd = root;
                return [definition];
            },
        });
        output.info("fb mcp is offered to VS Code's assistants as an MCP server");
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

    const registerTool = api.lm?.registerTool;
    const { LanguageModelToolResult, LanguageModelTextPart } = api;
    if (registerTool === undefined || LanguageModelToolResult === undefined || LanguageModelTextPart === undefined) {
        return [];
    }

    const client = new McpClient(
        () => {
            const fb = locateFb(folder()).path;
            const root = folder()?.uri.fsPath;
            return fb === undefined ? undefined : { command: fb, args: mcpArguments(root), cwd: root };
        },
        (line) => output.info(`fb mcp: ${line}`),
    );
    const disposables: vscode.Disposable[] = [{ dispose: () => client.dispose() }];
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

    void vscode.commands.executeCommand("setContext", LANGUAGE_MODEL_TOOLS_KEY, true);
    output.info("fb mcp's tools are offered to VS Code's assistants as language-model tools");
    return disposables;
}
