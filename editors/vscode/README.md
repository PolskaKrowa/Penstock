# Freshet Language Support for VS Code

This extension provides:

- `.fsh` file recognition and TextMate syntax highlighting;
- bracket matching, comment toggling and indentation defaults;
- diagnostics from Penstock's `freshet-language-server`;
- hover, document symbols, go to definition, find references and rename, as implemented by the current server.

Syntax colouring is provided by the editor grammar. The current LSP does not yet implement semantic tokens or completions, so this extension does not claim those features.

## Build the language server

From the Penstock source checkout:

```sh
cmake -S . -B build
cmake --build build --target freshet-language-server --parallel
```

The target places the executable in `bin/` at the checkout root. When you open that checkout as the VS Code workspace, the extension detects `bin/freshet-language-server` automatically.

For a normal Freshet project, either put the installed `freshet-language-server` on `PATH`, or configure its full path:

```json
{
  "freshet.server.path": "/absolute/path/to/Penstock/bin/freshet-language-server"
}
```

Additional command-line arguments and environment variables can be set with `freshet.server.args` and `freshet.server.env`. The server runs with the first workspace folder as its working directory, which is important for project-relative module and include resolution.

## Install for development

```sh
cd editors/vscode
npm install
```

Open `editors/vscode` in VS Code and press **F5** to launch an Extension Development Host. To create a VSIX package, install the declared dependencies and run:

```sh
npm run package
code --install-extension penstock-project.freshet-language-support-0.1.1.vsix
```

The `Freshet: Restart Language Server` command restarts the server after changing its executable configuration.

## Current LSP feature set

The server currently provides full-document synchronisation, diagnostics, hover, document symbols, definition, references and rename. Completions and semantic tokens are not implemented yet; syntax highlighting is therefore supplied locally by the TextMate grammar.

## Troubleshooting: “reading 'positionEncoding'” during startup

This error means the client did not receive `result.capabilities` from the
process it actually launched. The fixed source returns a correctly wrapped
`InitializeResult`. If the error persists, the most likely cause is that VS Code
is launching a different, older executable (for example one found on `PATH`, or
one in another Penstock checkout).

Build the server from this checkout (CMake places it in the checkout's `bin/`):

```sh
cmake -S . -B build
cmake --build build --target freshet-language-server --parallel
python3 scripts/lsp_nav_check.py "$PWD/bin/freshet-language-server"
```

Then explicitly point VS Code at that exact executable in workspace or user
settings:

```json
{
  "freshet.server.path": "/absolute/path/to/Penstock/bin/freshet-language-server"
}
```

This extension build logs the configured command, resolved executable path,
working directory and executable metadata to **View → Output → Freshet Language
Server**. Compare the logged path with the path passed to the smoke test above.
If they differ, VS Code is still launching the old binary. Run **Freshet: Restart
Language Server** or reload the VS Code window. This diagnostic extension build
is version `0.1.1`.
