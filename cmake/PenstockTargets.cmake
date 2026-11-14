# ------------------------------------------------------------ build targets
# penstock (compiler CLI), freshet-language-server (LSP over stdio),
# libfsh (public C API static library) and the rt_ir runtime-IR cache
# prewarm.  All targets share the FRONTEND_SOURCES list defined in the
# top-level CMakeLists.txt: one list, every target — the language server
# can never again miss a file the compiler gained.  Build-tree binaries
# land in <source root>/bin and scratch/IR under <source root>/build;
# install rules live in cmake/PenstockInstall.cmake.
set(PENSTOCK_SOURCES
  ${FRONTEND_SOURCES}
  src/evo.cpp    # SAT-guided evo search — driven from the CLI only
  src/main.cpp)

# Stage A0: a deliberately non-superoptimising compiler.  CMake uses this
# binary to compile src/superopt.fsh into an object before the final compiler
# is linked.  It has the same frontend/runtime paths as the final compiler,
# but no reference to the native Z3/Alive2 bridge or the Freshet optimiser.
add_executable(penstock-bootstrap EXCLUDE_FROM_ALL ${PENSTOCK_SOURCES})
target_include_directories(penstock-bootstrap PRIVATE src)
target_compile_options(penstock-bootstrap PRIVATE -Wall -Wno-unused-function)
target_compile_definitions(penstock-bootstrap PRIVATE
  PENSTOCK_DEFAULT_LLVM_ROOT="${LLVM_ROOT}"
  PENSTOCK_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}/penstock"
  PENSTOCK_SUPEROPT_BOOTSTRAP=1)
set_target_properties(penstock-bootstrap PROPERTIES
  OUTPUT_NAME "penstock-bootstrap"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

add_executable(penstock ${PENSTOCK_SOURCES})
target_include_directories(penstock PRIVATE src)
set_target_properties(penstock PROPERTIES EXPORT_NAME compiler)
target_compile_options(penstock PRIVATE -Wall -Wno-unused-function)
# Bake the resolved backend in as penstock's RUNTIME default: bare
# `bin/penstock` invocations then use exactly the toolchain this build
# smoke-tested (and the one the prewarmed runtime IR cache belongs to),
# with no exported LLVM_ROOT required.  At runtime the env LLVM_ROOT
# still wins, and the 23..19 probe stays as a last resort if the baked
# prefix has since vanished.
target_compile_definitions(penstock PRIVATE
  PENSTOCK_DEFAULT_LLVM_ROOT="${LLVM_ROOT}"
  PENSTOCK_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}/penstock")

# In the build tree, the compiler sits in <source>/bin and uses
# <source>/runtime + <source>/build. Install rules below place the runtime
# under the prefix data directory; installed scratch files use the user cache.
set_target_properties(penstock PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

# ------------------------------------------- runtime IR cache (build/*.ll)
# penstock merges libfreshet into user modules by textual IR concatenation.
# The cache key the compiler reads (src/main.cpp runtimeLl(), audit §3) is
# build/frt_rt_<llvm-major>_o<level>.ll — keyed by LLVM major AND runtime
# optimisation level — so the prewarm must write exactly that name at the
# matching -O<level>.  Building it here keeps the first compile/test warm
# and lets `clean` clear it honestly.
# The default below mirrors the g_opts.runtimeOpt default in
# src/common.h — KEEP THE TWO IN SYNC (the level is part of the
# cache file name the compiler looks for).
set(PENSTOCK_RT_IR_OPT_LEVEL 2)
# The cache path is pinned to <source root>/build/ (NOT the CMake binary
# dir) because that is where penstock looks for it at runtime.
execute_process(
  COMMAND "${LLVM_ROOT}/bin/clang" --version
  OUTPUT_VARIABLE _clang_version_out
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _clang_version_rc)
set(LLVM_MAJOR "")
set(LLVM_FULL "")
if(_clang_version_rc EQUAL 0)
  # "clang version 22.1.8 (...)" — patch component optional
  string(REGEX MATCH "clang version ([0-9]+)\\.([0-9]+)(\\.([0-9]+))?" _vm
         "${_clang_version_out}")
  if(_vm)
    set(LLVM_MAJOR "${CMAKE_MATCH_1}")
    set(LLVM_FULL "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
    if(CMAKE_MATCH_4)
      string(APPEND LLVM_FULL ".${CMAKE_MATCH_4}")
    endif()
  endif()
endif()
if(NOT LLVM_MAJOR)
  message(WARNING
    "penstock: cannot parse `clang --version` output — the runtime IR cache "
    "falls back to the unkeyed name build/frt_rt_o${PENSTOCK_RT_IR_OPT_LEVEL}.ll "
    "(penstock will rebuild its own keyed cache on first use).")
  set(FRT_RT_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/build/frt_rt_o${PENSTOCK_RT_IR_OPT_LEVEL}.ll")
else()
  set(FRT_RT_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/build/frt_rt_${LLVM_MAJOR}_o${PENSTOCK_RT_IR_OPT_LEVEL}.ll")
endif()

add_custom_command(
  OUTPUT "${FRT_RT_CACHE}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/build"
  COMMAND "${LLVM_ROOT}/bin/clang" -S -emit-llvm -O${PENSTOCK_RT_IR_OPT_LEVEL} -g0 -x c
          "${CMAKE_CURRENT_SOURCE_DIR}/runtime/frt_rt.c"
          -o "${FRT_RT_CACHE}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/frt_rt.c"
  COMMENT "freshet runtime -> LLVM IR (cache: ${FRT_RT_CACHE})"
  VERBATIM)

add_custom_target(rt_ir DEPENDS "${FRT_RT_CACHE}")
add_dependencies(penstock rt_ir)
add_dependencies(penstock-bootstrap rt_ir)

# ----------------------------------------------- Stage E: language server
# freshet-language-server: LSP 3.18 over stdio JSON-RPC (report §4).
# Built from src/lsp/server.cpp + the same frontend library used by the
# CLI (FRONTEND_SOURCES above): one list, both targets — the LSP can
# never again miss a file the compiler gained.  Same C++17 + same host
# compiler as penstock; links no LLVM libraries (it analyses via the
# frontend API only and never invokes clang itself).
set(LSP_SOURCES
  ${FRONTEND_SOURCES}
  src/lsp/server.cpp)
add_executable(freshet-language-server ${LSP_SOURCES})
target_include_directories(freshet-language-server PRIVATE src)
target_compile_definitions(freshet-language-server PRIVATE PENSTOCK_SUPEROPT_BOOTSTRAP=1)
target_compile_options(freshet-language-server PRIVATE -Wall -Wno-unused-function)
# Same output dir as penstock so editors can find both binaries together.
set_target_properties(freshet-language-server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

# ----------------------------------------------------- Stage A: libfsh
# libfsh public-API implementation layered over runtime/frt_rt.c
# (report §5).  Builds a static library the user can link against in
# addition to the frt_* symbols they already get from the combined IR.
#
# Stage A fix: this requires the C language (enabled in the project()
# call above) AND a working C compiler (defaulted to ${LLVM_ROOT}/bin/clang
# above).  Without both, CMake silently skips generating the build rules
# for CMakeFiles/libfsh.dir/ and the build fails with
#   "No rule to make target 'CMakeFiles/libfsh.dir/build'"
# which is the symptom the user hits when project() enables only CXX.
add_library(libfsh STATIC
  src/libfsh.c
  runtime/frt_rt.c)
target_include_directories(libfsh PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/fsh>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fsh>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(libfsh PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(libfsh PUBLIC m)
endif()
target_compile_options(libfsh PRIVATE -Wall -Wno-unused-function)
# Note: no target_compile_features(... c_std_99) — CMAKE_C_STANDARD is
# already set globally to 11 above, which propagates to every C target
# in the build.  Adding c_std_99 here would be redundant (and on some
# CMake versions caused silent rule-skipping when combined with a
# globally-set CMAKE_C_STANDARD).
set_target_properties(libfsh PROPERTIES
  OUTPUT_NAME fsh
  EXPORT_NAME fsh
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
