# Freshet support for Vim

The plugin provides native `.fsh` filetype detection, syntax highlighting and basic editing defaults. It works in Vim without an LSP plugin. Its filetype detector explicitly overrides Vim's built-in `.fsh` mapping, which is for a different language.

## Install

Copy the contents of this directory into your Vim runtime directory, preserving the directory layout. For a personal installation:

```sh
mkdir -p ~/.vim/pack/penstock/start/freshet
cp -R editors/vim/* ~/.vim/pack/penstock/start/freshet/
```

Restart Vim and open a `.fsh` file. Check `:set filetype?` — it should report `filetype=freshet`.

## Optional language-server features

Vim does not include an LSP client. Install [vim-lsp](https://github.com/prabirshrestha/vim-lsp) and ensure `freshet-language-server` is on `PATH`. The bundled plugin registers the server for Freshet buffers and maps `gd` (definition), `gr` (references), `<leader>rn` (rename), `K` (hover), and `[g`/`]g` (diagnostic navigation). These mappings apply when vim-lsp enables the buffer.

For a binary outside `PATH`, set its executable path in your `.vimrc` before the plugin loads:

```vim
let g:freshet_lsp_command = '/absolute/path/to/freshet-language-server'
```

To disable LSP auto-registration while keeping syntax highlighting, set:

```vim
let g:freshet_lsp_enabled = 0
```

The current language server does not implement completion or semantic tokens. Vim syntax colouring is handled by `syntax/freshet.vim`.
