# ------------------------------------------------- third-party submodules
# Centralises the discovery of the optional in-tree dependency submodules
# checked out under third_party/:
#
#   third_party/llvm     llvm-project (llvm + clang)
#   third_party/alive2   Alive2 verifier
#   third_party/z3       Z3 SMT solver
#
# When a submodule is initialised (git submodule update --init), Penstock
# builds the dependency from that checkout instead of acquiring it from the
# network (FetchContent clones / official LLVM release tarball):
#
#   * third_party/z3      replaces the penstock_z3 FetchContent clone
#   * third_party/alive2  replaces the penstock_alive2 FetchContent clone
#   * third_party/llvm    (a) becomes the source of the private RTTI/EH LLVM
#                         build used by Alive2, and (b) can additionally build
#                         the whole clang backend toolchain from source so no
#                         release tarball is downloaded at all
#
# Escape hatches:
#   -DPENSTOCK_USE_SUBMODULES=OFF             ignore third_party/ entirely
#   -DPENSTOCK_SUBMODULE_Z3=OFF               ignore one submodule (=ON makes a
#   -DPENSTOCK_SUBMODULE_ALIVE2=OFF            missing checkout fatal; same
#   -DPENSTOCK_SUBMODULE_LLVM=OFF              semantics for each dependency)
#   -DPENSTOCK_BUILD_LLVM_FROM_SUBMODULE=OFF  keep the historical backend
#                                             discovery (probe / tarball)
#
# NOTE: configuring patches Z3's and Alive2's CMakeLists in place (component
# targets are namespaced to avoid collisions between the two projects).  The
# patches are idempotent; `git status` inside an affected submodule shows the
# patched files as modified until `git checkout -- .` restores them, and the
# next configure simply re-applies them.

include_guard(GLOBAL)

option(PENSTOCK_USE_SUBMODULES
  "Prefer initialised third_party/* submodules over fetched dependencies" ON)
option(PENSTOCK_BUILD_LLVM_FROM_SUBMODULE
  "Build the LLVM/Clang backend from the third_party/llvm submodule instead \
of autodetecting or downloading a release toolchain" ON)

# Captured at include time: every consumer (cmake/LlvmBootstrap.cmake before
# project(), cmake/PenstockSuperopt.cmake after it) sits in the top-level
# source directory, so this is the repository root's third_party/.
if(NOT DEFINED PENSTOCK_THIRD_PARTY_ROOT)
  set(PENSTOCK_THIRD_PARTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
endif()

find_program(PENSTOCK_GIT_EXECUTABLE NAMES git)

# penstock_third_party_submodule(<name> <marker> <out_var>)
#   <name>    submodule directory name under third_party/
#   <marker>  relative path that must exist for the checkout to count as
#             initialised; an empty directory (an un-updated submodule) is
#             indistinguishable from "absent" and is silently skipped
#   <out_var> receives the absolute source directory, or "" when the
#             submodule is absent / uninitialised / disabled
function(penstock_third_party_submodule name marker out_var)
  set(${out_var} "" PARENT_SCOPE)
  string(TOUPPER "${name}" _upper)
  set(_forced "")
  if(DEFINED PENSTOCK_SUBMODULE_${_upper})
    set(_forced "${PENSTOCK_SUBMODULE_${_upper}}")
  endif()

  # Explicit per-submodule override wins over the global switch.
  if(_forced)
    # forced ON (any truthy constant)
  elseif(DEFINED PENSTOCK_SUBMODULE_${_upper})
    return()  # forced OFF
  elseif(NOT PENSTOCK_USE_SUBMODULES)
    return()
  endif()

  set(_dir "${PENSTOCK_THIRD_PARTY_ROOT}/${name}")
  if(NOT IS_DIRECTORY "${_dir}" OR NOT EXISTS "${_dir}/${marker}")
    if(_forced)
      message(FATAL_ERROR
        "PENSTOCK_SUBMODULE_${_upper}=ON but third_party/${name} is not "
        "initialised (missing third_party/${name}/${marker}). Run:\n"
        "  git submodule update --init third_party/${name}")
    endif()
    return()
  endif()
  set(${out_var} "${_dir}" PARENT_SCOPE)
endfunction()

# penstock_submodule_git_description(<dir> <out_var>)
# Best-effort `git describe --tags --always --dirty` for status lines; empty
# when the directory is not a git checkout (e.g. an exported tarball).
function(penstock_submodule_git_description dir out_var)
  set(_desc "")
  if(PENSTOCK_GIT_EXECUTABLE AND IS_DIRECTORY "${dir}")
    execute_process(
      COMMAND "${PENSTOCK_GIT_EXECUTABLE}" -C "${dir}"
              describe --tags --always --dirty
      OUTPUT_VARIABLE _desc
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _git_rc
      ERROR_QUIET)
    if(NOT _git_rc EQUAL 0)
      set(_desc "")
    endif()
  endif()
  set(${out_var} "${_desc}" PARENT_SCOPE)
endfunction()

# penstock_git_identity(<dir> <out_var>)
# Identity of a source tree used to stamp from-source builds: the HEAD commit
# for git checkouts, or a content digest of llvm/CMakeLists.txt otherwise.
function(penstock_git_identity dir out_var)
  set(_id "")
  if(PENSTOCK_GIT_EXECUTABLE AND IS_DIRECTORY "${dir}")
    execute_process(
      COMMAND "${PENSTOCK_GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
      OUTPUT_VARIABLE _id
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _git_rc
      ERROR_QUIET)
    if(NOT _git_rc EQUAL 0)
      set(_id "")
    endif()
  endif()
  if("${_id}" STREQUAL "")
    if(EXISTS "${dir}/llvm/CMakeLists.txt")
      file(SHA256 "${dir}/llvm/CMakeLists.txt" _digest)
      set(_id "sha256-${_digest}")
    else()
      set(_id "unknown-source")
    endif()
  endif()
  set(${out_var} "${_id}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------- Z3 resolution
# Sets in the caller's scope:
#   penstock_z3_SOURCE_DIR / penstock_z3_BINARY_DIR  ("" when not a submodule)
#   PENSTOCK_Z3_FROM_SUBMODULE      TRUE when the submodule is in use
#   PENSTOCK_Z3_SUBMODULE_VERSION   dotted version parsed from git describe
#   PENSTOCK_Z3_SUBMODULE_DESCRIPTION  human-readable checkout description
# The variable contract matches what FetchContent_GetProperties would set,
# so the rest of PenstockSuperopt.cmake is origin-agnostic.
function(penstock_resolve_z3_sources)
  set(penstock_z3_SOURCE_DIR "" PARENT_SCOPE)
  set(penstock_z3_BINARY_DIR "" PARENT_SCOPE)
  set(PENSTOCK_Z3_FROM_SUBMODULE FALSE PARENT_SCOPE)
  set(PENSTOCK_Z3_SUBMODULE_VERSION "" PARENT_SCOPE)
  set(PENSTOCK_Z3_SUBMODULE_DESCRIPTION "" PARENT_SCOPE)

  penstock_third_party_submodule(z3 "src/api/CMakeLists.txt" _dir)
  if(NOT _dir)
    return()
  endif()

  penstock_submodule_git_description("${_dir}" _desc)
  set(PENSTOCK_Z3_SUBMODULE_DESCRIPTION
    "${_desc}" PARENT_SCOPE)
  if(_desc MATCHES "([0-9]+[.][0-9]+[.][0-9]+)")
    set(PENSTOCK_Z3_SUBMODULE_VERSION "${CMAKE_MATCH_1}" PARENT_SCOPE)
  endif()

  set(penstock_z3_SOURCE_DIR "${_dir}" PARENT_SCOPE)
  set(penstock_z3_BINARY_DIR "${CMAKE_BINARY_DIR}/third-party/z3" PARENT_SCOPE)
  set(PENSTOCK_Z3_FROM_SUBMODULE TRUE PARENT_SCOPE)
endfunction()

# ---------------------------------------------------- Alive2 resolution
# Same contract as the Z3 resolver, for Alive2 (marker: ir/instr.h, which the
# upstream tree has had at every revision Penstock tracks).
function(penstock_resolve_alive2_sources)
  set(penstock_alive2_SOURCE_DIR "" PARENT_SCOPE)
  set(penstock_alive2_BINARY_DIR "" PARENT_SCOPE)
  set(PENSTOCK_ALIVE2_FROM_SUBMODULE FALSE PARENT_SCOPE)
  set(PENSTOCK_ALIVE2_SUBMODULE_DESCRIPTION "" PARENT_SCOPE)

  penstock_third_party_submodule(alive2 "ir/instr.h" _dir)
  if(NOT _dir)
    return()
  endif()

  penstock_submodule_git_description("${_dir}" _desc)
  set(PENSTOCK_ALIVE2_SUBMODULE_DESCRIPTION "${_desc}" PARENT_SCOPE)

  set(penstock_alive2_SOURCE_DIR "${_dir}" PARENT_SCOPE)
  set(penstock_alive2_BINARY_DIR
    "${CMAKE_BINARY_DIR}/third-party/alive2" PARENT_SCOPE)
  set(PENSTOCK_ALIVE2_FROM_SUBMODULE TRUE PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------- patches
# The two functions below were part of PenstockSuperopt.cmake and are
# unchanged apart from taking the source directory as an argument: they
# namespace Z3's and Alive2's colliding component targets (Z3's historical
# `opt` component vs LLVM's `opt` executable; Alive2's `smt`/`util`
# libraries vs Z3 4.15.x components of the same names). Both are
# idempotent and work on any checkout origin (submodule or FetchContent).
function(penstock_patch_z3_for_embedded_build source_dir)
  # Z3's optimisation component historically declared itself as `opt`,
  # which collides with LLVM's `opt` tool target (target names are
  # globally unique in CMake). Penstock therefore renames the component
  # to penstock_z3_opt on Z3 <= 4.15.x. Upstream Z3 renamed the component
  # to `z3_opt` on master after the 4.15.3 release (the src/api/CMakeLists
  # dependency entry renamed alongside it), so newer checkouts need no
  # patch at all. Three states are recognised here:
  #   `opt`             Z3 <= 4.15.x -> renamed, api dependency updated
  #   `z3_opt`          upstream rename -> checkout left untouched
  #   `penstock_z3_opt` this patch already applied -> idempotent no-op
  # Anything else refuses to configure rather than risk a silent target
  # collision; the message names the supported pins.
  set(_penstock_z3_opt_cmake "${source_dir}/src/opt/CMakeLists.txt")
  if(NOT EXISTS "${_penstock_z3_opt_cmake}")
    message(FATAL_ERROR
      "The selected Z3 source revision does not contain src/opt/CMakeLists.txt; "
      "cannot apply the LLVM `opt` target collision workaround.")
  endif()
  file(READ "${_penstock_z3_opt_cmake}" _penstock_z3_opt_cmake_text)
  # Whitespace between the macro call and the component name (a cosmetic
  # reformat upstream) is tolerated: read the declared name, then branch.
  # CMAKE_MATCH_1 keeps stale values when the regex does not match, so it
  # is cleared first.
  set(CMAKE_MATCH_1 "")
  string(REGEX MATCH "z3_add_component[ \t\r\n]*\\([ \t\r\n]*([A-Za-z0-9_]+)"
    _penstock_z3_opt_decl "${_penstock_z3_opt_cmake_text}")
  set(_penstock_z3_opt_name "${CMAKE_MATCH_1}")
  # The dependency name src/api must use to stay consistent with the
  # component; empty means the upstream rename already took care of it.
  set(_penstock_z3_opt_dep_name "")
  if(_penstock_z3_opt_name STREQUAL "opt")
    # Z3 <= 4.15.x: keep whatever whitespace followed the macro's opening
    # parenthesis in the replacement.
    string(REGEX REPLACE "z3_add_component([ \t\r\n]*\\([ \t\r\n]*)opt"
      "z3_add_component\\1penstock_z3_opt"
      _penstock_z3_opt_cmake_text "${_penstock_z3_opt_cmake_text}")
    file(WRITE "${_penstock_z3_opt_cmake}" "${_penstock_z3_opt_cmake_text}")
    set(_penstock_z3_opt_dep_name "penstock_z3_opt")
  elseif(_penstock_z3_opt_name STREQUAL "z3_opt")
    # Upstream Z3 already renamed the component: the LLVM `opt` collision
    # it worked around is gone, and src/api already depends on `z3_opt`.
    # Leave both files untouched.
    message(STATUS
      "Penstock: Z3 already names its optimisation component 'z3_opt' "
      "(upstream rename; no LLVM 'opt' collision to work around).")
  elseif(_penstock_z3_opt_name STREQUAL "penstock_z3_opt")
    # Idempotent re-configure: an earlier run already applied the rename.
    set(_penstock_z3_opt_dep_name "penstock_z3_opt")
  elseif(_penstock_z3_opt_name STREQUAL "")
    message(FATAL_ERROR
      "Z3's src/opt/CMakeLists.txt declares no z3_add_component(...) "
      "component at all:\n  ${_penstock_z3_opt_cmake}\n"
      "The Z3 build layout changed in a way this workaround does not "
      "know, and guessing risks a collision with LLVM's 'opt' target. "
      "Pin a known Z3 revision, e.g.\n"
      "  git -C third_party/z3 checkout z3-4.15.3\n"
      "or restore the revision recorded by Penstock's submodule:\n"
      "  git submodule update --init third_party/z3\n"
      "or bypass the submodule and let Penstock fetch the pinned tag:\n"
      "  cmake <args> -DPENSTOCK_SUBMODULE_Z3=OFF\n"
      "and report the layout above so the workaround can learn it.")
  else()
    message(FATAL_ERROR
      "Could not recognise Z3's optimisation component declaration in "
      "${_penstock_z3_opt_cmake} (found '${_penstock_z3_opt_name}'). "
      "Penstock renames the legacy 'opt' component to avoid a collision "
      "with LLVM's 'opt' target and accepts upstream's 'z3_opt' rename; "
      "any other layout is refused rather than risk the collision. "
      "Pin a known Z3 revision, e.g.\n"
      "  git -C third_party/z3 checkout z3-4.15.3\n"
      "or restore the revision recorded by Penstock's submodule:\n"
      "  git submodule update --init third_party/z3\n"
      "or bypass the submodule and let Penstock fetch the pinned tag:\n"
      "  cmake <args> -DPENSTOCK_SUBMODULE_Z3=OFF\n"
      "and report the declaration above so the workaround can learn it.")
  endif()

  # Z3's API component declares an explicit dependency on the optimisation
  # component. Keep its component graph consistent with the renamed target;
  # upstream z3_opt trees are internally consistent already, so only the
  # renamed-component cases inspect src/api.
  if(NOT "${_penstock_z3_opt_dep_name}" STREQUAL "")
    set(_penstock_z3_api_cmake "${source_dir}/src/api/CMakeLists.txt")
    if(NOT EXISTS "${_penstock_z3_api_cmake}")
      message(FATAL_ERROR
        "The selected Z3 source revision has no src/api/CMakeLists.txt.")
    endif()
    file(READ "${_penstock_z3_api_cmake}" _penstock_z3_api_cmake_text)
    # The dependency entry's indentation and position after the keyword
    # vary across revisions; tolerate any whitespace run after
    # COMPONENT_DEPENDENCIES before the component name.
    set(CMAKE_MATCH_1 "")
    string(REGEX MATCH "COMPONENT_DEPENDENCIES([ \t\r\n]+)opt"
      _penstock_z3_api_dep "${_penstock_z3_api_cmake_text}")
    if(NOT "${_penstock_z3_api_dep}" STREQUAL "")
      string(REGEX REPLACE "COMPONENT_DEPENDENCIES([ \t\r\n]+)opt"
        "COMPONENT_DEPENDENCIES\\1${_penstock_z3_opt_dep_name}"
        _penstock_z3_api_cmake_text "${_penstock_z3_api_cmake_text}")
      file(WRITE "${_penstock_z3_api_cmake}" "${_penstock_z3_api_cmake_text}")
    else()
      # Not found: either an earlier configure already renamed the entry
      # (idempotent re-run), or the dependency layout genuinely changed.
      string(FIND "${_penstock_z3_api_cmake_text}"
        "COMPONENT_DEPENDENCIES[ \t\r\n][ \t\r\n]*${_penstock_z3_opt_dep_name}"
        _penstock_z3_api_dep_renamed)
      if(_penstock_z3_api_dep_renamed EQUAL -1)
        # One last exact form: the pre-4.15.3-style literal that the
        # previous revision of this patch produced.
        string(FIND "${_penstock_z3_api_cmake_text}"
          "COMPONENT_DEPENDENCIES\n    ${_penstock_z3_opt_dep_name}"
          _penstock_z3_api_dep_literal)
        if(_penstock_z3_api_dep_literal EQUAL -1)
          message(FATAL_ERROR
            "Could not update Z3's API dependency on the renamed opt "
            "component. The selected Z3 source layout may have changed "
            "(src/api/CMakeLists.txt names neither 'opt' after "
            "COMPONENT_DEPENDENCIES nor '${_penstock_z3_opt_dep_name}'). "
            "Pin a known Z3 revision (git -C third_party/z3 checkout "
            "z3-4.15.3) and report the file above so the workaround can "
            "learn it.")
        endif()
      endif()
    endif()
  endif()
endfunction()

function(penstock_patch_alive2_for_embedded_build source_dir)
  set(_penstock_alive2_cmake "${source_dir}/CMakeLists.txt")
  file(READ "${_penstock_alive2_cmake}" _penstock_alive2_cmake_text)
  string(REPLACE "add_custom_target(\"check\""
                 "add_custom_target(\"alive2-check\""
    _penstock_alive2_cmake_text "${_penstock_alive2_cmake_text}")
  # Penstock consumes Alive2's static libraries directly. Do not build or
  # install its standalone CLI tools (alive, alive-tv, alive-exec, etc.).
  string(REPLACE "install(TARGETS alive alive-jobserver)"
                 "# Penstock embeds Alive2; standalone CLI install disabled."
    _penstock_alive2_cmake_text "${_penstock_alive2_cmake_text}")
  string(REPLACE "install(TARGETS alive-tv quick-fuzz alive-exec)"
                 "# Penstock embeds Alive2; standalone LLVM tool install disabled."
    _penstock_alive2_cmake_text "${_penstock_alive2_cmake_text}")
  # Alive2 and Z3 are both added to the same CMake directory hierarchy.
  # Alive2's own libraries are unfortunately named `smt` and `util`, which
  # collide with Z3 4.15.x component targets of the same names. Namespace the
  # two Alive2 targets in its top-level CMakeLists before add_subdirectory();
  # keep its source lists and library contents unchanged. Patch every target
  # reference in the root file so the dependencies remain internally
  # consistent. This is intentionally idempotent for an existing checkout
  # that has already been configured once.
  string(FIND "${_penstock_alive2_cmake_text}"
    "add_library(smt STATIC \${SMT_SRCS})" _penstock_alive2_smt_decl)
  string(FIND "${_penstock_alive2_cmake_text}"
    "add_library(penstock_alive2_smt STATIC \${SMT_SRCS})" _penstock_alive2_smt_decl_renamed)
  if(NOT _penstock_alive2_smt_decl EQUAL -1)
    string(REPLACE "add_library(smt STATIC \${SMT_SRCS})"
                   "add_library(penstock_alive2_smt STATIC \${SMT_SRCS})"
      _penstock_alive2_cmake_text "${_penstock_alive2_cmake_text}")
  elseif(_penstock_alive2_smt_decl_renamed EQUAL -1)
    message(FATAL_ERROR
      "Could not namespace Alive2's smt target. The selected Alive2 source "
      "layout has changed; refusing to configure with a Z3 target collision.")
  endif()

  string(FIND "${_penstock_alive2_cmake_text}"
    "add_library(util STATIC \${UTIL_SRCS})" _penstock_alive2_util_decl)
  string(FIND "${_penstock_alive2_cmake_text}"
    "add_library(penstock_alive2_util STATIC \${UTIL_SRCS})" _penstock_alive2_util_decl_renamed)
  if(NOT _penstock_alive2_util_decl EQUAL -1)
    string(REPLACE "add_library(util STATIC \${UTIL_SRCS})"
                   "add_library(penstock_alive2_util STATIC \${UTIL_SRCS})"
      _penstock_alive2_cmake_text "${_penstock_alive2_cmake_text}")
  elseif(_penstock_alive2_util_decl_renamed EQUAL -1)
    message(FATAL_ERROR
      "Could not namespace Alive2's util target. The selected Alive2 source "
      "layout has changed; refusing to configure with a Z3 target collision.")
  endif()

  string(FIND "${_penstock_alive2_cmake_text}"
    "add_dependencies(util generate_version)" _penstock_alive2_util_dependency)
  string(FIND "${_penstock_alive2_cmake_text}"
    "add_dependencies(penstock_alive2_util generate_version)" _penstock_alive2_util_dependency_renamed)
  if(NOT _penstock_alive2_util_dependency EQUAL -1)
    string(REPLACE "add_dependencies(util generate_version)"
                   "add_dependencies(penstock_alive2_util generate_version)"
      _penstock_alive2_cmake_text "${_penstock_alive2_cmake_text}")
  elseif(_penstock_alive2_util_dependency_renamed EQUAL -1)
    message(FATAL_ERROR
      "Could not update Alive2's util dependency on generate_version.")
  endif()

  string(FIND "${_penstock_alive2_cmake_text}"
    "set(ALIVE_LIBS ir smt tools util)" _penstock_alive2_libs)
  string(FIND "${_penstock_alive2_cmake_text}"
    "set(ALIVE_LIBS ir penstock_alive2_smt tools penstock_alive2_util)" _penstock_alive2_libs_renamed)
  if(NOT _penstock_alive2_libs EQUAL -1)
    string(REPLACE "set(ALIVE_LIBS ir smt tools util)"
                   "set(ALIVE_LIBS ir penstock_alive2_smt tools penstock_alive2_util)"
      _penstock_alive2_cmake_text "${_penstock_alive2_cmake_text}")
  elseif(_penstock_alive2_libs_renamed EQUAL -1)
    message(FATAL_ERROR
      "Could not update Alive2's ALIVE_LIBS list for the namespaced targets.")
  endif()

  # Force Alive2 to consume Penstock's in-tree Z3 target through the config
  # package PenstockSuperopt.cmake generates. Module mode would select LLVM's
  # FindZ3.cmake, which probes for a prebuilt on-disk archive at configure
  # time and cannot work with this not-yet-built source target.
  string(REPLACE "find_package(Z3 4.8.5 REQUIRED)"
                 "find_package(Z3 4.8.5 CONFIG REQUIRED)"
    _penstock_alive2_cmake_text "${_penstock_alive2_cmake_text}")
  string(FIND "${_penstock_alive2_cmake_text}" "find_package(Z3 4.8.5 CONFIG REQUIRED)" _penstock_alive2_z3_config_lookup)
  if(_penstock_alive2_z3_config_lookup EQUAL -1)
    message(FATAL_ERROR
      "Could not patch Alive2's Z3 lookup to CONFIG mode. Its CMakeLists.txt "
      "has changed; refusing to fall back to the invalid -llibz3 probe.")
  endif()
  # Upstream's FindZ3 normally points at an installed include directory where
  # z3++.h is flattened into the include root. Our in-tree source checkout
  # keeps it under src/api/c++, so add that include directory explicitly.
  string(REPLACE "include_directories(\${Z3_INCLUDE_DIR})"
                 "include_directories(\${Z3_INCLUDE_DIR} \${Z3_CXX_INCLUDE_DIRS})"
    _penstock_alive2_cmake_text "${_penstock_alive2_cmake_text}")
  string(FIND "${_penstock_alive2_cmake_text}"
    "include_directories(\${Z3_INCLUDE_DIR} \${Z3_CXX_INCLUDE_DIRS})"
    _penstock_alive2_z3_cpp_include)
  if(_penstock_alive2_z3_cpp_include EQUAL -1)
    message(FATAL_ERROR
      "Could not add Z3's in-tree C++ API include directory to Alive2. "
      "Its CMakeLists.txt has changed; refusing a build that may not find z3++.h.")
  endif()
  file(WRITE "${_penstock_alive2_cmake}" "${_penstock_alive2_cmake_text}")
endfunction()

# ------------------------------------------- LLVM backend from submodule
# Builds the complete LLVM/Clang backend toolchain (clang, clang++, opt, the
# monolithic libLLVM shared library and the development SDK) from the
# third_party/llvm submodule source.  The result is a prefix usable directly
# as LLVM_ROOT: an LLVM BUILD TREE, not an installed prefix — bin/, lib/ and
# lib/cmake/ are populated by the build, while the bulk of the SDK headers
# (PassPlugin.h included) stays in the source checkout and is reached through
# the generated LLVMConfig.cmake's LLVM_INCLUDE_DIRS.  The first build is long
# (a full Release clang); it happens
# once at configure time, and later configures reuse the result unless the
# submodule's HEAD commit moved (detected through a stamp file), in which
# case only the incremental delta is rebuilt.
#
# <source_root>  llvm-project checkout (contains llvm/) or an llvm/ directory
# <out_root>     receives the resulting toolchain prefix ("" on failure)
#
# Also sets PENSTOCK_LLVM_BACKEND_FROM_SUBMODULE=TRUE in the caller's scope
# so cmake/PenstockSuperopt.cmake can reuse the same SDK for Alive2 instead
# of building a second private LLVM from the same sources.
function(penstock_build_llvm_backend_from_source source_root out_root)
  set(${out_root} "" PARENT_SCOPE)
  set(PENSTOCK_LLVM_BACKEND_FROM_SUBMODULE TRUE PARENT_SCOPE)

  if(EXISTS "${source_root}/llvm/CMakeLists.txt")
    set(_src_dir "${source_root}/llvm")
  elseif(EXISTS "${source_root}/CMakeLists.txt")
    set(_src_dir "${source_root}")
  else()
    message(FATAL_ERROR
      "penstock: third_party/llvm is initialised but is not an llvm-project "
      "checkout (no llvm/CMakeLists.txt): ${source_root}")
  endif()

  # The cache default is evaluated here (before project(), CMAKE_BINARY_DIR
  # is already the -B directory); users normally pin it with -D to survive
  # `rm -rf build` or to share one toolchain build between checkouts.
  set(PENSTOCK_LLVM_BACKEND_BUILD_DIR "${CMAKE_BINARY_DIR}/third-party/llvm-backend"
    CACHE PATH
    "Build directory for the LLVM/Clang backend built from third_party/llvm")
  set(_bdir "${PENSTOCK_LLVM_BACKEND_BUILD_DIR}")
  set(PENSTOCK_LLVM_BACKEND_BUILD_TYPE "Release" CACHE STRING
    "Build type of the LLVM/Clang backend built from third_party/llvm")
  set(PENSTOCK_LLVM_BACKEND_TARGETS_TO_BUILD "Native" CACHE STRING
    "LLVM backends built in the from-submodule toolchain (default: host only)")
  set(PENSTOCK_LLVM_BACKEND_BUILD_JOBS "" CACHE STRING
    "Parallel jobs for the from-submodule LLVM build (default: \
CMAKE_BUILD_PARALLEL_LEVEL or the number of CPUs)")

  set(_stamp "${_bdir}/.penstock-llvm-source-stamp")

  # A complete toolchain prefix: compilers, opt, the monolithic shared
  # library and the development SDK.  The from-submodule result is a build
  # tree, so the plugin header is accepted either from the prefix include
  # (installed layout) or from the source checkout's include directory
  # (build-tree layout: a real LLVM build tree only generates
  # include/llvm/Config headers and leaves the rest in the source tree).
  #
  # <root>         prefix to validate (build tree or installed prefix)
  # <src_include>  source-tree include root ("" when unknown)
  # <out_var>      receives TRUE when every component is present
  # <missing_var>  receives the list of missing components ("" if complete)
  function(_penstock_backend_complete root src_include out_var missing_var)
    set(_missing "")
    foreach(_probe
        "bin/clang"
        "bin/clang++"
        "bin/opt"
        "lib/cmake/llvm/LLVMConfig.cmake")
      if(NOT EXISTS "${root}/${_probe}")
        list(APPEND _missing "${_probe}")
      endif()
    endforeach()
    set(_have_sdk_header FALSE)
    foreach(_include_root "${root}/include" "${src_include}")
      if(NOT "${_include_root}" STREQUAL "" AND
         (EXISTS "${_include_root}/llvm/Plugins/PassPlugin.h" OR
          EXISTS "${_include_root}/llvm/Passes/PassPlugin.h"))
        set(_have_sdk_header TRUE)
      endif()
    endforeach()
    if(NOT _have_sdk_header)
      list(APPEND _missing
        "PassPlugin.h (searched ${root}/include and ${src_include})")
    endif()
    file(GLOB _dylib
      "${root}/lib/libLLVM*.so*"
      "${root}/lib/libLLVM*.dylib*")
    if(NOT _dylib)
      list(APPEND _missing "lib/libLLVM* shared library")
    endif()
    if(_missing)
      set(${out_var} FALSE PARENT_SCOPE)
    else()
      set(${out_var} TRUE PARENT_SCOPE)
    endif()
    set(${missing_var} "${_missing}" PARENT_SCOPE)
  endfunction()

  _penstock_backend_complete("${_bdir}" "${_src_dir}/include" _complete
    _missing_precheck)
  penstock_git_identity("${source_root}" _identity)
  set(_stamp_ok FALSE)
  if(_complete AND EXISTS "${_stamp}")
    file(READ "${_stamp}" _stamp_text)
    string(FIND "${_stamp_text}" "${_identity}" _stamp_hit)
    if(NOT _stamp_hit LESS 0)
      set(_stamp_ok TRUE)
    endif()
  endif()

  if(_complete AND _stamp_ok)
    message(STATUS
      "Penstock: reusing the LLVM/Clang backend built from third_party/llvm "
      "(${_identity}) at ${_bdir}")
    set(${out_root} "${_bdir}" PARENT_SCOPE)
    return()
  endif()

  if(NOT _complete AND EXISTS "${_bdir}/CMakeCache.txt" AND NOT _stamp_ok)
    set(_moved_detail "no previous source stamp")
    if(_stamp_text)
      string(REGEX MATCH "identity: [^\r\n]*" _old_identity "${_stamp_text}")
      if(_old_identity)
        set(_moved_detail "was ${_old_identity}")
      endif()
    endif()
    message(STATUS
      "Penstock: third_party/llvm moved since the backend was last built "
      "(${_moved_detail}; now ${_identity}); rebuilding the delta")
  endif()

  if(_complete AND NOT _stamp_ok AND NOT EXISTS "${_stamp}")
    message(STATUS
      "Penstock: ${_bdir} already holds a complete backend prefix without a "
      "source stamp; confirming it with an incremental build pass (a build "
      "interrupted before validation leaves exactly this state).")
  endif()

  message(STATUS
    "Penstock: building the LLVM/Clang backend from the third_party/llvm "
    "submodule instead of downloading a release toolchain.")
  message(STATUS
    "  source : ${_src_dir} (${_identity})")
  message(STATUS
    "  build  : ${_bdir} (${PENSTOCK_LLVM_BACKEND_BUILD_TYPE}; this is a "
    "full clang build and takes a while on first configure)")

  if(NOT EXISTS "${_bdir}/CMakeCache.txt" OR NOT _stamp_ok)
    set(_configure_args
      -S "${_src_dir}"
      -B "${_bdir}"
      "-DCMAKE_BUILD_TYPE=${PENSTOCK_LLVM_BACKEND_BUILD_TYPE}"
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DLLVM_ENABLE_PROJECTS=clang
      -DLLVM_ENABLE_RTTI=ON
      -DLLVM_ENABLE_EH=ON
      # The monolithic shared library keeps one LLVM copy across penstock,
      # the language-server-side superoptimiser and the opt plugin.
      -DBUILD_SHARED_LIBS=OFF
      -DLLVM_BUILD_LLVM_DYLIB=ON
      -DLLVM_LINK_LLVM_DYLIB=ON
      "-DLLVM_TARGETS_TO_BUILD=${PENSTOCK_LLVM_BACKEND_TARGETS_TO_BUILD}"
      -DLLVM_INCLUDE_TOOLS=ON
      -DLLVM_INCLUDE_TESTS=OFF
      -DLLVM_INCLUDE_EXAMPLES=OFF
      -DLLVM_INCLUDE_BENCHMARKS=OFF
      -DLLVM_ENABLE_ASSERTIONS=OFF
      # Keep the from-source toolchain self-contained: no optional
      # third-party development packages are required to build or run it.
      -DLLVM_ENABLE_TERMINFO=OFF
      -DLLVM_ENABLE_ZLIB=OFF
      -DLLVM_ENABLE_ZSTD=OFF
      -DLLVM_ENABLE_LIBXML2=OFF
      -DLLVM_ENABLE_LIBEDIT=OFF
      -DLLVM_ENABLE_FFI=OFF
      -DLLVM_ENABLE_LIBPFM=OFF)
    if(CMAKE_GENERATOR)
      list(APPEND _configure_args -G "${CMAKE_GENERATOR}")
    endif()
    if(CMAKE_C_COMPILER)
      list(APPEND _configure_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
    endif()
    if(CMAKE_CXX_COMPILER)
      list(APPEND _configure_args "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    endif()
    if(CMAKE_MAKE_PROGRAM)
      list(APPEND _configure_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
    endif()
    if(CMAKE_TOOLCHAIN_FILE)
      list(APPEND _configure_args "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
    endif()
    if(CMAKE_GENERATOR_PLATFORM)
      list(APPEND _configure_args -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
      list(APPEND _configure_args -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" ${_configure_args}
      RESULT_VARIABLE _configure_rc)
    if(NOT _configure_rc EQUAL 0)
      message(FATAL_ERROR
        "Penstock: could not configure the LLVM/Clang backend from "
        "third_party/llvm (cmake exited with ${_configure_rc}).")
    endif()
  endif()

  set(_jobs "${PENSTOCK_LLVM_BACKEND_BUILD_JOBS}")
  if("${_jobs}" STREQUAL "")
    if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL} AND
       NOT "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" STREQUAL "")
      set(_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
    else()
      set(_jobs "")
      execute_process(
        COMMAND nproc
        OUTPUT_VARIABLE _cpus
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _nproc_rc
        ERROR_QUIET)
      if(NOT _nproc_rc EQUAL 0 OR "${_cpus}" STREQUAL "")
        execute_process(
          COMMAND sysctl -n hw.ncpu
          OUTPUT_VARIABLE _cpus
          OUTPUT_STRIP_TRAILING_WHITESPACE
          RESULT_VARIABLE _sysctl_rc
          ERROR_QUIET)
        if(NOT _sysctl_rc EQUAL 0 OR "${_cpus}" STREQUAL "")
          set(_cpus 2)
        endif()
      endif()
      set(_jobs "${_cpus}")
    endif()
  endif()
  message(STATUS "  jobs   : ${_jobs}")

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_bdir}"
            --config "${PENSTOCK_LLVM_BACKEND_BUILD_TYPE}"
            --target clang opt LLVM clang-resource-headers
            --parallel "${_jobs}"
    RESULT_VARIABLE _build_rc)
  if(NOT _build_rc EQUAL 0)
    message(FATAL_ERROR
      "Penstock: the LLVM/Clang backend build from third_party/llvm failed "
      "(cmake --build exited with ${_build_rc}). See the compiler errors "
      "above; a re-run resumes the incremental build where it stopped.")
  endif()

  # The build tree always has bin/clang; give it a clang++ sibling when LLVM
  # did not create one itself (release installs have it, build trees vary).
  if(NOT EXISTS "${_bdir}/bin/clang++" AND EXISTS "${_bdir}/bin/clang")
    file(CREATE_LINK clang "${_bdir}/bin/clang++" SYMBOLIC RESULT _link_rc)
    if(NOT _link_rc EQUAL 0)
      execute_process(
        COMMAND ln -s clang "${_bdir}/bin/clang++"
        RESULT_VARIABLE _ln_rc)
      if(NOT _ln_rc EQUAL 0)
        message(FATAL_ERROR
          "Penstock: could not create bin/clang++ next to the freshly built "
          "clang in ${_bdir}/bin.")
      endif()
    endif()
  endif()

  _penstock_backend_complete("${_bdir}" "${_src_dir}/include" _complete
    _missing_components)
  if(NOT _complete)
    string(REPLACE ";" "\n  * " _missing_report "${_missing_components}")
    message(FATAL_ERROR
      "Penstock: the from-submodule LLVM/Clang build finished but "
      "${_bdir} does not contain a complete backend prefix; missing:\n"
      "  * ${_missing_report}\n"
      "The from-submodule backend is an LLVM build tree: SDK headers live in "
      "${_src_dir}/include rather than in the build tree's include/. Re-"
      "running the configure resumes the incremental build where it stopped.")
  endif()

  file(WRITE "${_stamp}"
    "source: ${source_root}\nidentity: ${_identity}\n")
  message(STATUS "  ready  : ${_bdir}")
  set(${out_root} "${_bdir}" PARENT_SCOPE)
endfunction()
