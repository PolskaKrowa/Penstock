# Freshet editor integrations

This directory contains editor support for the `.fsht` Freshet language
(the extension was renamed from `.fsh` to avoid the GLSL fragment-shader
clash in editor filetype detection):

- `vscode/` — installable VS Code extension source with TextMate highlighting and an LSP client for `freshet-language-server`.
- `vim/` — native Vim filetype detection and syntax highlighting, plus optional `vim-lsp` registration.

The LSP executable is built by the existing CMake target:

```sh
cmake --build build --target freshet-language-server --parallel
```

The current LSP feature set is diagnostics, hover, document symbols, go to definition, references and rename, completion, inlay hints, document + range formatting ("Format Document" / "Format Selection", the same token-preserving engine as `penstock fmt`) and semantic tokens (types, function arguments, locals, fields, enum variants, module/import names). The syntax definitions still provide baseline keyword/string/comment highlighting when the server is not running.

