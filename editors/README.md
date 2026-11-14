# Freshet editor integrations

This directory contains editor support for the `.fsh` Freshet language:

- `vscode/` — installable VS Code extension source with TextMate highlighting and an LSP client for `freshet-language-server`.
- `vim/` — native Vim filetype detection and syntax highlighting, plus optional `vim-lsp` registration.

The LSP executable is built by the existing CMake target:

```sh
cmake --build build --target freshet-language-server --parallel
```

The current LSP feature set is diagnostics, hover, document symbols, go to definition, references and rename. Completion and semantic-token support are not implemented, so both editors use syntax definitions for baseline syntax highlighting.

