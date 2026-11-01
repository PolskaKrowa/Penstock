#!/usr/bin/env bash
# CMake-free fallback build for penstock + freshet-language-server (plain
# g++/clang++ -std=c++17; the project links no LLVM libraries). Handy on
# machines without a usable CMake or LLVM SDK.
#
# KEEP IN SYNC with CMakeLists.txt FRONTEND_SOURCES: every front-end
# .cpp must appear in BOTH lists below (the CMake build composes both
# targets from one shared list; this script spells them out).
set -e
cd "$(dirname "$0")/.."
# front-end shared by the CLI and the language server (no main/evo/lsp):
SHARED_SRCS="src/checks.cpp src/checks_parfor.cpp src/effects.cpp src/fuse_unroll.cpp src/irgen.cpp src/irgen_call.cpp src/irgen_expr.cpp src/irgen_parfor.cpp src/irgen_pgo.cpp src/irgen_stmt.cpp src/lane_analysis.cpp src/lexer.cpp src/opt_util.cpp src/ownership.cpp src/parser.cpp src/preproc.cpp src/report.cpp src/sema.cpp src/sema_expr.cpp src/soa_auto.cpp src/types.cpp src/session.cpp src/superopt.cpp src/precompute.cpp src/generics.cpp src/modules.cpp src/fmt.cpp src/profit_model.cpp src/loop_analysis.cpp src/loop_transforms.cpp"
mkdir -p bin
g++ -std=c++17 -O1 -Wall -Wno-unused-variable -o bin/penstock $SHARED_SRCS src/evo.cpp src/main.cpp
g++ -std=c++17 -O1 -o bin/freshet-language-server $SHARED_SRCS src/lsp/server.cpp
echo "build OK: bin/penstock bin/freshet-language-server"
