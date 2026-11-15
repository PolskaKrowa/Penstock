# ------------------------------------------------- third-party submodule tests
# Logic tests for cmake/ThirdPartySubmodules.cmake. Run through
# scripts/test-third-party-submodules.sh, which drives the modes and checks
# the expected-fatal cases in subprocesses.
#
# Modes (cmake -DPENSTOCK_TEST_MODE=... -P this-file):
#   detection    fake third_party roots: absent / uninitialised / marker /
#                per-submodule override / global switch
#   fatal-forced PENSTOCK_SUBMODULE_Z3=ON with no checkout -> must FATAL
#   real-z3      resolver + patcher + idempotency against third_party/z3
#   real-alive2  resolver + patcher + idempotency against third_party/alive2
#   builder      penstock_build_llvm_backend_from_source on a fake
#                llvm-project tree: build, reuse, rebuild-on-move

if(NOT DEFINED PENSTOCK_TEST_MODE)
  message(FATAL_ERROR "PENSTOCK_TEST_MODE is required")
endif()
if(NOT DEFINED PENSTOCK_TEST_WORK)
  message(FATAL_ERROR "PENSTOCK_TEST_WORK is required")
endif()
if(NOT DEFINED PENSTOCK_TEST_REPO)
  message(FATAL_ERROR "PENSTOCK_TEST_REPO is required")
endif()

function(expect_eq actual expected description)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
      "TEST FAILURE: ${description}\n  expected: '${expected}'\n  actual:   '${actual}'")
  endif()
  message(STATUS "PASS: ${description}")
endfunction()

function(expect_contains haystack_var needle description)
  string(FIND "${${haystack_var}}" "${needle}" _pos)
  if(_pos LESS 0)
    message(FATAL_ERROR
      "TEST FAILURE: ${description}\n  missing needle: '${needle}'")
  endif()
  message(STATUS "PASS: ${description}")
endfunction()

# The submodule-aware module. PENSTOCK_THIRD_PARTY_ROOT may be pre-set by the
# runner (fake-tree modes) or captured from the repository root (real modes).
include("${PENSTOCK_TEST_REPO}/cmake/ThirdPartySubmodules.cmake")

if(PENSTOCK_TEST_MODE STREQUAL "detection")
  set(_root "${PENSTOCK_THIRD_PARTY_ROOT}")
  file(MAKE_DIRECTORY "${_root}")
  # absent
  penstock_third_party_submodule(z3 "src/api/CMakeLists.txt" _v)
  expect_eq("${_v}" "" "absent submodule is skipped")
  # uninitialised (empty directory, exactly what an un-updated gitlink gives)
  file(MAKE_DIRECTORY "${_root}/z3")
  penstock_third_party_submodule(z3 "src/api/CMakeLists.txt" _v)
  expect_eq("${_v}" "" "uninitialised submodule (empty dir) is skipped")
  # initialised
  file(MAKE_DIRECTORY "${_root}/z3/src/api")
  file(WRITE "${_root}/z3/src/api/CMakeLists.txt" "# fake z3\n")
  penstock_third_party_submodule(z3 "src/api/CMakeLists.txt" _v)
  expect_eq("${_v}" "${_root}/z3" "initialised submodule is discovered")
  # wrong marker
  penstock_third_party_submodule(z3 "does/not/exist" _v)
  expect_eq("${_v}" "" "marker miss is reported as absent")
  # per-submodule override OFF
  set(PENSTOCK_SUBMODULE_Z3 OFF)
  penstock_third_party_submodule(z3 "src/api/CMakeLists.txt" _v)
  expect_eq("${_v}" "" "PENSTOCK_SUBMODULE_Z3=OFF skips the submodule")
  unset(PENSTOCK_SUBMODULE_Z3)
  # global switch OFF
  set(PENSTOCK_USE_SUBMODULES OFF)
  penstock_third_party_submodule(z3 "src/api/CMakeLists.txt" _v)
  expect_eq("${_v}" "" "PENSTOCK_USE_SUBMODULES=OFF skips the submodule")
  unset(PENSTOCK_USE_SUBMODULES)
  # resolvers fall back cleanly on the fake tree
  set(CMAKE_BINARY_DIR "${PENSTOCK_TEST_WORK}/build")
  penstock_resolve_z3_sources()
  expect_eq("${penstock_z3_SOURCE_DIR}" "${_root}/z3" "z3 resolver source dir")
  expect_eq("${penstock_z3_BINARY_DIR}"
    "${PENSTOCK_TEST_WORK}/build/third-party/z3" "z3 resolver binary dir")
  expect_eq("${PENSTOCK_Z3_FROM_SUBMODULE}" "TRUE" "z3 resolver origin flag")
  expect_eq("${PENSTOCK_Z3_SUBMODULE_VERSION}" "" "z3 version empty without git")
  # alive2 marker missing in this fake tree -> fetch path
  penstock_resolve_alive2_sources()
  expect_eq("${penstock_alive2_SOURCE_DIR}" "" "alive2 resolver falls back to fetch")
  expect_eq("${PENSTOCK_ALIVE2_FROM_SUBMODULE}" "FALSE" "alive2 origin is fetch")

elseif(PENSTOCK_TEST_MODE STREQUAL "fatal-forced")
  set(_root "${PENSTOCK_TEST_WORK}/third_party")
  file(MAKE_DIRECTORY "${_root}/z3/src/api")
  file(WRITE "${_root}/z3/src/api/CMakeLists.txt" "# fake z3\n")
  # PENSTOCK_SUBMODULE_Z3=ON is injected by the runner; the marker miss must
  # be fatal.
  penstock_third_party_submodule(z3 "no/such/marker" _v)
  message(FATAL_ERROR "TEST FAILURE: forced submodule did not fail fatally")

elseif(PENSTOCK_TEST_MODE STREQUAL "real-z3")
  set(CMAKE_BINARY_DIR "${PENSTOCK_TEST_WORK}/build")
  penstock_submodule_git_description("${PENSTOCK_THIRD_PARTY_ROOT}/z3" _desc)
  expect_contains(_desc "z3-4.15.3" "real z3 git describe reports the pinned tag")
  penstock_resolve_z3_sources()
  expect_eq("${PENSTOCK_Z3_FROM_SUBMODULE}" "TRUE" "real z3 resolver origin")
  expect_eq("${PENSTOCK_Z3_SUBMODULE_VERSION}" "4.15.3"
    "real z3 version parsed from git describe")
  # The patcher must find the pristine upstream markers before patching.
  file(READ "${PENSTOCK_THIRD_PARTY_ROOT}/z3/src/opt/CMakeLists.txt" _opt_before)
  expect_contains(_opt_before "z3_add_component(opt"
    "pristine z3 opt component marker exists")
  penstock_patch_z3_for_embedded_build("${PENSTOCK_THIRD_PARTY_ROOT}/z3")
  file(READ "${PENSTOCK_THIRD_PARTY_ROOT}/z3/src/opt/CMakeLists.txt" _opt_after)
  expect_contains(_opt_after "z3_add_component(penstock_z3_opt"
    "z3 opt component namespaced")
  file(READ "${PENSTOCK_THIRD_PARTY_ROOT}/z3/src/api/CMakeLists.txt" _api_after)
  expect_contains(_api_after "COMPONENT_DEPENDENCIES\n    penstock_z3_opt"
    "z3 api dependency updated")
  file(SHA256 "${PENSTOCK_THIRD_PARTY_ROOT}/z3/src/opt/CMakeLists.txt" _h1)
  file(SHA256 "${PENSTOCK_THIRD_PARTY_ROOT}/z3/src/api/CMakeLists.txt" _h2)
  penstock_patch_z3_for_embedded_build("${PENSTOCK_THIRD_PARTY_ROOT}/z3")
  file(SHA256 "${PENSTOCK_THIRD_PARTY_ROOT}/z3/src/opt/CMakeLists.txt" _h1b)
  file(SHA256 "${PENSTOCK_THIRD_PARTY_ROOT}/z3/src/api/CMakeLists.txt" _h2b)
  expect_eq("${_h1b}" "${_h1}" "z3 opt patch is idempotent")
  expect_eq("${_h2b}" "${_h2}" "z3 api patch is idempotent")

elseif(PENSTOCK_TEST_MODE STREQUAL "real-alive2")
  set(CMAKE_BINARY_DIR "${PENSTOCK_TEST_WORK}/build")
  penstock_resolve_alive2_sources()
  expect_eq("${PENSTOCK_ALIVE2_FROM_SUBMODULE}" "TRUE" "real alive2 resolver origin")
  expect_eq("${penstock_alive2_BINARY_DIR}"
    "${PENSTOCK_TEST_WORK}/build/third-party/alive2"
    "real alive2 resolver binary dir")
  file(READ "${PENSTOCK_THIRD_PARTY_ROOT}/alive2/CMakeLists.txt" _before)
  expect_contains(_before "find_package(Z3 4.8.5 REQUIRED)"
    "pristine alive2 Z3 lookup marker exists")
  penstock_patch_alive2_for_embedded_build("${PENSTOCK_THIRD_PARTY_ROOT}/alive2")
  file(READ "${PENSTOCK_THIRD_PARTY_ROOT}/alive2/CMakeLists.txt" _after)
  expect_contains(_after "add_custom_target(\"alive2-check\""
    "alive2 check target renamed")
  expect_contains(_after "add_library(penstock_alive2_smt STATIC"
    "alive2 smt target namespaced")
  expect_contains(_after "add_library(penstock_alive2_util STATIC"
    "alive2 util target namespaced")
  expect_contains(_after "set(ALIVE_LIBS ir penstock_alive2_smt tools penstock_alive2_util)"
    "alive2 ALIVE_LIBS updated")
  expect_contains(_after "find_package(Z3 4.8.5 CONFIG REQUIRED)"
    "alive2 Z3 lookup forced to CONFIG mode")
  expect_contains(_after
    "include_directories(\${Z3_INCLUDE_DIR} \${Z3_CXX_INCLUDE_DIRS})"
    "alive2 Z3 C++ include dir added")
  file(SHA256 "${PENSTOCK_THIRD_PARTY_ROOT}/alive2/CMakeLists.txt" _h1)
  penstock_patch_alive2_for_embedded_build("${PENSTOCK_THIRD_PARTY_ROOT}/alive2")
  file(SHA256 "${PENSTOCK_THIRD_PARTY_ROOT}/alive2/CMakeLists.txt" _h1b)
  expect_eq("${_h1b}" "${_h1}" "alive2 patch is idempotent")

elseif(PENSTOCK_TEST_MODE STREQUAL "builder")
    # Fake llvm-project checkout: a trivial CMake project that "builds" a
    # backend-shaped prefix (bin/clang, bin/opt, libLLVM, SDK files) by
    # copying a prepared template. It only needs to exercise the builder's
    # configure -> build -> validate -> stamp flow.
    set(_fake "${PENSTOCK_TEST_WORK}/fake-llvm-project")
    file(MAKE_DIRECTORY "${_fake}/llvm" "${_fake}/prefix-template/bin"
      "${_fake}/prefix-template/lib/cmake/llvm"
      "${_fake}/prefix-template/include/llvm/Plugins")
    file(WRITE "${_fake}/llvm/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.16)
project(FakeLLVM LANGUAGES NONE)
# NOTE: \${CMAKE_COMMAND} and \${CMAKE_BINARY_DIR} are deliberately escaped so
# they expand inside THIS generated project, not inside the harness.
add_custom_target(clang
  COMMAND \"\${CMAKE_COMMAND}\" -E copy_directory
          \"${_fake}/prefix-template\" \"\${CMAKE_BINARY_DIR}\")
add_custom_target(opt
  COMMAND \"\${CMAKE_COMMAND}\" -E copy_directory
          \"${_fake}/prefix-template\" \"\${CMAKE_BINARY_DIR}\")
add_custom_target(LLVM
  COMMAND \"\${CMAKE_COMMAND}\" -E copy_directory
          \"${_fake}/prefix-template\" \"\${CMAKE_BINARY_DIR}\")
add_custom_target(clang-resource-headers
  COMMAND \"\${CMAKE_COMMAND}\" -E copy_directory
          \"${_fake}/prefix-template\" \"\${CMAKE_BINARY_DIR}\")
")
    file(WRITE "${_fake}/prefix-template/bin/clang" "#!/bin/sh\necho 'clang version 99.0.0 (fake llvm-project)'\n")
    file(WRITE "${_fake}/prefix-template/bin/opt" "#!/bin/sh\nexit 0\n")
    file(WRITE "${_fake}/prefix-template/lib/libLLVM.so" "fake-dylib\n")
    file(WRITE "${_fake}/prefix-template/lib/cmake/llvm/LLVMConfig.cmake"
      "set(LLVM_PACKAGE_VERSION 99.0.0)\nset(LLVM_ENABLE_RTTI ON)\nset(LLVM_ENABLE_EH ON)\n")
    file(WRITE "${_fake}/prefix-template/include/llvm/Plugins/PassPlugin.h" "// fake\n")
    execute_process(COMMAND chmod +x "${_fake}/prefix-template/bin/clang"
                             "${_fake}/prefix-template/bin/opt")
    # A real git repo makes the source identity a commit sha (stamp flow).
    execute_process(COMMAND git -C "${_fake}" init -q)
    execute_process(COMMAND git -C "${_fake}" -c user.email=t@t -c user.name=t
                             add -A)
    execute_process(COMMAND git -C "${_fake}" -c user.email=t@t -c user.name=t
                             commit -qm "fake llvm-project")

    # Cache (not a normal variable): the builder declares its own cache
    # entry, and a normal variable would be shadowed by that declaration.
    set(PENSTOCK_LLVM_BACKEND_BUILD_DIR "${PENSTOCK_TEST_WORK}/fake-llvm-build"
      CACHE PATH "harness override" FORCE)
    penstock_build_llvm_backend_from_source("${_fake}" _out)
    expect_eq("${_out}" "${PENSTOCK_TEST_WORK}/fake-llvm-build"
      "builder returns the toolchain prefix")
    foreach(_probe
        "${_out}/bin/clang"
        "${_out}/bin/clang++"
        "${_out}/bin/opt"
        "${_out}/lib/cmake/llvm/LLVMConfig.cmake"
        "${_out}/lib/libLLVM.so"
        "${_out}/include/llvm/Plugins/PassPlugin.h")
      if(NOT EXISTS "${_probe}")
        message(FATAL_ERROR "TEST FAILURE: built prefix is missing ${_probe}")
      endif()
    endforeach()
    message(STATUS "PASS: built prefix contains clang/clang++/opt, dylib and SDK")
    file(READ "${_out}/.penstock-llvm-source-stamp" _stamp1)
    expect_contains(_stamp1 "identity: "
      "source stamp written after the build")

    # Second call: complete + matching stamp -> fast reuse path.
    penstock_build_llvm_backend_from_source("${_fake}" _out2)
    expect_eq("${_out2}" "${PENSTOCK_TEST_WORK}/fake-llvm-build"
      "builder reuses the complete prefix")
    file(READ "${_out}/.penstock-llvm-source-stamp" _stamp1b)
    expect_eq("${_stamp1b}" "${_stamp1}" "reuse path leaves the stamp alone")

    # Moving the submodule source (new commit) must rebuild the delta.
    file(APPEND "${_fake}/llvm/CMakeLists.txt" "# moved\n")
    execute_process(COMMAND git -C "${_fake}" -c user.email=t@t -c user.name=t
                             commit -qam "move")
    penstock_build_llvm_backend_from_source("${_fake}" _out3)
    expect_eq("${_out3}" "${PENSTOCK_TEST_WORK}/fake-llvm-build"
      "builder rebuilds after the source moved")
    file(READ "${_out}/.penstock-llvm-source-stamp" _stamp2)
    if("${_stamp2}" STREQUAL "${_stamp1}")
      message(FATAL_ERROR "TEST FAILURE: stamp not updated after source move")
    endif()
    message(STATUS "PASS: stamp tracks the submodule commit")

else()
  message(FATAL_ERROR "unknown PENSTOCK_TEST_MODE: ${PENSTOCK_TEST_MODE}")
endif()

message(STATUS "MODE ${PENSTOCK_TEST_MODE}: ALL CHECKS PASSED")
