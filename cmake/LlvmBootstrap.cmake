# ------------------------------------------------- backend toolchain bootstrap
# Resolves the LLVM/Clang backend used both as penstock's code-generation
# backend (driven as a subprocess at runtime) and as the default host
# compiler for the tools themselves:
#
#   1. explicit -DLLVM_ROOT=... / $LLVM_ROOT (a leading ~/ is expanded)
#   2. local LLVM autodetection: ../tools/llvmNN beside the project, then
#      system prefixes /usr/lib/llvm-NN, newest first (majors 23..19)
#   3. verified download of an official LLVM release into the
#      PENSTOCK_LLVM_DOWNLOAD_DIR cache when no local toolchain exists
#
# If a local clang exists without its development SDK, the matching SDK is
# fetched automatically so the opt plugin can still be built (see
# cmake/PenstockOptPlugin.cmake).  A configure-time smoke test compiles a
# trivial program with the selected clang so broken toolchains fail
# immediately with a targeted hint instead of mid-build.
#
# This file is included from the top-level CMakeLists.txt BEFORE project()
# so the backend's clang++ can be selected as the host compiler; it
# executes in the includer's scope.
# Keep configure-time bootstrap output consistent across CMake 3.16+.
# The output is deliberately plain ASCII so it remains readable in IDEs,
# redirected logs and terminals without ANSI colour support.
function(penstock_bootstrap_banner)
  message(STATUS "")
  message(STATUS "  +----------------------------------------------------------+")
  message(STATUS "  |                    PENSTOCK BUILD                        |")
  message(STATUS "  |                LLVM TOOLCHAIN BOOTSTRAP                  |")
  message(STATUS "  +----------------------------------------------------------+")
endfunction()

function(penstock_bootstrap_step step title detail)
  message(STATUS "")
  message(STATUS "  [${step}/6] ${title}")
  if(NOT "${detail}" STREQUAL "")
    message(STATUS "         ${detail}")
  endif()
endfunction()

function(penstock_bootstrap_ok detail)
  message(STATUS "         [OK] ${detail}")
endfunction()

function(penstock_bootstrap_info detail)
  message(STATUS "         [INFO] ${detail}")
endfunction()

function(penstock_bootstrap_plugin_stage title detail)
  message(STATUS "")
  message(STATUS "  [PENSTOCK / OPT PLUGIN] ${title}")
  if(NOT "${detail}" STREQUAL "")
    message(STATUS "         ${detail}")
  endif()
endfunction()

# Download to a temporary path, then atomically promote it only after the
# transport has succeeded. curl gives a readable progress bar; CMake's native
# downloader remains available on machines without curl.
function(penstock_download_file url destination description show_progress timeout_seconds)
  set(_partial "${destination}.part")
  file(REMOVE "${_partial}")

  if(_penstock_curl_executable AND
     NOT "${_penstock_curl_executable}" MATCHES "-NOTFOUND$")
    set(_curl_args
      --fail
      --location
      --retry 3
      --retry-delay 2
      --connect-timeout 30
      --max-time "${timeout_seconds}"
      --user-agent "Penstock-CMake")
    if(show_progress)
      list(APPEND _curl_args --progress-bar)
    else()
      list(APPEND _curl_args --silent --show-error)
    endif()
    list(APPEND _curl_args --output "${_partial}" "${url}")

    execute_process(
      COMMAND "${_penstock_curl_executable}" ${_curl_args}
      RESULT_VARIABLE _curl_result)
    if(NOT "${_curl_result}" STREQUAL "0")
      file(REMOVE "${_partial}")
      message(FATAL_ERROR
        "Penstock: ${description} failed (curl result: ${_curl_result}). "
        "See the transfer diagnostic above, check network/proxy access, or "
        "set LLVM_ROOT to a local installation.")
    endif()
  else()
    set(_download_status "")
    if(show_progress)
      file(DOWNLOAD "${url}" "${_partial}"
        STATUS _download_status
        SHOW_PROGRESS
        TLS_VERIFY ON
        TIMEOUT "${timeout_seconds}")
    else()
      file(DOWNLOAD "${url}" "${_partial}"
        STATUS _download_status
        TLS_VERIFY ON
        TIMEOUT "${timeout_seconds}")
    endif()

    # STATUS is documented as <numeric-code>;<message>, but guard every
    # access: some CMake/network backends can leave the message empty.
    set(_download_code "unknown")
    set(_download_message "CMake did not provide an error description")
    if(DEFINED _download_status AND NOT "${_download_status}" STREQUAL "")
      list(LENGTH _download_status _download_status_length)
      if(_download_status_length GREATER 0)
        list(GET _download_status 0 _download_code)
      endif()
      if(_download_status_length GREATER 1)
        list(GET _download_status 1 _download_message)
      endif()
    endif()

    if(NOT "${_download_code}" STREQUAL "0")
      file(REMOVE "${_partial}")
      if("${_download_message}" STREQUAL "" OR
         "${_download_message}" STREQUAL "NOTFOUND")
        set(_download_message
          "CMake's downloader returned no detail (status ${_download_code})")
      endif()
      message(FATAL_ERROR
        "Penstock: ${description} failed (CMake download status "
        "${_download_code}: ${_download_message}). Check network/proxy access, "
        "or set LLVM_ROOT to a local installation.")
    endif()
  endif()

  if(NOT EXISTS "${_partial}" OR IS_DIRECTORY "${_partial}")
    file(REMOVE "${_partial}")
    message(FATAL_ERROR
      "Penstock: ${description} reported success but produced no file.")
  endif()
  file(RENAME "${_partial}" "${destination}")
endfunction()

find_program(_penstock_curl_executable NAMES curl)

# LLVM's exported LLVMSupport target can reference zstd::libzstd_static.
# Some binary LLVM packages do not arrange for that dependency target to be
# created by LLVMConfig.cmake, so make it available before find_package(LLVM).
# Prefer an installed package/library; only build zstd from source as a last
# resort. The target name is the public contract used by LLVM's export files.
function(penstock_prepare_llvm_zstd_dependency)
  if(TARGET zstd::libzstd_static)
    return()
  endif()

  find_package(zstd CONFIG QUIET)
  if(TARGET zstd::libzstd_static)
    message(STATUS "Penstock: zstd::libzstd_static is already available")
    return()
  endif()

  # Some zstd packages export a generic or shared target rather than the
  # static target name recorded in LLVMExports.cmake. Bridge that target name
  # without changing the package's own imported target.
  foreach(_zstd_target zstd::libzstd zstd::libzstd_shared)
    if(TARGET ${_zstd_target})
      add_library(zstd::libzstd_static INTERFACE IMPORTED GLOBAL)
      set_property(TARGET zstd::libzstd_static PROPERTY
        INTERFACE_LINK_LIBRARIES "${_zstd_target}")
      message(STATUS "Penstock: using zstd dependency target ${_zstd_target}")
      return()
    endif()
  endforeach()

  find_path(_penstock_zstd_include_dir NAMES zstd.h)

  # Prefer a real static archive when the development package provides one.
  set(_penstock_saved_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_STATIC_LIBRARY_SUFFIX}")
  find_library(_penstock_zstd_static_library NAMES zstd zstd_static)
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${_penstock_saved_library_suffixes}")
  if(_penstock_zstd_static_library)
    add_library(zstd::libzstd_static UNKNOWN IMPORTED GLOBAL)
    set_target_properties(zstd::libzstd_static PROPERTIES
      IMPORTED_LOCATION "${_penstock_zstd_static_library}")
    if(_penstock_zstd_include_dir)
      set_property(TARGET zstd::libzstd_static PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${_penstock_zstd_include_dir}")
    endif()
    message(STATUS
      "Penstock: using system zstd static library ${_penstock_zstd_static_library}")
    return()
  endif()

  # A shared-only system installation can still satisfy the link interface;
  # keep the compatibility target INTERFACE so CMake links the real library.
  find_library(_penstock_zstd_shared_library NAMES zstd)
  if(_penstock_zstd_shared_library)
    add_library(zstd::libzstd_static INTERFACE IMPORTED GLOBAL)
    set_property(TARGET zstd::libzstd_static PROPERTY
      INTERFACE_LINK_LIBRARIES "${_penstock_zstd_shared_library}")
    if(_penstock_zstd_include_dir)
      set_property(TARGET zstd::libzstd_static PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${_penstock_zstd_include_dir}")
    endif()
    message(STATUS
      "Penstock: using system zstd shared library ${_penstock_zstd_shared_library}")
    return()
  endif()

  if(NOT PENSTOCK_FETCH_ZSTD)
    message(FATAL_ERROR
      "The selected LLVM SDK requires zstd::libzstd_static, but no usable "
      "system zstd package/library was found. Install the zstd development "
      "package or reconfigure with -DPENSTOCK_FETCH_ZSTD=ON.")
  endif()

  include(FetchContent)
  set(ZSTD_BUILD_STATIC ON CACHE BOOL "Build zstd's static library" FORCE)
  set(ZSTD_BUILD_SHARED OFF CACHE BOOL "Do not build zstd's shared library" FORCE)
  set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Do not build zstd command-line tools" FORCE)
  set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Do not build zstd tests" FORCE)
  set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "Do not build zstd contrib tools" FORCE)

  message(STATUS
    "Penstock: fetching zstd ${PENSTOCK_ZSTD_VERSION} for the LLVM SDK")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.18")
    # SOURCE_SUBDIR is supported from CMake 3.18; zstd keeps its project file
    # under build/cmake rather than at the repository root.
    FetchContent_Declare(penstock_zstd
      GIT_REPOSITORY https://github.com/facebook/zstd.git
      GIT_TAG "v${PENSTOCK_ZSTD_VERSION}"
      GIT_SHALLOW TRUE
      SOURCE_SUBDIR build/cmake)
    FetchContent_MakeAvailable(penstock_zstd)
  else()
    # CMake 3.16/3.17: populate manually, then add zstd's CMake subdirectory.
    FetchContent_Declare(penstock_zstd
      GIT_REPOSITORY https://github.com/facebook/zstd.git
      GIT_TAG "v${PENSTOCK_ZSTD_VERSION}"
      GIT_SHALLOW TRUE)
    FetchContent_GetProperties(penstock_zstd)
    if(NOT penstock_zstd_POPULATED)
      FetchContent_Populate(penstock_zstd)
      add_subdirectory(
        "${penstock_zstd_SOURCE_DIR}/build/cmake"
        "${penstock_zstd_BINARY_DIR}"
        EXCLUDE_FROM_ALL)
    endif()
  endif()

  if(TARGET libzstd_static AND NOT TARGET zstd::libzstd_static)
    add_library(zstd::libzstd_static ALIAS libzstd_static)
  endif()
  if(NOT TARGET zstd::libzstd_static)
    message(FATAL_ERROR
      "Fetched zstd ${PENSTOCK_ZSTD_VERSION}, but it did not define "
      "libzstd_static. Check the pinned zstd release and its CMake build.")
  endif()
endfunction()

# LLVM moved the public new-pass-manager plugin header from
# llvm/Passes/PassPlugin.h to llvm/Plugins/PassPlugin.h. Support both layouts
# so downloaded LLVM 23+ SDKs and older system LLVM installations work.
function(penstock_has_pass_plugin_header include_root out_var)
  if(EXISTS "${include_root}/llvm/Plugins/PassPlugin.h" OR
     EXISTS "${include_root}/llvm/Passes/PassPlugin.h")
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

# Pick the latest official release we know for a selected LLVM major. This is
# used only when a user supplied clang exists but its development SDK does not.
function(penstock_latest_llvm_release major out_var)
  if("${major}" STREQUAL "23")
    set(_version "23.1.4")
  elseif("${major}" STREQUAL "22")
    set(_version "22.1.8")
  elseif("${major}" STREQUAL "21")
    set(_version "21.1.8")
  elseif("${major}" STREQUAL "20")
    set(_version "20.1.8")
  elseif("${major}" STREQUAL "19")
    set(_version "19.1.7")
  else()
    set(_version "")
  endif()
  set(${out_var} "${_version}" PARENT_SCOPE)
endfunction()

# Download and unpack one official LLVM release. The SHA-256 digest is read
# from GitHub release metadata and checked independently before extraction,
# regardless of whether curl or CMake performs the transfer.
function(penstock_download_llvm version out_root)
  if(ARGC GREATER 2)
    set(_download_purpose "${ARGV2}")
  else()
    set(_download_purpose "primary")
  endif()

  if(NOT PENSTOCK_FETCH_LLVM)
    message(FATAL_ERROR
      "LLVM ${version} is required but PENSTOCK_FETCH_LLVM=OFF. Install a full "
      "LLVM/Clang toolchain or reconfigure with -DPENSTOCK_FETCH_LLVM=ON.")
  endif()
  if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
      "Automatic LLVM downloads currently support Linux hosts only. "
      "Set LLVM_ROOT to a local LLVM installation.")
  endif()
  if(NOT "${version}" MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Invalid PENSTOCK LLVM release version: '${version}'")
  endif()

  # CMAKE_HOST_SYSTEM_PROCESSOR is populated by project() on some CMake
  # generators/platforms, but this bootstrap runs before project() so that
  # clang can be selected as the host compiler. Fall back to uname -m here.
  set(_host_cpu "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  if(NOT _host_cpu)
    execute_process(
      COMMAND uname -m
      RESULT_VARIABLE _uname_rc
      OUTPUT_VARIABLE _host_cpu
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(NOT _uname_rc EQUAL 0)
      set(_host_cpu "unknown")
    endif()
  endif()
  string(TOLOWER "${_host_cpu}" _host_cpu)
  if(_host_cpu MATCHES "^(x86_64|amd64)$")
    set(_llvm_arch "X64")
  elseif(_host_cpu MATCHES "^(aarch64|arm64)$")
    set(_llvm_arch "ARM64")
  else()
    message(FATAL_ERROR
      "No automatic LLVM ${version} binary is configured for host CPU "
      "'${CMAKE_HOST_SYSTEM_PROCESSOR}'. Set LLVM_ROOT to a local installation.")
  endif()

  set(_download_dir "${PENSTOCK_LLVM_DOWNLOAD_DIR}/llvm-${version}-${_llvm_arch}")
  set(_archive_name "LLVM-${version}-Linux-${_llvm_arch}.tar.xz")
  set(_archive "${_download_dir}/${_archive_name}")
  set(_release_json "${_download_dir}/release.json")
  set(_extract_dir "${_download_dir}/extract")
  set(_root "${_extract_dir}/LLVM-${version}-Linux-${_llvm_arch}")

  if(_download_purpose STREQUAL "primary")
    penstock_bootstrap_step("2" "LLVM SDK ACQUISITION"
      "Preparing LLVM ${version} (${_llvm_arch})")
  else()
    penstock_bootstrap_plugin_stage("SUPPLEMENTAL SDK ACQUISITION"
      "Preparing matching LLVM ${version} development SDK (${_llvm_arch})")
  endif()

  # Reuse a previously extracted SDK without touching the network.
  penstock_has_pass_plugin_header("${_root}/include" _root_has_pass_plugin_header)
  if(EXISTS "${_root}/bin/clang" AND
     EXISTS "${_root}/bin/clang++" AND
     EXISTS "${_root}/bin/opt" AND
     _root_has_pass_plugin_header AND
     EXISTS "${_root}/lib/cmake/llvm/LLVMConfig.cmake")
    penstock_bootstrap_ok("Reusing extracted SDK: ${_root}")
    if(_download_purpose STREQUAL "primary")
      penstock_bootstrap_step("3" "ARCHIVE INTEGRITY"
        "Previously acquired SDK cache; archive was verified on download")
      penstock_bootstrap_ok("Cached SDK layout is complete")
      penstock_bootstrap_step("4" "SDK EXTRACTION"
        "No extraction required; reusing the existing SDK")
      penstock_bootstrap_ok("Extraction skipped")
    else()
      penstock_bootstrap_plugin_stage("SDK CACHE" "Existing extracted SDK is complete")
    endif()
    set(${out_root} "${_root}" PARENT_SCOPE)
    return()
  endif()

  file(MAKE_DIRECTORY "${_download_dir}")
  if(NOT EXISTS "${_release_json}")
    set(_release_api
      "https://api.github.com/repos/llvm/llvm-project/releases/tags/llvmorg-${version}")
    penstock_bootstrap_info("Reading official release metadata from GitHub")
    penstock_download_file(
      "${_release_api}" "${_release_json}"
      "LLVM ${version} release metadata" FALSE 120)
  endif()

  file(READ "${_release_json}" _release_contents)
  # GitHub's REST API may emit compact or pretty-printed JSON. Do not search
  # for a literal `"name": "..."` (with one particular space layout): JSON
  # whitespace is insignificant and the response format may change.
  string(REPLACE "." "\\." _archive_name_regex "${_archive_name}")
  string(REGEX MATCH
    "\"name\"[ \t\r\n]*:[ \t\r\n]*\"${_archive_name_regex}\""
    _asset_name_field "${_release_contents}")
  if(NOT "${_asset_name_field}" STREQUAL "")
    string(FIND "${_release_contents}" "${_asset_name_field}" _asset_name_pos)
  else()
    set(_asset_name_pos -1)
  endif()

  if(_asset_name_pos LESS 0)
    # Metadata can be stale if a release was published before its binary
    # assets finished uploading. Refresh once before reporting an asset error.
    file(REMOVE "${_release_json}")
    set(_release_api
      "https://api.github.com/repos/llvm/llvm-project/releases/tags/llvmorg-${version}")
    penstock_bootstrap_info("Refreshing release metadata to check for newly published assets")
    penstock_download_file(
      "${_release_api}" "${_release_json}"
      "refreshed LLVM ${version} release metadata" FALSE 120)
    file(READ "${_release_json}" _release_contents)
    string(REGEX MATCH
      "\"name\"[ \t\r\n]*:[ \t\r\n]*\"${_archive_name_regex}\""
      _asset_name_field "${_release_contents}")
    if(NOT "${_asset_name_field}" STREQUAL "")
      string(FIND "${_release_contents}" "${_asset_name_field}" _asset_name_pos)
    else()
      set(_asset_name_pos -1)
    endif()
  endif()
  if(_asset_name_pos LESS 0)
    # Include the relevant asset names from the response. This makes API
    # errors, incomplete releases and filename mismatches distinguishable.
    string(REGEX MATCHALL
      "\"name\"[ \t\r\n]*:[ \t\r\n]*\"[^\"]+\""
      _release_asset_fields "${_release_contents}")
    set(_release_asset_summary "")
    foreach(_asset_field IN LISTS _release_asset_fields)
      string(REGEX REPLACE
        "^\"name\"[ \t\r\n]*:[ \t\r\n]*\"" ""
        _asset_label "${_asset_field}")
      string(REGEX REPLACE "\"$" "" _asset_label "${_asset_label}")
      if(_asset_label MATCHES "(Linux|linux).*(tar\\.xz|tar\\.zst)$")
        if(_release_asset_summary STREQUAL "")
          set(_release_asset_summary "${_asset_label}")
        else()
          string(APPEND _release_asset_summary ", ${_asset_label}")
        endif()
      endif()
    endforeach()
    if(_release_asset_summary STREQUAL "")
      set(_release_asset_summary "<no Linux archive names found in GitHub API response>")
    endif()
    message(FATAL_ERROR
      "Could not find ${_archive_name} in the LLVM ${version} GitHub release "
      "metadata. Linux archives reported: ${_release_asset_summary}. "
      "Check ${_release_json}; it may contain an API rate-limit/error response. "
      "You can also set LLVM_ROOT to a local LLVM installation.")
  endif()

  string(SUBSTRING "${_release_contents}" ${_asset_name_pos} -1 _asset_tail)
  string(REGEX MATCH
    "\"digest\"[ \\t\\r\\n]*:[ \\t\\r\\n]*\"sha256:([0-9A-Fa-f]+)\""
    _digest_match "${_asset_tail}")
  set(_expected_sha256 "${CMAKE_MATCH_1}")
  string(LENGTH "${_expected_sha256}" _sha256_length)
  if(NOT _sha256_length EQUAL 64 OR NOT _expected_sha256 MATCHES "^[0-9A-Fa-f]+$")
    message(FATAL_ERROR
      "GitHub did not publish a SHA-256 digest for ${_archive_name}; refusing "
      "to use an unverified LLVM download. Set LLVM_ROOT to a local install.")
  endif()
  string(TOLOWER "${_expected_sha256}" _expected_sha256)

  set(_need_download TRUE)
  if(EXISTS "${_archive}")
    file(SHA256 "${_archive}" _cached_sha256)
    string(TOLOWER "${_cached_sha256}" _cached_sha256)
    if(_cached_sha256 STREQUAL _expected_sha256)
      set(_need_download FALSE)
      penstock_bootstrap_info("Found a previously downloaded archive")
    else()
      file(REMOVE "${_archive}")
      penstock_bootstrap_info("Cached archive digest did not match; downloading a clean copy")
    endif()
  endif()

  if(_need_download)
    set(_archive_url
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-${version}/${_archive_name}")
    penstock_bootstrap_info("Official release archive: ${_archive_name}")
    penstock_download_file(
      "${_archive_url}" "${_archive}"
      "LLVM ${version} SDK archive" TRUE 3600)
  endif()

  if(_download_purpose STREQUAL "primary")
    penstock_bootstrap_step("3" "ARCHIVE INTEGRITY"
      "Verifying the downloaded archive against GitHub's SHA-256 digest")
  else()
    penstock_bootstrap_plugin_stage("ARCHIVE INTEGRITY"
      "Verifying the SDK archive against GitHub's SHA-256 digest")
  endif()
  file(SHA256 "${_archive}" _downloaded_sha256)
  string(TOLOWER "${_downloaded_sha256}" _downloaded_sha256)
  if(NOT _downloaded_sha256 STREQUAL _expected_sha256)
    file(REMOVE "${_archive}")
    message(FATAL_ERROR
      "LLVM ${version} archive SHA-256 mismatch. The archive has been removed; "
      "retry the configure or set LLVM_ROOT to a local installation.")
  endif()
  penstock_bootstrap_ok("SHA-256 ${_downloaded_sha256}")

  if(_download_purpose STREQUAL "primary")
    penstock_bootstrap_step("4" "SDK EXTRACTION"
      "Unpacking ${_archive_name} into ${_extract_dir}")
  else()
    penstock_bootstrap_plugin_stage("SDK EXTRACTION"
      "Unpacking ${_archive_name} into ${_extract_dir}")
  endif()
  penstock_has_pass_plugin_header("${_root}/include" _root_has_pass_plugin_header)
  if(NOT EXISTS "${_root}/bin/clang" OR
     NOT _root_has_pass_plugin_header OR
     NOT EXISTS "${_root}/lib/cmake/llvm/LLVMConfig.cmake")
    file(REMOVE_RECURSE "${_extract_dir}")
    file(MAKE_DIRECTORY "${_extract_dir}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xJf "${_archive}"
      WORKING_DIRECTORY "${_extract_dir}"
      RESULT_VARIABLE _extract_rc
      ERROR_VARIABLE _extract_error)
    if(NOT _extract_rc EQUAL 0)
      message(FATAL_ERROR
        "Could not extract ${_archive}: ${_extract_error}. "
        "Ensure CMake was built with xz archive support, or provide LLVM_ROOT.")
    endif()
  else()
    penstock_bootstrap_info("Extracted SDK already exists; skipping unpack")
  endif()

  penstock_has_pass_plugin_header("${_root}/include" _root_has_pass_plugin_header)
  if(NOT EXISTS "${_root}/bin/clang" OR
     NOT EXISTS "${_root}/bin/clang++" OR
     NOT EXISTS "${_root}/bin/opt" OR
     NOT _root_has_pass_plugin_header OR
     NOT EXISTS "${_root}/lib/cmake/llvm/LLVMConfig.cmake")
    message(FATAL_ERROR
      "The downloaded LLVM ${version} archive did not contain the expected "
      "clang/clang++, opt, PassPlugin.h and LLVMConfig.cmake files. "
      "Remove ${_extract_dir} and retry, or set LLVM_ROOT manually.")
  endif()
  penstock_bootstrap_ok("LLVM ${version} SDK extracted and validated")

  set(${out_root} "${_root}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------- LLVM_ROOT
penstock_bootstrap_banner()
penstock_bootstrap_step("1" "TOOLCHAIN DISCOVERY"
  "Checking LLVM_ROOT, the environment, standard install prefixes and the SDK cache")

# Resolve the backend BEFORE project():
#   1. explicit -DLLVM_ROOT=... / LLVM_ROOT environment override
#   2. complete local LLVM prefixes (newest first)
#   3. official LLVM binary release download (when enabled)
# A downloaded release contains the compiler and matching LLVM development SDK.
if(DEFINED LLVM_ROOT)
  set(_llvm_src "cache")
elseif(DEFINED ENV{LLVM_ROOT})
  set(LLVM_ROOT "$ENV{LLVM_ROOT}")
  set(_llvm_src "env")
else()
  set(_llvm_src "probe")
  foreach(_maj 23 22 21 20 19)
    set(_pfx_list
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/llvm${_maj}/usr/lib/llvm-${_maj}"
        "/usr/lib/llvm-${_maj}")
    foreach(_pfx IN LISTS _pfx_list)
      if(EXISTS "${_pfx}/bin/clang" AND EXISTS "${_pfx}/bin/clang++")
        set(LLVM_ROOT "${_pfx}")
        break()
      endif()
    endforeach()
    if(LLVM_ROOT)
      break()
    endif()
  endforeach()
endif()

# Accept `~/...` spellings for explicit paths.
if(LLVM_ROOT MATCHES "^~(/|$)"
   AND DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  string(REGEX REPLACE "^~" "" _llvm_tilde_tail "${LLVM_ROOT}")
  set(LLVM_ROOT "$ENV{HOME}${_llvm_tilde_tail}")
endif()

if(NOT LLVM_ROOT OR NOT EXISTS "${LLVM_ROOT}/bin/clang")
  if(_llvm_src STREQUAL "probe" AND PENSTOCK_FETCH_LLVM)
    penstock_download_llvm("${PENSTOCK_LLVM_VERSION}" LLVM_ROOT)
    set(_llvm_src "download")
  elseif(_llvm_src STREQUAL "probe")
    message(FATAL_ERROR
      "penstock: no supported LLVM toolchain found (19-23). Either:\n"
      "  * cmake -B build -DLLVM_ROOT=/path/to/llvm-N,\n"
      "  * export LLVM_ROOT=/path/to/llvm-N,\n"
      "  * enable automatic download (default) with -DPENSTOCK_FETCH_LLVM=ON,\n"
      "  * or disable downloads with -DPENSTOCK_FETCH_LLVM=OFF and install LLVM locally.")
  else()
    message(FATAL_ERROR
      "penstock: pinned LLVM_ROOT=${LLVM_ROOT} (from ${_llvm_src}) has no "
      "bin/clang — point it at a full LLVM install prefix, or clear it with "
      "cmake -B build -ULLVM_ROOT to re-enable autodetection.")
  endif()
endif()
if(NOT EXISTS "${LLVM_ROOT}/bin/clang++")
  message(FATAL_ERROR
    "penstock: LLVM_ROOT=${LLVM_ROOT} has bin/clang but no bin/clang++ — "
    "point LLVM_ROOT at a full clang/clang++ install prefix.")
endif()

# A downloaded primary SDK reports stages 2-4 inside penstock_download_llvm().
# When an existing local toolchain wins discovery, report those stages as cache /
# local-install decisions instead of leaving gaps in the bootstrap presentation.
if(NOT _llvm_src STREQUAL "download")
  penstock_bootstrap_step("2" "LLVM SDK ACQUISITION"
    "Using the selected local toolchain: ${LLVM_ROOT}")
  penstock_bootstrap_ok("No download required")
  penstock_bootstrap_step("3" "ARCHIVE INTEGRITY"
    "No archive was downloaded; local installation selected")
  penstock_bootstrap_info("Archive hash verification not applicable")
  penstock_bootstrap_step("4" "SDK EXTRACTION"
    "The selected LLVM installation is already unpacked")
  penstock_bootstrap_ok("No extraction required")
endif()

# --------------------------------------------------- backend smoke test
# A backend clang that EXISTS is not necessarily one that WORKS.  Two
# real-world failure modes, both invisible to CMake's own compiler
# self-test (its test program contains no #include):
#   * a self-built clang whose baked default target triple is empty ->
#     "error: unknown target triple 'unknown'"
#   * a prefix missing its builtin headers (lib/clang/<N>/include) ->
#     "fatal error: 'stddef.h' file not found" — stddef.h is a CLANG
#     builtin header, not a glibc one; glibc's stdio.h includes it.
# Both otherwise surface only at [4%] (the runtime-IR step) or at
# penstock runtime, far from their cause.  Run BEFORE project() so a
# broken backend also pre-empts the (less helpful) host-compiler
# self-test failure.
penstock_bootstrap_step("5" "COMPILER VALIDATION"
  "Compiling and linking a small C program with ${LLVM_ROOT}/bin/clang")
set(_smoke_dir "${CMAKE_BINARY_DIR}/backend-smoke")
file(MAKE_DIRECTORY "${_smoke_dir}")
file(WRITE "${_smoke_dir}/smoke.c"
"#include <stddef.h>
#include <stdio.h>
int main(void){printf(\"ok\\n\");return 0;}
")
execute_process(
  COMMAND "${LLVM_ROOT}/bin/clang" -x c "${_smoke_dir}/smoke.c"
          -o "${_smoke_dir}/smoke.bin"
  RESULT_VARIABLE _smoke_rc
  OUTPUT_QUIET
  ERROR_VARIABLE _smoke_err)
if(NOT _smoke_rc EQUAL 0)
  execute_process(
    COMMAND "${LLVM_ROOT}/bin/clang" -print-resource-dir
    RESULT_VARIABLE _smoke_rdir_rc
    OUTPUT_VARIABLE _smoke_rdir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT _smoke_rdir_rc EQUAL 0)
    set(_smoke_rdir "<clang -print-resource-dir failed as well>")
  endif()
  set(_smoke_hint "")
  if(_smoke_err MATCHES "unknown target triple")
    string(APPEND _smoke_hint "
  This clang was built with an EMPTY default target triple — an LLVM
  configure problem, not a Freshet one (llvm::Triple::normalize(\"\") is
  the literal string 'unknown').  Reconfigure that LLVM tree in a FRESH
  build directory with -DLLVM_ENABLE_PROJECTS=clang and
  -DLLVM_TARGETS_TO_BUILD=<your-target>, passing NO -DLLVM_*TRIPLE*
  setting, then rebuild — or point LLVM_ROOT at a distro clang instead.")
  elseif(_smoke_err MATCHES "'(stddef|stdarg|stdbool|stdint|inttypes|float)\\.h' .*not found")
    string(APPEND _smoke_hint "
  This clang is missing its BUILTIN headers: <stddef.h> and friends are
  compiler headers that live in the resource dir
      ${_smoke_rdir}
  which is empty or absent on this machine (glibc's stdio.h includes
  <stddef.h>, so even a hello-world C program fails).  On Debian/Ubuntu
  install libclang-common-<major>-dev; on Arch/Fedora reinstall the
  clang package; for a self-built LLVM build the clang-resource-headers
  target.  Stopgap: copy clang/lib/Headers/*.h from an LLVM source
  checkout into that resource dir.")
  elseif(_smoke_err MATCHES "'stdio\\.h' .*not found")
    string(APPEND _smoke_hint "
  The SYSTEM C headers are missing (glibc development package).
  Install libc6-dev (Debian/Ubuntu) or glibc-devel (Fedora).")
  endif()
  message(FATAL_ERROR
    "penstock: the selected backend clang does not work:
  ${LLVM_ROOT}/bin/clang
It failed to compile a trivial smoke program:
${_smoke_err}
Freshet drives this clang at build time (runtime .ll cache) AND at
penstock runtime (backend), so this must be fixed first.${_smoke_hint}")
else()
  set(PENSTOCK_SMOKE_OK "ok (trivial program, builtin headers, link)")
  penstock_bootstrap_ok("clang compiled, included system/builtin headers and linked successfully")
endif()
