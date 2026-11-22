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
# front-end shared by the CLI and the language server (no main/evo/lsp).
# -Isrc: scripts/fecheck.cpp quotes "common.h" / "loop_analysis.h" etc.
# that live in src/ (the .cpp sources in src/ resolve their own includes
# relative to their file, but the scripts/ driver does not).
SHARED_SRCS="src/checks.cpp src/checks_parfor.cpp src/effects.cpp src/fuse_unroll.cpp src/irgen.cpp src/irgen_call.cpp src/irgen_expr.cpp src/irgen_parfor.cpp src/irgen_pgo.cpp src/irgen_stmt.cpp src/lane_analysis.cpp src/lexer.cpp src/opt_util.cpp src/ownership.cpp src/parser.cpp src/preproc.cpp src/report.cpp src/sema.cpp src/sema_expr.cpp src/soa_auto.cpp src/types.cpp src/session.cpp src/superopt.cpp src/precompute.cpp src/generics.cpp src/modules.cpp src/fmt.cpp src/profit_model.cpp src/loop_analysis.cpp src/loop_transforms.cpp src/mem_stage.cpp src/vector_plan.cpp src/bce.cpp src/dstk.cpp src/fp_reduction.cpp src/basecase.cpp src/traits.cpp src/consteval.cpp src/cheader.cpp"
mkdir -p bin
g++ -std=c++17 -O1 -Wall -Wno-unused-variable -Isrc -o bin/penstock $SHARED_SRCS src/evo.cpp src/main.cpp
g++ -std=c++17 -O1 -Isrc -o bin/freshet-language-server $SHARED_SRCS src/lsp/server.cpp
# fecheck: frontend-only driver (parse -> sema -> checks -> loop+mem passes
# -> IRgen) for environments with no LLVM 19-23 backend — validation and
# sandbox testing of the source-level passes without driving clang.
g++ -std=c++17 -O1 -Isrc -o bin/fecheck $SHARED_SRCS scripts/fecheck.cpp
echo "build OK: bin/penstock bin/freshet-language-server bin/fecheck"
