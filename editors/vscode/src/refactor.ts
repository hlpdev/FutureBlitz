// Change Signature: fb-lsp's fb/changeSignature gives a Function's or Method's parameters as its
// declaration writes them, which the user changes in an input box, and then the edits of its
// declaration and of every call.

import * as vscode from "vscode";
import type { LanguageClient, WorkspaceEdit } from "vscode-languageclient/node";

export async function changeSignature(client: LanguageClient | undefined): Promise<void> {
    const editor = vscode.window.activeTextEditor;
    if (client === undefined || editor?.document.languageId !== "futureblitz") {
        return;
    }

    const at = {
        textDocument: { uri: editor.document.uri.toString() },
        position: client.code2ProtocolConverter.asPosition(editor.selection.active),
    };
    try {
        const current = await client.sendRequest<{ parameters: string }>("fb/changeSignature", at);
        const parameters = await vscode.window.showInputBox({
            title: "Change Signature",
            value: current.parameters,
            prompt:
                'Reorder or take out parameters; "new: Type <- old" renames one, and ' +
                '"name: Type = value" adds one, whose value each call passes',
        });
        if (parameters === undefined || parameters === current.parameters) {
            return;
        }

        const edit = await client.sendRequest<WorkspaceEdit>("fb/changeSignature", {
            ...at,
            parameters,
        });
        await vscode.workspace.applyEdit(await client.protocol2CodeConverter.asWorkspaceEdit(edit));
    } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        void vscode.window.showErrorMessage(`Change Signature: ${message}`);
    }
}
