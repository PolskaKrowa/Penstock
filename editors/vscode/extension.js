'use strict';

const fs = require('node:fs');
const path = require('node:path');
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client;
let outputChannel;

function executableName() {
  return process.platform === 'win32'
    ? 'freshet-language-server.exe'
    : 'freshet-language-server';
}

function resolveServerCommand(config, workspaceFolder) {
  const configured = config.get('path', 'freshet-language-server');

  // A source checkout builds the server into <checkout>/bin. Prefer that
  // executable automatically when the editor is opened on the Penstock repo.
  if (configured === 'freshet-language-server' && workspaceFolder) {
    const localBinary = path.join(
      workspaceFolder.uri.fsPath,
      'bin',
      executableName()
    );
    if (fs.existsSync(localBinary)) {
      return localBinary;
    }
  }

  // Otherwise let the OS resolve a PATH command, or use the configured path.
  return configured;
}

async function startLanguageClient(context) {
  const config = vscode.workspace.getConfiguration('freshet.server');
  const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
  const command = resolveServerCommand(config, workspaceFolder);
  const args = config.get('args', []);
  const extraEnv = config.get('env', {});

  const options = {};
  if (workspaceFolder) {
    // The server currently resolves project-relative paths from its process
    // working directory, so use the first workspace folder as that directory.
    options.cwd = workspaceFolder.uri.fsPath;
  }
  options.env = { ...process.env, ...extraEnv };

  const serverOptions = {
    command,
    args,
    options
  };

  const clientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'freshet' }
    ],
    outputChannel
  };

  const languageClient = new LanguageClient(
    'freshetLanguageServer',
    'Freshet Language Server',
    serverOptions,
    clientOptions
  );
  client = languageClient;

  await languageClient.start();
  context.subscriptions.push({
    dispose: () => {
      void languageClient.stop();
    }
  });
}

async function restartLanguageClient(context) {
  if (client) {
    await client.stop();
    client = undefined;
  }
  await startLanguageClient(context);
}

async function activate(context) {
  outputChannel = vscode.window.createOutputChannel('Freshet Language Server');
  context.subscriptions.push(outputChannel);

  const restartCommand = vscode.commands.registerCommand(
    'freshet.restartLanguageServer',
    () => restartLanguageClient(context)
  );
  context.subscriptions.push(restartCommand);

  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (event.affectsConfiguration('freshet.server')) {
        void restartLanguageClient(context).catch((error) => {
          outputChannel.appendLine(`Failed to restart language server: ${error}`);
        });
      }
    })
  );

  await startLanguageClient(context);
}

async function deactivate() {
  if (client) {
    const activeClient = client;
    client = undefined;
    await activeClient.stop();
  }
}

module.exports = {
  activate,
  deactivate
};
