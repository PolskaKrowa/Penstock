" Freshet source files use the .fsht extension (renamed from .fsh, which
" clashes with GLSL fragment shader files that Vim maps to its built-in
" glslf filetype).  The explicit `set filetype=` (rather than
" :setfiletype) keeps detection unconditional.
augroup freshet_filetype_detection
  autocmd!
  autocmd BufRead,BufNewFile *.fsht set filetype=freshet
augroup END
