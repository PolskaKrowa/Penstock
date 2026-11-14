" Vim syntax highlighting for the Freshet language.
if exists('b:current_syntax')
  finish
endif

syntax case match

" Comments are declared before strings so quoted comment markers remain text.
syntax keyword freshetTodo TODO FIXME XXX NOTE contained
syntax match freshetLineComment "//.*$" contains=freshetTodo
syntax region freshetBlockComment start="/\*" end="\*/" contains=freshetTodo

" Preprocessor directives supported by Penstock.
syntax match freshetPreprocessor /^\s*#\s*\zs\%(include\|define\|undef\|ifdef\|ifndef\|else\|endif\|link\|pragma\)\>/

" Strings and escapes.
syntax region freshetString start=+"+ skip=+\\.+ end=+"+ contains=freshetEscape
syntax match freshetEscape /\\[\\"nt0]/ contained

" Numeric literals, including the compiler's scalar suffixes.
syntax match freshetNumber /\<0[xX][0-9A-Fa-f]\+\%(u8\|u16\|u32\|u64\|usize\|i8\|i16\|i32\|i64\)\?\>/
syntax match freshetNumber /\<[0-9]\+\%([.][0-9]\+\)\?\%([eE][+-]\?[0-9]\+\)\?\%(u8\|u16\|u32\|u64\|usize\|i8\|i16\|i32\|i64\|f32\|f64\)\?\>/

" Attributes (for example @pure, @inline, @alias and @test).
syntax match freshetAttribute /@[A-Za-z_][A-Za-z0-9_]*/

" Declaration names and function call sites.
syntax match freshetFunctionCall /\<[A-Za-z_][A-Za-z0-9_]*\ze\s*(/
syntax match freshetFunctionDeclaration /\%(\<fn\>\s\+\)\@<=[A-Za-z_][A-Za-z0-9_]*/
syntax match freshetTypeDeclaration /\%(\<\%(struct\|enum\)\>\s\+\)\@<=[A-Za-z_][A-Za-z0-9_]*/

" Primitive and standard-library type names.
syntax keyword freshetType u8 u16 u32 u64 usize i8 i16 i32 i64 f32 f64 bool void str Span
syntax keyword freshetType String Option Result

" Language keywords (kept in sync with src/lexer.cpp).
syntax keyword freshetKeyword fn let mut return defer if else while for parfor in
syntax keyword freshetKeyword struct align true false as export extern give by step
syntax keyword freshetKeyword reduction threads chunk affinity collapse close spread
syntax keyword freshetKeyword soa sizeof alignof nil break continue enum match unsafe
syntax keyword freshetKeyword module import pub

syntax keyword freshetBoolean true false nil

" Operators and punctuation.
syntax match freshetOperator /[+*%<>=!&|^~?:-]/
syntax match freshetPunctuation /[{}()\[\],;.]/

highlight default link freshetTodo Todo
highlight default link freshetLineComment Comment
highlight default link freshetBlockComment Comment
highlight default link freshetPreprocessor PreProc
highlight default link freshetString String
highlight default link freshetEscape SpecialChar
highlight default link freshetNumber Number
highlight default link freshetAttribute PreProc
highlight default link freshetFunctionDeclaration Function
highlight default link freshetFunctionCall Function
highlight default link freshetTypeDeclaration Type
highlight default link freshetType Type
highlight default link freshetKeyword Statement
highlight default link freshetBoolean Boolean
highlight default link freshetOperator Operator
highlight default link freshetPunctuation Delimiter

let b:current_syntax = 'freshet'
