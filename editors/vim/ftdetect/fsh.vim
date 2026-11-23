" Vim has a built-in .fsh mapping for another language; Freshet .fsh files
" must override it explicitly rather than using :setfiletype (which is
" conditional on the filetype still being empty).
augroup freshet_filetype_detection
  autocmd!
  autocmd BufRead,BufNewFile *.fsh set filetype=freshet
augroup END
