# Penstock — Freshet's superoptimising compiler.
#
# The build is organised as:
#   cmake/LlvmBootstrap.cmake      backend toolchain discovery/download/smoke test
#   cmake/PenstockTargets.cmake    penstock, freshet-language-server, libfsh, rt_ir
#   cmake/PenstockOptPlugin.cmake  optional LLVM opt pass plugin (-O4 pipeline)
#   cmake/PenstockTests.cmake      check / bench / CTest wiring
#   cmake/PenstockInstall.cmake    install rules + CMake package export
#
# Typical use:
#   cmake -B build                          # configure (local LLVM or verified download)
#   cmake --build build                     # bin/penstock + runtime IR cache
#   cmake --build build --target check      # regression suite
#   cmake --build build --target bench      # AoS/SoA/C benchmarks (same backend)
#   ctest --test-dir build                  # the suite through CTest
#   cmake --install build                   # optional install (docs/install.md)
#
# Pin a backend (any LLVM 19-23 prefix with bin/clang + bin/clang++):
#   cmake -B build -DLLVM_ROOT=/path/to/llvm-19   # CMake variable (sticky)
#   LLVM_ROOT=/path/to/llvm-19 cmake -B build     # or the environment
#   cmake -B build -ULLVM_ROOT                    # unpin -> autodetection/download
#
# Penstock itself links no LLVM libraries (the backend clang is driven as a
# subprocess); the host C++ compiler defaults to the backend's clang++ and
# can be overridden via the usual CMAKE_CXX_COMPILER mechanisms.

cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------- LLVM bootstrap
# A clean checkout can bootstrap a complete official LLVM binary release.
# The Linux release archive contains clang/clang++, opt, the public LLVM
# headers, libraries and LLVMConfig.cmake (so the opt plugin can be built too).
option(PENSTOCK_FETCH_LLVM
  "Download an official LLVM toolchain/SDK when a suitable local one is unavailable" ON)
option(PENSTOCK_BUILD_OPT_PLUGIN
  "Build a loadable LLVM opt plugin exposing the Penstock -O4 IR pipeline" ON)
set(PENSTOCK_LLVM_VERSION "23.1.4" CACHE STRING
  "LLVM release to download when no local LLVM toolchain is found")
set(PENSTOCK_LLVM_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps"
  CACHE PATH "Directory used for downloaded LLVM releases")
option(PENSTOCK_FETCH_ZSTD
  "Fetch zstd when the selected LLVM SDK requires it and no system library is available" ON)
set(PENSTOCK_ZSTD_VERSION "1.5.7" CACHE STRING
  "zstd release to fetch when LLVM's exported targets require zstd")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LlvmBootstrap.cmake")

# -------------------------------------------------- project + host compiler
# Default the host C++ compiler to the resolved clang++ (Makefile parity);
# the user can override via the usual -DCMAKE_CXX_COMPILER=... mechanisms.
# Stage A / libfsh: now that the project enables the C language too
# (libfsh's add_library has .c sources), we also default CMAKE_C_COMPILER
# to the resolved clang so the runtime + libfsh build against the same
# toolchain the smoke test passed on.  CMake picks CC/cc from PATH
# otherwise, which may be a different (and possibly older) gcc.
if(NOT DEFINED CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER "${LLVM_ROOT}/bin/clang++")
endif()
if(NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "${LLVM_ROOT}/bin/clang")
endif()

# Stage A / libfsh (report §5): the historical project() enabled only
# CXX because the runtime .c file was compiled via a raw `clang -x c`
# shell command in add_custom_command.  libfsh's add_library(... STATIC
# .c) goes through CMake's compiler abstraction instead, so it needs
# the C language enabled here.  Keep the C standard explicit so the
# runtime headers (which use C99 features like stdbool/stdint) compile
# portably.
project(freshet VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING
      "Build type (Release recommended; penstock is a build tool)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C standard for libfsh + runtime (use C11; C99 would also work, but
# C11 is the modern baseline and is supported by every clang we target).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -------------------------------------------------------------- penstock
#
# ONE shared front-end source list (FRONTEND_SOURCES) feeds BOTH
# executables.  It exists because the hand-maintained per-target lists
# once drifted: generics.cpp was added to the compiler but not to the
# language server, and `cmake --build --target freshet-language-server`
# failed at link time with undefined references to
# freshet::ensurePreludeString / bindTypeEncoding / instantiateGenericFn.
# New front-end files go in FRONTEND_SOURCES and land in every target
# automatically; only the per-target drivers (main.cpp, evo.cpp,
# lsp/server.cpp) are added below it.
#
set(FRONTEND_SOURCES
  src/checks.cpp
  src/checks_parfor.cpp
  src/effects.cpp
  src/fuse_unroll.cpp
  src/irgen.cpp
  src/irgen_call.cpp
  src/irgen_expr.cpp
  src/irgen_parfor.cpp
  src/irgen_pgo.cpp
  src/irgen_stmt.cpp
  src/lane_analysis.cpp
  src/lexer.cpp
  src/opt_util.cpp
  src/ownership.cpp
  src/parser.cpp
  src/preproc.cpp
  src/report.cpp
  src/sema.cpp
  src/sema_expr.cpp
  src/soa_auto.cpp
  src/types.cpp
  src/session.cpp
  src/superopt.cpp
  src/precompute.cpp # finite-domain algorithm rewriting + Alive2 validation
  src/generics.cpp # Phase 2a: monomorphisation (report §4.2)
  src/modules.cpp # Phase 3a: module system — imports, visibility, IR symbols (§6.1/§6.2)
  src/fmt.cpp    # Phase 4: `penstock fmt` formatter (report §10.2)
  src/profit_model.cpp # optimisation-audit §8: shared profitability model
  src/loop_analysis.cpp # audit §6.1/§6.3/§6.10: typed const fold, ranges, parfor advisor
  src/loop_transforms.cpp) # audit §6.4–§6.9: LICM/partial-unroll/peel/interchange/tile/distribute/unroll-and-jam

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PenstockTargets.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PenstockOptPlugin.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PenstockTests.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PenstockInstall.cmake")

# ------------------------------------------------------------ configure log
message(STATUS "")
message(STATUS "  +----------------------------------------------------------+")
message(STATUS "  |              PENSTOCK CONFIGURATION COMPLETE             |")
message(STATUS "  +----------------------------------------------------------+")
message(STATUS "    Backend          : LLVM ${LLVM_FULL} (${LLVM_ROOT})")
message(STATUS "    Compiler check   : ${PENSTOCK_SMOKE_OK}")
message(STATUS "    Host C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "    Penstock binary  : ${CMAKE_CURRENT_SOURCE_DIR}/bin/penstock")
if(PENSTOCK_OPT_PLUGIN_BUILT)
  message(STATUS "    O4 opt plugin    : ${CMAKE_CURRENT_BINARY_DIR}/plugins/opt/PenstockOptPlugin.so")
else()
  message(STATUS "    O4 opt plugin    : not built")
endif()
message(STATUS "    Runtime IR cache : ${FRT_RT_CACHE}")
message(STATUS "")
message(STATUS "    Build            : cmake --build build")
message(STATUS "    Tests            : cmake --build build --target check")
message(STATUS "    Benchmarks       : cmake --build build --target bench")
message(STATUS "    Install          : cmake --install build")
message(STATUS "    Install prefix   : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  +----------------------------------------------------------+")
message(STATUS "")
