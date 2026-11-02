# ------------------------------------------------ embedded superoptimiser
# The stage graph is intentionally self-hosting:
#   A. penstock-bootstrap is built without superoptimisation;
#   B. that compiler compiles src/superopt.fsh to a native object;
#   C. final penstock links the Freshet object plus static Z3/Alive2 libraries.
# No Souper package, alive-tv executable, or z3 executable is used at runtime.

include(FetchContent)

option(PENSTOCK_BUILD_EMBEDDED_SUPEROPT
  "Build the Freshet-hosted in-process Z3/Alive2 superoptimiser" ON)
set(PENSTOCK_Z3_GIT_TAG "z3-4.15.3" CACHE STRING
  "Z3 tag used for the embedded static solver")
set(PENSTOCK_ALIVE2_GIT_TAG "master" CACHE STRING
  "Alive2 revision used for the embedded verifier")
set(PENSTOCK_ALIVE2_LLVM_GIT_TAG "main" CACHE STRING
  "LLVM revision paired with Alive2 (upstream recommends LLVM main for Alive2 master)")

if(PENSTOCK_BUILD_EMBEDDED_SUPEROPT)
  # Z3 is linked into both the executable and the optional LLVM plugin.
  # Ensure its component object libraries are suitable for a shared plugin.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)

  # Alive2's LLVM adapter is tightly coupled to LLVM headers and C++ APIs.
  # Its translator requires RTTI and exception handling, and Alive2 master is
  # developed against LLVM main rather than necessarily the latest release
  # branch. Inspect the backend SDK first, then normally use a separate
  # private checkout/build for the verifier to avoid mixing API revisions.
  option(PENSTOCK_BUILD_RTTI_LLVM
    "Build a private LLVM revision matched to Alive2, with RTTI/EH enabled" ON)
  set(PENSTOCK_RTTI_LLVM_SOURCE_DIR "" CACHE PATH
    "Optional llvm-project checkout matching PENSTOCK_ALIVE2_LLVM_GIT_TAG")
  # Use a fresh build directory: earlier Penstock builds used a release-tagged
  # LLVM source tree here, which is not API-compatible with Alive2 master.
  set(PENSTOCK_ALIVE2_LLVM_BUILD_DIR
    "${CMAKE_BINARY_DIR}/_deps/penstock-alive2-llvm-build" CACHE PATH
    "Build directory for the private RTTI/EH-enabled LLVM used by Alive2")
  # Internal compatibility name consumed by the plugin/install sections.
  # This normal variable intentionally overrides any old cached path.
  set(PENSTOCK_RTTI_LLVM_BUILD_DIR "${PENSTOCK_ALIVE2_LLVM_BUILD_DIR}")
  set(PENSTOCK_RTTI_LLVM_TARGETS_TO_BUILD "Native" CACHE STRING
    "LLVM backends built in the private RTTI-enabled SDK (default: host only)")

  function(_penstock_llvm_config_value config_file key out_var)
    file(STRINGS "${config_file}" _config_lines
      REGEX "^[ \t]*set\\(${key}[ \t]+")
    set(_value "")
    foreach(_line IN LISTS _config_lines)
      string(REGEX REPLACE
        "^[ \t]*set\\(${key}[ \t]+\\\"?([^\\\"\\) \t]+).*"
        "\\1" _candidate "${_line}")
      if(NOT "${_candidate}" STREQUAL "${_line}" AND
         NOT "${_candidate}" MATCHES "^@")
        set(_value "${_candidate}")
        break()
      endif()
    endforeach()
    set(${out_var} "${_value}" PARENT_SCOPE)
  endfunction()

  # Locate the config file without calling find_package: importing a bad SDK
  # first would create globally named LLVM targets that cannot be replaced by
  # the private RTTI build later in the same CMake configure run.
  set(_penstock_llvm_config_hints
    "${LLVM_DIR}"
    "${LLVM_ROOT}/lib/cmake/llvm"
    "${LLVM_ROOT}/lib64/cmake/llvm"
    "${LLVM_ROOT}/lib/llvm-${LLVM_MAJOR}/lib/cmake/llvm"
    "/usr/lib/llvm-${LLVM_MAJOR}/lib/cmake/llvm"
    ${CMAKE_PREFIX_PATH})
  unset(_penstock_llvm_config_file CACHE)
  find_file(_penstock_llvm_config_file
    NAMES LLVMConfig.cmake
    HINTS ${_penstock_llvm_config_hints}
    PATH_SUFFIXES lib/cmake/llvm lib64/cmake/llvm)

  if(NOT _penstock_llvm_config_file AND PENSTOCK_FETCH_LLVM)
    penstock_latest_llvm_release("${LLVM_MAJOR}" _penstock_alive_llvm_version)
    if(_penstock_alive_llvm_version)
      message(STATUS
        "Matching LLVM ${LLVM_MAJOR} development SDK not found; fetching "
        "the official SDK before checking its RTTI/EH configuration")
      penstock_download_llvm("${_penstock_alive_llvm_version}"
        _penstock_alive_llvm_root "alive2")
      set(_penstock_llvm_config_file
        "${_penstock_alive_llvm_root}/lib/cmake/llvm/LLVMConfig.cmake")
    endif()
  endif()

  if(NOT _penstock_llvm_config_file OR
     NOT EXISTS "${_penstock_llvm_config_file}")
    message(FATAL_ERROR
      "The embedded superoptimiser requires an LLVM development SDK. "
      "Install LLVM ${LLVM_MAJOR} headers/libraries, enable "
      "PENSTOCK_FETCH_LLVM, or provide a matching LLVM_DIR.")
  endif()

  _penstock_llvm_config_value("${_penstock_llvm_config_file}"
    LLVM_PACKAGE_VERSION _penstock_sdk_version)
  _penstock_llvm_config_value("${_penstock_llvm_config_file}"
    LLVM_ENABLE_RTTI _penstock_sdk_rtti)
  _penstock_llvm_config_value("${_penstock_llvm_config_file}"
    LLVM_ENABLE_EH _penstock_sdk_eh)
  string(REGEX MATCH "^[0-9]+" _penstock_alive_llvm_major
    "${_penstock_sdk_version}")
  if(NOT "${_penstock_alive_llvm_major}" STREQUAL "${LLVM_MAJOR}")
    message(FATAL_ERROR
      "Embedded Alive2 needs LLVM major ${LLVM_MAJOR}, but "
      "${_penstock_llvm_config_file} reports ${_penstock_sdk_version}. "
      "Set LLVM_DIR to the matching SDK.")
  endif()

  string(TOUPPER "${_penstock_sdk_rtti}" _penstock_sdk_rtti_upper)
  string(TOUPPER "${_penstock_sdk_eh}" _penstock_sdk_eh_upper)
  set(_penstock_sdk_has_rtti_eh TRUE)
  if(NOT _penstock_sdk_rtti_upper MATCHES "^(ON|TRUE|YES|1)$" OR
     NOT _penstock_sdk_eh_upper MATCHES "^(ON|TRUE|YES|1)$")
    set(_penstock_sdk_has_rtti_eh FALSE)
  endif()

  # Alive2's development branch tracks LLVM main, not necessarily the latest
  # numbered LLVM release. Building it against the backend SDK by version
  # number alone can compile against a different C++ API (e.g. LLVM 23.1.4
  # paired with a newer Alive2 master). By default, build a separate LLVM
  # checkout at PENSTOCK_ALIVE2_LLVM_GIT_TAG even when the user's backend SDK
  # already has RTTI/EH. The selected LLVM_ROOT/clang remains Penstock's
  # backend; only the in-process verifier uses this private LLVM runtime.
  if(PENSTOCK_BUILD_RTTI_LLVM)
    message(STATUS
      "Building private Alive2 LLVM revision '${PENSTOCK_ALIVE2_LLVM_GIT_TAG}' "
      "with RTTI/EH (backend SDK remains LLVM ${_penstock_sdk_version})")

    if(PENSTOCK_RTTI_LLVM_SOURCE_DIR)
      if(EXISTS "${PENSTOCK_RTTI_LLVM_SOURCE_DIR}/llvm/CMakeLists.txt")
        set(_penstock_rtti_llvm_source_root "${PENSTOCK_RTTI_LLVM_SOURCE_DIR}")
        set(_penstock_rtti_llvm_source_dir "${PENSTOCK_RTTI_LLVM_SOURCE_DIR}/llvm")
      elseif(EXISTS "${PENSTOCK_RTTI_LLVM_SOURCE_DIR}/CMakeLists.txt")
        get_filename_component(_penstock_rtti_llvm_source_dir
          "${PENSTOCK_RTTI_LLVM_SOURCE_DIR}" ABSOLUTE)
        get_filename_component(_penstock_rtti_llvm_source_root
          "${_penstock_rtti_llvm_source_dir}" DIRECTORY)
      else()
        message(FATAL_ERROR
          "PENSTOCK_RTTI_LLVM_SOURCE_DIR must point to an llvm-project "
          "checkout or its llvm/ subdirectory matching "
          "PENSTOCK_ALIVE2_LLVM_GIT_TAG='${PENSTOCK_ALIVE2_LLVM_GIT_TAG}'.")
      endif()
    else()
      # A new FetchContent name forces a fresh checkout instead of reusing
      # the older `penstock_rtti_llvm_source` cache pinned to llvmorg-23.1.4.
      FetchContent_Declare(penstock_alive2_llvm_source
        GIT_REPOSITORY https://github.com/llvm/llvm-project.git
        GIT_TAG "${PENSTOCK_ALIVE2_LLVM_GIT_TAG}"
        GIT_SHALLOW TRUE)
      FetchContent_GetProperties(penstock_alive2_llvm_source)
      if(NOT penstock_alive2_llvm_source_POPULATED)
        if(POLICY CMP0169)
          cmake_policy(PUSH)
          cmake_policy(SET CMP0169 OLD)
          FetchContent_Populate(penstock_alive2_llvm_source)
          cmake_policy(POP)
        else()
          FetchContent_Populate(penstock_alive2_llvm_source)
        endif()
      endif()
      set(_penstock_rtti_llvm_source_root
        "${penstock_alive2_llvm_source_SOURCE_DIR}")
      set(_penstock_rtti_llvm_source_dir
        "${penstock_alive2_llvm_source_SOURCE_DIR}/llvm")
    endif()

    if(NOT EXISTS "${_penstock_rtti_llvm_source_dir}/CMakeLists.txt")
      message(FATAL_ERROR
        "The Alive2-compatible LLVM source tree has no llvm/CMakeLists.txt: "
        "${_penstock_rtti_llvm_source_dir}")
    endif()

    if(WIN32)
      message(FATAL_ERROR
        "The private Alive2 LLVM build currently requires a platform that "
        "supports LLVM_BUILD_LLVM_DYLIB. Supply an SDK whose LLVM API revision "
        "matches Alive2 and which has RTTI/EH enabled, or disable the private "
        "build with -DPENSTOCK_BUILD_RTTI_LLVM=OFF.")
    endif()

    message(STATUS
      "Configuring private LLVM '${PENSTOCK_ALIVE2_LLVM_GIT_TAG}' with "
      "RTTI/EH for Alive2; Penstock's backend toolchain remains "
      "${LLVM_ROOT}")
    if(APPLE)
      set(_penstock_nested_llvm_build_rpath "@loader_path/../lib")
    else()
      set(_penstock_nested_llvm_build_rpath "$ORIGIN/../lib")
    endif()
    set(_penstock_rtti_llvm_configure_args
      -S "${_penstock_rtti_llvm_source_dir}"
      -B "${PENSTOCK_RTTI_LLVM_BUILD_DIR}"
      -G "${CMAKE_GENERATOR}"
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      "-DCMAKE_BUILD_RPATH=${_penstock_nested_llvm_build_rpath}"
      -DLLVM_ENABLE_RTTI=ON
      -DLLVM_ENABLE_EH=ON
      # LLVM_BUILD_LLVM_DYLIB creates the monolithic shared library used by
      # opt and the Penstock plugin. LLVM rejects BUILD_SHARED_LIBS=ON with
      # this option; pin it OFF (including in an existing nested build cache).
      -DBUILD_SHARED_LIBS=OFF
      -DLLVM_BUILD_LLVM_DYLIB=ON
      -DLLVM_LINK_LLVM_DYLIB=ON
      "-DLLVM_TARGETS_TO_BUILD=${PENSTOCK_RTTI_LLVM_TARGETS_TO_BUILD}"
      -DLLVM_INCLUDE_TOOLS=ON
      -DLLVM_INCLUDE_TESTS=OFF
      -DLLVM_INCLUDE_EXAMPLES=OFF
      -DLLVM_INCLUDE_BENCHMARKS=OFF
      -DLLVM_INCLUDE_UTILS=OFF
      -DLLVM_ENABLE_ASSERTIONS=OFF
      -DLLVM_ENABLE_TERMINFO=OFF
      -DLLVM_ENABLE_ZLIB=OFF
      -DLLVM_ENABLE_ZSTD=OFF
      -DLLVM_ENABLE_LIBXML2=OFF
      -DLLVM_ENABLE_LIBEDIT=OFF
      -DLLVM_ENABLE_FFI=OFF
      -DLLVM_ENABLE_LIBPFM=OFF)
    if(CMAKE_C_COMPILER)
      list(APPEND _penstock_rtti_llvm_configure_args
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
    endif()
    if(CMAKE_CXX_COMPILER)
      list(APPEND _penstock_rtti_llvm_configure_args
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    endif()
    if(CMAKE_MAKE_PROGRAM)
      list(APPEND _penstock_rtti_llvm_configure_args
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
    endif()
    if(CMAKE_TOOLCHAIN_FILE)
      list(APPEND _penstock_rtti_llvm_configure_args
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
    endif()
    if(CMAKE_GENERATOR_PLATFORM)
      list(APPEND _penstock_rtti_llvm_configure_args
        -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
      list(APPEND _penstock_rtti_llvm_configure_args
        -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" ${_penstock_rtti_llvm_configure_args}
      RESULT_VARIABLE _penstock_rtti_llvm_configure_rc
      OUTPUT_VARIABLE _penstock_rtti_llvm_configure_out
      ERROR_VARIABLE _penstock_rtti_llvm_configure_err)
    if(NOT _penstock_rtti_llvm_configure_rc EQUAL 0)
      message(FATAL_ERROR
        "Could not configure the private Alive2 LLVM revision "
        "'${PENSTOCK_ALIVE2_LLVM_GIT_TAG}'.\n"
        "${_penstock_rtti_llvm_configure_out}\n"
        "${_penstock_rtti_llvm_configure_err}")
    endif()

    set(_penstock_private_llvm_config
      "${PENSTOCK_RTTI_LLVM_BUILD_DIR}/lib/cmake/llvm/LLVMConfig.cmake")
    if(NOT EXISTS "${_penstock_private_llvm_config}")
      message(FATAL_ERROR
        "The private LLVM configure completed but did not produce "
        "${_penstock_private_llvm_config}.")
    endif()
    _penstock_llvm_config_value("${_penstock_private_llvm_config}"
      LLVM_PACKAGE_VERSION _penstock_private_llvm_version)
    _penstock_llvm_config_value("${_penstock_private_llvm_config}"
      LLVM_ENABLE_RTTI _penstock_private_llvm_rtti)
    _penstock_llvm_config_value("${_penstock_private_llvm_config}"
      LLVM_ENABLE_EH _penstock_private_llvm_eh)
    # Do not require the private verifier LLVM to have the same numbered
    # release as the user's backend SDK. Alive2 master follows LLVM main and
    # its C++ API can be ahead of the most recent release branch. The private
    # monolithic libLLVM is used consistently by the native bridge (and opt
    # host, if enabled); Penstock's clang driver remains LLVM_ROOT.
    if(NOT _penstock_private_llvm_rtti OR NOT _penstock_private_llvm_eh)
      message(FATAL_ERROR
        "The private LLVM build did not enable both RTTI and EH. "
        "Its LLVMConfig.cmake reports RTTI=${_penstock_private_llvm_rtti}, "
        "EH=${_penstock_private_llvm_eh}.")
    endif()
    # Keep the user's original LLVM_DIR cache entry intact; this normal
    # directory-scope value selects the private SDK for this configure only.
    set(LLVM_DIR "${PENSTOCK_RTTI_LLVM_BUILD_DIR}/lib/cmake/llvm")
    set(PENSTOCK_RTTI_LLVM_IN_USE TRUE)
    set(PENSTOCK_RTTI_LLVM_SOURCE_ROOT "${_penstock_rtti_llvm_source_root}")
    set(PENSTOCK_RTTI_LLVM_SOURCE_DIR "${_penstock_rtti_llvm_source_dir}")
    set(PENSTOCK_RTTI_LLVM_LIBRARY_DIR "${PENSTOCK_RTTI_LLVM_BUILD_DIR}/lib")
    set(PENSTOCK_ALIVE2_LLVM_VERSION "${_penstock_private_llvm_version}")

    # LLVM and opt are built as a nested project. Alive2 and Penstock link
    # against the generated build-tree package, and this dependency ensures
    # the shared LLVM runtime exists before either consumer is compiled/linked.
    set(_penstock_rtti_llvm_targets LLVM)
    if(PENSTOCK_BUILD_OPT_PLUGIN)
      list(APPEND _penstock_rtti_llvm_targets opt)
    endif()
    add_custom_target(penstock_rtti_llvm
      COMMAND "${CMAKE_COMMAND}" --build "${PENSTOCK_RTTI_LLVM_BUILD_DIR}"
              --config Release --target ${_penstock_rtti_llvm_targets}
      USES_TERMINAL
      COMMENT "Building private LLVM ${PENSTOCK_ALIVE2_LLVM_GIT_TAG} with RTTI and EH for Alive2")

    unset(LLVM_FOUND)
    find_package(LLVM CONFIG REQUIRED
      PATHS "${PENSTOCK_RTTI_LLVM_BUILD_DIR}/lib/cmake/llvm"
      NO_DEFAULT_PATH)
  else()
    get_filename_component(_penstock_llvm_config_dir
      "${_penstock_llvm_config_file}" DIRECTORY)
    set(LLVM_DIR "${_penstock_llvm_config_dir}")
    if(COMMAND penstock_prepare_llvm_zstd_dependency)
      penstock_prepare_llvm_zstd_dependency()
    endif()
    find_package(LLVM CONFIG REQUIRED
      PATHS "${_penstock_llvm_config_dir}"
      NO_DEFAULT_PATH)
    set(PENSTOCK_RTTI_LLVM_IN_USE FALSE)
    if(NOT _penstock_sdk_has_rtti_eh)
      message(FATAL_ERROR
        "The selected LLVM SDK (LLVM ${_penstock_sdk_version}) has "
        "RTTI=${_penstock_sdk_rtti}, EH=${_penstock_sdk_eh}. Alive2 requires "
        "both. Enable PENSTOCK_BUILD_RTTI_LLVM or provide an LLVM checkout "
        "that matches PENSTOCK_ALIVE2_GIT_TAG.")
    endif()
    message(WARNING
      "Using the selected LLVM SDK directly for Alive2. Ensure it matches "
      "the API revision expected by Alive2 '${PENSTOCK_ALIVE2_GIT_TAG}'. "
      "The default private LLVM build avoids this compatibility risk.")
  endif()

  if(NOT LLVM_FOUND)
    message(FATAL_ERROR "CMake could not load the selected LLVM SDK configuration.")
  endif()
  if(NOT LLVM_ENABLE_RTTI OR NOT LLVM_ENABLE_EH)
    message(FATAL_ERROR
      "Alive2 requires LLVM_ENABLE_RTTI=ON and LLVM_ENABLE_EH=ON. "
      "The selected LLVM config still reports RTTI=${LLVM_ENABLE_RTTI}, "
      "EH=${LLVM_ENABLE_EH}.")
  endif()

  find_program(PENSTOCK_RE2C_EXECUTABLE NAMES re2c)
  if(NOT PENSTOCK_RE2C_EXECUTABLE)
    message(FATAL_ERROR
      "Alive2's source build requires re2c (for its standalone parser tool). "
      "Install re2c with your system package manager and reconfigure.")
  endif()
  set(RE2C "${PENSTOCK_RE2C_EXECUTABLE}" CACHE FILEPATH "re2c executable for Alive2" FORCE)

  # Build Z3 as a static archive. Its C++ API include directory is added
  # explicitly because z3++.h lives in src/api/c++ in upstream's source tree.
  set(Z3_BUILD_LIBZ3_SHARED OFF CACHE BOOL "Build static Z3" FORCE)
  set(Z3_BUILD_LIBZ3_CORE ON CACHE BOOL "Build the Z3 core library" FORCE)
  set(Z3_BUILD_EXECUTABLE OFF CACHE BOOL "Do not build the Z3 CLI" FORCE)
  set(Z3_BUILD_TEST_EXECUTABLES OFF CACHE BOOL "Do not build Z3 tests" FORCE)
  set(Z3_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "Do not build Z3 Python bindings" FORCE)
  set(Z3_ENABLE_EXAMPLE_TARGETS OFF CACHE BOOL "Do not build Z3 examples" FORCE)
  FetchContent_Declare(penstock_z3
    GIT_REPOSITORY https://github.com/Z3Prover/z3.git
    GIT_TAG "${PENSTOCK_Z3_GIT_TAG}"
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(penstock_z3)
  if(NOT penstock_z3_POPULATED)
    # Populate first so we can apply a narrow target-name compatibility patch
    # before Z3 creates its component targets in Penstock's CMake directory.
    # LLVMConfig exports an imported executable target named `opt`; Z3 4.15.x
    # also names one of its object-library components `opt`. Those names are
    # globally unique in CMake, so keep the Z3 component namespaced.
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
      FetchContent_Populate(penstock_z3)
      cmake_policy(POP)
    else()
      FetchContent_Populate(penstock_z3)
    endif()
  endif()
  set(_penstock_z3_opt_cmake "${penstock_z3_SOURCE_DIR}/src/opt/CMakeLists.txt")
  if(NOT EXISTS "${_penstock_z3_opt_cmake}")
    message(FATAL_ERROR
      "The selected Z3 source revision does not contain src/opt/CMakeLists.txt; "
      "cannot apply the LLVM `opt` target collision workaround.")
  endif()
  file(READ "${_penstock_z3_opt_cmake}" _penstock_z3_opt_cmake_text)
  string(FIND "${_penstock_z3_opt_cmake_text}" "z3_add_component(opt" _penstock_z3_opt_decl)
  string(FIND "${_penstock_z3_opt_cmake_text}" "z3_add_component(penstock_z3_opt" _penstock_z3_opt_renamed_decl)
  if(NOT _penstock_z3_opt_decl EQUAL -1)
    string(REPLACE "z3_add_component(opt"
                   "z3_add_component(penstock_z3_opt"
      _penstock_z3_opt_cmake_text "${_penstock_z3_opt_cmake_text}")
    file(WRITE "${_penstock_z3_opt_cmake}" "${_penstock_z3_opt_cmake_text}")
  elseif(_penstock_z3_opt_renamed_decl EQUAL -1)
    message(FATAL_ERROR
      "Could not find Z3's expected `z3_add_component(opt ...)` declaration in "
      "${_penstock_z3_opt_cmake}. The Z3 source layout has changed; refusing "
      "to continue with a possible CMake target collision.")
  endif()
  # Z3's API component declares an explicit dependency on the `opt`
  # component. Keep its component graph consistent with the renamed target.
  set(_penstock_z3_api_cmake "${penstock_z3_SOURCE_DIR}/src/api/CMakeLists.txt")
  if(NOT EXISTS "${_penstock_z3_api_cmake}")
    message(FATAL_ERROR "The selected Z3 source revision has no src/api/CMakeLists.txt.")
  endif()
  file(READ "${_penstock_z3_api_cmake}" _penstock_z3_api_cmake_text)
  string(FIND "${_penstock_z3_api_cmake_text}"
    "COMPONENT_DEPENDENCIES\n    opt" _penstock_z3_api_opt_dep)
  string(FIND "${_penstock_z3_api_cmake_text}"
    "COMPONENT_DEPENDENCIES\n    penstock_z3_opt" _penstock_z3_api_opt_dep_renamed)
  if(NOT _penstock_z3_api_opt_dep EQUAL -1)
    string(REPLACE "COMPONENT_DEPENDENCIES\n    opt"
                   "COMPONENT_DEPENDENCIES\n    penstock_z3_opt"
      _penstock_z3_api_cmake_text "${_penstock_z3_api_cmake_text}")
    file(WRITE "${_penstock_z3_api_cmake}" "${_penstock_z3_api_cmake_text}")
  elseif(_penstock_z3_api_opt_dep_renamed EQUAL -1)
    message(FATAL_ERROR
      "Could not update Z3's API dependency on the renamed opt component. "
      "The selected Z3 source layout may have changed.")
  endif()
  add_subdirectory("${penstock_z3_SOURCE_DIR}" "${penstock_z3_BINARY_DIR}" EXCLUDE_FROM_ALL)
  if(NOT TARGET libz3)
    message(FATAL_ERROR "The selected Z3 source revision did not create target libz3.")
  endif()
  if(NOT TARGET z3::libz3)
    add_library(z3::libz3 ALIAS libz3)
  endif()
  # Export both the C and C++ API include directories through the target; this
  # lets Alive2 and Penstock consume z3++.h without global include_directories.
  target_include_directories(libz3 PUBLIC
    "$<BUILD_INTERFACE:${penstock_z3_SOURCE_DIR}/src/api>"
    "$<BUILD_INTERFACE:${penstock_z3_SOURCE_DIR}/src/api/c++>")

  # Alive2's upstream CMake currently calls find_package(Z3 4.8.5 REQUIRED),
  # which selects LLVM's FindZ3.cmake. That module insists on linking a probe
  # executable during configure. Z3 is a just-added build target and its
  # archive does not exist yet; handing it the bare string `libz3` produces
  # the invalid linker option -llibz3. Instead, use a tiny config package that
  # links the actual in-tree target and reports the tag's version. This avoids
  # configure-time linking and preserves the build dependency on libz3.
  set(_penstock_z3_package_dir "${CMAKE_CURRENT_BINARY_DIR}/penstock-z3-package")
  file(MAKE_DIRECTORY "${_penstock_z3_package_dir}")
  set(_penstock_z3_default_version "4.15.3")
  string(REGEX MATCH "[0-9]+[.][0-9]+[.][0-9]+" _penstock_z3_tag_version
    "${PENSTOCK_Z3_GIT_TAG}")
  if(_penstock_z3_tag_version)
    set(_penstock_z3_default_version "${_penstock_z3_tag_version}")
  endif()
  set(PENSTOCK_Z3_PACKAGE_VERSION "${_penstock_z3_default_version}")
  file(WRITE "${_penstock_z3_package_dir}/Z3Config.cmake"
    "set(Z3_FOUND TRUE)\n"
    "set(Z3_VERSION ${PENSTOCK_Z3_PACKAGE_VERSION})\n"
    "set(Z3_INCLUDE_DIR \"${penstock_z3_SOURCE_DIR}/src/api\")\n"
    "set(Z3_C_INCLUDE_DIRS \"${penstock_z3_SOURCE_DIR}/src/api\")\n"
    "set(Z3_CXX_INCLUDE_DIRS \"${penstock_z3_SOURCE_DIR}/src/api/c++\")\n"
    "set(Z3_LIBRARIES z3::libz3)\n")
  file(WRITE "${_penstock_z3_package_dir}/Z3ConfigVersion.cmake"
    "set(PACKAGE_VERSION ${PENSTOCK_Z3_PACKAGE_VERSION})\n"
    "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
    "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
    "else()\n  set(PACKAGE_VERSION_COMPATIBLE FALSE)\nendif()\n")
  set(Z3_DIR "${_penstock_z3_package_dir}" CACHE PATH "Fetched Z3 package config" FORCE)
  set(Z3_INCLUDE_DIR "${penstock_z3_SOURCE_DIR}/src/api")
  set(Z3_C_INCLUDE_DIRS "${penstock_z3_SOURCE_DIR}/src/api")
  set(Z3_CXX_INCLUDE_DIRS "${penstock_z3_SOURCE_DIR}/src/api/c++")
  set(Z3_LIBRARIES z3::libz3)

  # Alive2 currently builds C++20 static libraries (IR, SMT, tools, util and
  # llvm_util). BUILD_TV is OFF: the compiler calls the verifier API directly,
  # not its command-line executable or LLVM opt plugin.
  set(BUILD_TV OFF CACHE BOOL "Do not build Alive2's opt plugin" FORCE)
  set(BUILD_LLVM_UTILS ON CACHE BOOL "Build Alive2's in-process LLVM adapter" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build Alive2 static libraries" FORCE)
  # LLVM_DIR is already scoped to the selected (possibly private) SDK above.
  # Do not force it into the cache: the next configure must rediscover the
  # user's original backend SDK and recreate the private fallback target.
  FetchContent_Declare(penstock_alive2
    GIT_REPOSITORY https://github.com/AliveToolkit/alive2.git
    GIT_TAG "${PENSTOCK_ALIVE2_GIT_TAG}"
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(penstock_alive2)
  if(NOT penstock_alive2_POPULATED)
    # FetchContent_Populate is used intentionally: Alive2's standalone
    # executable install rules are patched before its add_subdirectory call.
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
      FetchContent_Populate(penstock_alive2)
      cmake_policy(POP)
    else()
      FetchContent_Populate(penstock_alive2)
    endif()
  endif()
  # Alive2's standalone project creates a generic `check` target. Penstock
  # has its own regression-suite target with that name, so give Alive2's
  # optional source tests a distinct name before adding its project.
  set(_penstock_alive2_cmake "${penstock_alive2_SOURCE_DIR}/CMakeLists.txt")
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
  # consistent. This is intentionally idempotent for an existing FetchContent
  # checkout that has already been configured once.
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
  # package above. Module mode would select LLVM's FindZ3.cmake, which probes
  # for a prebuilt on-disk archive at configure time and cannot work with this
  # not-yet-built FetchContent target.
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
  add_subdirectory("${penstock_alive2_SOURCE_DIR}"
                   "${penstock_alive2_BINARY_DIR}" EXCLUDE_FROM_ALL)

  if(NOT TARGET llvm_util OR NOT TARGET ir OR
     NOT TARGET penstock_alive2_smt OR NOT TARGET tools OR
     NOT TARGET penstock_alive2_util)
    message(FATAL_ERROR
      "Alive2 did not expose the expected namespaced in-process library targets.")
  endif()

  list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
  include(AddLLVM)
  llvm_map_components_to_libnames(PENSTOCK_LLVM_COMPONENT_LIBS
    support core irreader bitwriter analysis passes transformutils codegen
    targetparser AllTargetsCodeGens AllTargetsDescs AllTargetsInfos)

  # Prefer the monolithic shared LLVM library if the SDK exports one. This
  # allows the same bridge archive to be used by the standalone compiler and
  # the optional opt plugin without loading a second copy of LLVM globals.
  set(PENSTOCK_LLVM_LINK_LIBS "")
  if(PENSTOCK_RTTI_LLVM_IN_USE AND TARGET LLVM)
    set(PENSTOCK_LLVM_LINK_LIBS LLVM)
  elseif(TARGET LLVM)
    get_target_property(_penstock_llvm_type LLVM TYPE)
    if(_penstock_llvm_type STREQUAL "SHARED_LIBRARY")
      set(PENSTOCK_LLVM_LINK_LIBS LLVM)
    elseif(_penstock_llvm_type STREQUAL "UNKNOWN_LIBRARY")
      get_target_property(_penstock_llvm_location LLVM IMPORTED_LOCATION)
      if(_penstock_llvm_location MATCHES "\\.so(\\.[0-9]+)*$" OR
         _penstock_llvm_location MATCHES "\\.dylib$")
        set(PENSTOCK_LLVM_LINK_LIBS LLVM)
      endif()
    endif()
  endif()
  if(NOT PENSTOCK_LLVM_LINK_LIBS)
    set(PENSTOCK_LLVM_LINK_LIBS ${PENSTOCK_LLVM_COMPONENT_LIBS})
  endif()

  add_library(penstock_superopt_core STATIC src/superopt_native.cpp)
  set_target_properties(penstock_superopt_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME penstock_superopt_core)
  target_compile_features(penstock_superopt_core PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(penstock_superopt_core PRIVATE /EHsc)
  else()
    target_compile_options(penstock_superopt_core PRIVATE -fexceptions)
  endif()
  target_include_directories(penstock_superopt_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${penstock_alive2_SOURCE_DIR}"
    "${penstock_alive2_BINARY_DIR}"
    "${penstock_z3_SOURCE_DIR}/src/api"
    "${penstock_z3_SOURCE_DIR}/src/api/c++"
    ${LLVM_INCLUDE_DIRS})
  if(LLVM_DEFINITIONS)
    separate_arguments(_penstock_llvm_definitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_options(penstock_superopt_core PRIVATE ${_penstock_llvm_definitions})
  endif()
  target_link_libraries(penstock_superopt_core PUBLIC
    llvm_util ir penstock_alive2_smt tools penstock_alive2_util z3::libz3
    ${PENSTOCK_LLVM_LINK_LIBS} Threads::Threads)
  if(PENSTOCK_RTTI_LLVM_IN_USE)
    add_dependencies(penstock_superopt_core penstock_rtti_llvm)
  endif()

  set(_penstock_freshet_superopt_ll
      "${CMAKE_CURRENT_BINARY_DIR}/freshet-superopt.ll")
  set(_penstock_freshet_superopt_obj
      "${CMAKE_CURRENT_BINARY_DIR}/freshet-superopt${CMAKE_C_OUTPUT_EXTENSION}")
  # Stage B: bootstrap Penstock emits LLVM IR for the Freshet engine. Compile
  # that IR with the selected matching Clang SDK so the object is explicitly
  # PIC (it is linked into both the executable and, when enabled, an opt .so).
  # No dynamic module loader or runtime source compilation is involved.
  add_custom_command(
    OUTPUT "${_penstock_freshet_superopt_obj}"
    COMMAND "$<TARGET_FILE:penstock-bootstrap>" -O0 -S --legacy-S
            "${CMAKE_CURRENT_SOURCE_DIR}/src/superopt.fsh"
            -o "${_penstock_freshet_superopt_ll}"
    COMMAND "${LLVM_ROOT}/bin/clang" -O2 -fPIC -c
            "${_penstock_freshet_superopt_ll}"
            -o "${_penstock_freshet_superopt_obj}"
    DEPENDS penstock-bootstrap
            "${CMAKE_CURRENT_SOURCE_DIR}/src/superopt.fsh"
            "${FRT_RT_CACHE}"
            "${LLVM_ROOT}/bin/clang"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Penstock stage B: bootstrap compiler + Clang -> PIC Freshet superoptimiser object"
    VERBATIM)
  add_custom_target(penstock_superopt_freshet
    DEPENDS "${_penstock_freshet_superopt_obj}")
  add_dependencies(penstock penstock_superopt_freshet)
  target_sources(penstock PRIVATE "${_penstock_freshet_superopt_obj}")
  target_link_libraries(penstock PRIVATE penstock_superopt_core)
  target_compile_definitions(penstock PRIVATE
    PENSTOCK_HAS_FSH_SUPEROPT=1 PENSTOCK_HAS_ALIVE2=1)
  if(PENSTOCK_RTTI_LLVM_IN_USE)
    file(RELATIVE_PATH _penstock_embedded_llvm_relpath
      "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
      "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/penstock/embedded/lib")
    if(APPLE)
      set(_penstock_embedded_llvm_rpath "@loader_path/${_penstock_embedded_llvm_relpath}")
    else()
      set(_penstock_embedded_llvm_rpath "$ORIGIN/${_penstock_embedded_llvm_relpath}")
    endif()
    set_property(TARGET penstock APPEND PROPERTY INSTALL_RPATH
      "${_penstock_embedded_llvm_rpath}")
    install(DIRECTORY "${PENSTOCK_RTTI_LLVM_LIBRARY_DIR}/"
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/penstock/embedded/lib"
      FILES_MATCHING
        PATTERN "cmake" EXCLUDE
        PATTERN "libLLVM.so*"
        PATTERN "libLLVM-*.so*"
        PATTERN "libLLVM.dylib*"
        PATTERN "libLLVM-*.dylib*")
    if(PENSTOCK_BUILD_OPT_PLUGIN)
      # Keep opt beside its matching LLVM runtime. The nested build's
      # $ORIGIN/../lib RPATH remains valid after this directory is installed.
      install(PROGRAMS "${PENSTOCK_RTTI_LLVM_BUILD_DIR}/bin/opt"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/penstock/embedded/bin")
    endif()
  endif()

  # Install the in-process libraries and public headers too, even though the
  # Penstock executable statically contains the required code. This supports
  # downstream CMake consumers and makes the vendored dependency footprint
  # explicit in the install tree.
  install(TARGETS penstock_superopt_core ir penstock_alive2_smt tools
    penstock_alive2_util llvm_util
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/penstock/embedded")
  install(TARGETS libz3
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/penstock/embedded"
    PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/penstock/z3/api")
  install(DIRECTORY "${penstock_z3_SOURCE_DIR}/src/api/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/penstock/z3/api"
    FILES_MATCHING PATTERN "*.h")
  install(DIRECTORY "${penstock_z3_SOURCE_DIR}/src/api/c++/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/penstock/z3/c++"
    FILES_MATCHING PATTERN "*.h")
  install(DIRECTORY "${penstock_alive2_SOURCE_DIR}/ir/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/penstock/alive2/ir"
    FILES_MATCHING PATTERN "*.h")
  install(DIRECTORY "${penstock_alive2_SOURCE_DIR}/smt/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/penstock/alive2/smt"
    FILES_MATCHING PATTERN "*.h")
  install(DIRECTORY "${penstock_alive2_SOURCE_DIR}/llvm_util/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/penstock/alive2/llvm_util"
    FILES_MATCHING PATTERN "*.h")
  install(DIRECTORY "${penstock_alive2_SOURCE_DIR}/tools/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/penstock/alive2/tools"
    FILES_MATCHING PATTERN "*.h")
  install(DIRECTORY "${penstock_alive2_SOURCE_DIR}/util/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/penstock/alive2/util"
    FILES_MATCHING PATTERN "*.h")
  install(FILES
    "${penstock_alive2_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/penstock-alive2")
  if(PENSTOCK_RTTI_LLVM_IN_USE AND
     EXISTS "${PENSTOCK_RTTI_LLVM_SOURCE_ROOT}/LICENSE.TXT")
    install(FILES "${PENSTOCK_RTTI_LLVM_SOURCE_ROOT}/LICENSE.TXT"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/penstock-llvm")
  elseif(PENSTOCK_RTTI_LLVM_IN_USE AND
         EXISTS "${PENSTOCK_RTTI_LLVM_SOURCE_ROOT}/LICENSE")
    install(FILES "${PENSTOCK_RTTI_LLVM_SOURCE_ROOT}/LICENSE"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/penstock-llvm")
  endif()
  if(EXISTS "${penstock_z3_SOURCE_DIR}/LICENSE.txt")
    install(FILES "${penstock_z3_SOURCE_DIR}/LICENSE.txt"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/penstock-z3")
  elseif(EXISTS "${penstock_z3_SOURCE_DIR}/LICENSE")
    install(FILES "${penstock_z3_SOURCE_DIR}/LICENSE"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/penstock-z3")
  endif()

  message(STATUS "Embedded superoptimiser: Freshet + static Z3 + in-process Alive2")
  message(STATUS "  Z3 revision    : ${PENSTOCK_Z3_GIT_TAG}")
  message(STATUS "  Alive2 revision: ${PENSTOCK_ALIVE2_GIT_TAG}")
  message(STATUS "  LLVM backend   : ${_penstock_sdk_version} (${LLVM_ROOT})")
  if(PENSTOCK_RTTI_LLVM_IN_USE)
    message(STATUS "  Alive2 LLVM ref: ${PENSTOCK_ALIVE2_LLVM_GIT_TAG} (${PENSTOCK_ALIVE2_LLVM_VERSION})")
    message(STATUS "  Alive2 LLVM dir: ${PENSTOCK_ALIVE2_LLVM_BUILD_DIR}")
  else()
    message(STATUS "  Alive2 LLVM SDK: ${LLVM_PACKAGE_VERSION} (user supplied)")
  endif()
else()
  message(STATUS "Embedded superoptimiser disabled (PENSTOCK_BUILD_EMBEDDED_SUPEROPT=OFF)")
endif()
