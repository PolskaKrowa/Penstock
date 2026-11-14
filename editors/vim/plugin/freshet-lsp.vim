" Optional vim-lsp integration. Native syntax highlighting does not need vim-lsp.
" Install prabirshrestha/vim-lsp to enable diagnostics and navigation.

if !exists('g:freshet_lsp_enabled')
  let g:freshet_lsp_enabled = 1
endif
if !exists('g:freshet_lsp_command')
  let g:freshet_lsp_command = 'freshet-language-server'
endif

if g:freshet_lsp_enabled && executable(g:freshet_lsp_command)
  augroup freshet_vim_lsp
    autocmd!
    autocmd User lsp_setup call lsp#register_server({
          \ 'name': 'freshet-language-server',
          \ 'cmd': {server_info -> [get(g:, 'freshet_lsp_command', 'freshet-language-server')]},
          \ 'allowlist': ['freshet'],
          \ 'languageId': {server_info -> 'freshet'}
          \ })
    autocmd User lsp_buffer_enabled call s:ConfigureFreshetLspBuffer()
  augroup END
endif

function! s:ConfigureFreshetLspBuffer() abort
  if &l:filetype !=# 'freshet'
    return
  endif
  setlocal signcolumn=yes
  nmap <buffer> gd <plug>(lsp-definition)
  nmap <buffer> gr <plug>(lsp-references)
  nmap <buffer> <leader>rn <plug>(lsp-rename)
  nmap <buffer> K <plug>(lsp-hover)
  nmap <buffer> [g <plug>(lsp-previous-diagnostic)
  nmap <buffer> ]g <plug>(lsp-next-diagnostic)
endfunction
