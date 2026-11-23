# Freshet Language Support for VS Code

This extension provides:

- `.fsht` file recognition and TextMate syntax highlighting;
- bracket matching, comment toggling and indentation defaults;
- diagnostics from Penstock's `freshet-language-server`;
- hover, document symbols, go to definition, find references and rename, as implemented by the current server;
- **Format Document / Format Selection** (see below), backed by the language server;
- semantic token colouring (see below) on top of the grammar.

Formatting runs the same token-preserving formatter as `penstock fmt` — it never changes what the program means, only how it is laid out.

## Formatting

The server implements `textDocument/formatting` and
`textDocument/rangeFormatting`, so VS Code's standard formatting surfaces are
enabled for Freshet files, exactly like the C/C++ extension:

- **Freshet: Format Document** (default keybinding `Shift+Alt+F`, menu:
  right-click → “Format Document”);
- **Freshet: Format Selection** (default keybinding `Ctrl+K Ctrl+F`, menu:
  right-click → “Format Selection” with a selection active).

The formatter honours the editor's `tabSize` / `insertSpaces` settings for
the open file. Formatting is token-level, so it also works while the
document has errors, and the server refuses (no edits) whenever the result
would change the token stream — the same safety gate as `penstock fmt`.
"Format Selection" snaps the selection outward to whole toplevel
declarations so it always formats self-contained code.

## Semantic colouring

The server implements `textDocument/semanticTokens/full`, overlaying the
TextMate grammar with reference-accurate colours. Themes with semantic-token
support (Dark+, Light+, Monokai et al) then colour:

| construct | token type |
| --- | --- |
| struct / enum / trait names (declaration + references, incl. annotations like `p: Point`, `-> String`) | `type` / `struct` / `enum` / `interface` |
| module and import names (`module geometry;`, `import geometry;`, the `geometry` of `geometry::distance`) | `namespace` |
| function declarations and resolved call sites (builtins like `print`, `sqrt` get the `defaultLibrary` modifier) | `function` |
| function arguments (declarations and uses) | `parameter` |
| `let` bindings and globals | `variable` |
| struct field names (declaration and access `p.x`) | `property` |
| enum variant names (declaration, construction, match arms) | `enumMember` |

The grammar still supplies keyword / string / number / comment colouring, and
module / import names are additionally coloured by the grammar so basic
colouring survives even when the server is not running.

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
code --install-extension penstock-project.freshet-language-support-0.1.2.vsix
```

The `Freshet: Restart Language Server` command restarts the server after changing its executable configuration.

## Current LSP feature set

The server currently provides full-document synchronisation, diagnostics,
hover, document symbols, definition, references, rename, completion,
inlay hints, document + range formatting ("Format Document" / "Format
Selection") and semantic tokens. Code actions and rename-with-preview are
not implemented yet; syntax highlighting is supplied by the TextMate grammar
and enriched by semantic tokens.

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
is version `0.1.2`.
