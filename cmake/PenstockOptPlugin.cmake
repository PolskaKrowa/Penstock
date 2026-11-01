# ------------------------------------------------- LLVM opt pass plugin (O4)
# Optional loadable plugin exposing Penstock's -O4 IR pipeline to LLVM's
# `opt` (docs/opt-plugin.md).  Requires a matching LLVM development SDK
# (headers, CMake modules, shared libLLVM); when the selected clang
# prefix is compiler-only, the official SDK for the same major is fetched
# instead of silently dropping the target.  Disable with
# -DPENSTOCK_BUILD_OPT_PLUGIN=OFF.
# -------------------------------------------- LLVM opt loadable O4 plugin
penstock_bootstrap_step("6" "OPT PLUGIN CONFIGURATION"
  "Checking for matching LLVM headers, CMake modules, libraries and opt")
# When the selected clang prefix is compiler-only, download the official SDK
# for the same LLVM major rather than silently dropping the plugin target.
set(PENSTOCK_OPT_PLUGIN_BUILT FALSE)
set(PENSTOCK_PLUGIN_LLVM_ROOT "")
set(PENSTOCK_OPT_EXECUTABLE "")
if(PENSTOCK_BUILD_OPT_PLUGIN AND UNIX)
  # Must run before LLVMConfig.cmake imports LLVMExports.cmake; the latter
  # references zstd::libzstd_static in LLVMSupport's link interface.
  penstock_prepare_llvm_zstd_dependency()

  find_package(LLVM CONFIG QUIET
    HINTS
      "${LLVM_ROOT}/lib/cmake/llvm"
      "${LLVM_ROOT}/lib64/cmake/llvm"
      "${LLVM_ROOT}/lib/llvm-${LLVM_MAJOR}/lib/cmake/llvm"
      "/usr/lib/llvm-${LLVM_MAJOR}/lib/cmake/llvm")

  set(_penstock_llvm_sdk_ok FALSE)
  set(_penstock_llvm_include_root "")
  set(_penstock_llvm_opt "")
  if(LLVM_FOUND)
    string(REGEX MATCH "^[0-9]+" _penstock_llvm_package_major "${LLVM_PACKAGE_VERSION}")
    if("${_penstock_llvm_package_major}" STREQUAL "${LLVM_MAJOR}"
       AND EXISTS "${LLVM_CMAKE_DIR}/AddLLVM.cmake")
      set(_penstock_include_candidates
        ${LLVM_INCLUDE_DIRS}
        "${LLVM_ROOT}/include"
        "${LLVM_INSTALL_PREFIX}/include")
      foreach(_include_dir IN LISTS _penstock_include_candidates)
        penstock_has_pass_plugin_header("${_include_dir}" _include_has_pass_plugin_header)
        if(_include_has_pass_plugin_header)
          set(_penstock_llvm_include_root "${_include_dir}")
          break()
        endif()
      endforeach()

      if(LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/opt")
        set(_penstock_llvm_opt "${LLVM_TOOLS_BINARY_DIR}/opt")
        get_filename_component(_penstock_plugin_bin_dir "${LLVM_TOOLS_BINARY_DIR}" ABSOLUTE)
        get_filename_component(PENSTOCK_PLUGIN_LLVM_ROOT "${_penstock_plugin_bin_dir}" DIRECTORY)
      elseif(EXISTS "${LLVM_ROOT}/bin/opt")
        set(_penstock_llvm_opt "${LLVM_ROOT}/bin/opt")
        set(PENSTOCK_PLUGIN_LLVM_ROOT "${LLVM_ROOT}")
      endif()

      if(_penstock_llvm_include_root AND _penstock_llvm_opt)
        set(_penstock_llvm_sdk_ok TRUE)
      endif()
    endif()
  endif()

  # A compiler-only distro package is common (for example, clang without the
  # llvm-<N>-dev package). Fetch the complete release archive for that major;
  # it supplies headers, CMake modules, libraries and a matching opt executable.
  if(NOT _penstock_llvm_sdk_ok AND PENSTOCK_FETCH_LLVM)
    penstock_latest_llvm_release("${LLVM_MAJOR}" _penstock_sdk_version)
    if(_penstock_sdk_version)
      message(STATUS
        "LLVM ${LLVM_MAJOR} development SDK not found; fetching matching "
        "official LLVM ${_penstock_sdk_version} SDK for the opt plugin")
      penstock_download_llvm("${_penstock_sdk_version}" _penstock_downloaded_sdk "plugin")

      # Do not let a cached LLVM_DIR from an unrelated system package override
      # the downloaded SDK selected for this plugin build.
      unset(LLVM_DIR CACHE)
      unset(LLVM_DIR)
      unset(LLVM_FOUND)
      find_package(LLVM CONFIG QUIET
        PATHS "${_penstock_downloaded_sdk}/lib/cmake/llvm"
        NO_DEFAULT_PATH)

      if(LLVM_FOUND)
        string(REGEX MATCH "^[0-9]+" _penstock_llvm_package_major "${LLVM_PACKAGE_VERSION}")
        penstock_has_pass_plugin_header(
          "${_penstock_downloaded_sdk}/include" _downloaded_sdk_has_pass_plugin_header)
        if("${_penstock_llvm_package_major}" STREQUAL "${LLVM_MAJOR}"
           AND EXISTS "${LLVM_CMAKE_DIR}/AddLLVM.cmake"
           AND _downloaded_sdk_has_pass_plugin_header
           AND EXISTS "${_penstock_downloaded_sdk}/bin/opt")
          set(_penstock_llvm_sdk_ok TRUE)
          set(_penstock_llvm_include_root "${_penstock_downloaded_sdk}/include")
          set(_penstock_llvm_opt "${_penstock_downloaded_sdk}/bin/opt")
          set(PENSTOCK_PLUGIN_LLVM_ROOT "${_penstock_downloaded_sdk}")
        endif()
      endif()
    endif()
  endif()

  if(_penstock_llvm_sdk_ok)
    set(PENSTOCK_OPT_EXECUTABLE "${_penstock_llvm_opt}")
    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
    include(AddLLVM)
    add_llvm_pass_plugin(PenstockOptPlugin plugins/opt/PenstockO4Plugin.cpp)
    target_sources(PenstockOptPlugin PRIVATE
      src/precompute.cpp
      src/superopt.cpp
      plugins/opt/PluginState.cpp)
    # A dynamically loaded plugin must use the same LLVM shared library as
    # the host opt process. Linking static LLVM component archives embeds a
    # second copy of LLVM and can register cl::Options twice at dlopen time.
    set(_penstock_llvm_link "")
    if(TARGET LLVM)
      get_target_property(_penstock_llvm_target_type LLVM TYPE)
      if(_penstock_llvm_target_type STREQUAL "SHARED_LIBRARY")
        set(_penstock_llvm_link LLVM)
      elseif(_penstock_llvm_target_type STREQUAL "UNKNOWN_LIBRARY")
        get_target_property(_penstock_llvm_target_location LLVM IMPORTED_LOCATION)
        if(_penstock_llvm_target_location MATCHES "\\.so(\\.[0-9]+)*$" OR
           _penstock_llvm_target_location MATCHES "\\.dylib$")
          set(_penstock_llvm_link LLVM)
        endif()
      endif()
    endif()

    # Some distributions export LLVMConfig.cmake without a monolithic target.
    # Search only the selected SDK's library directories for its shared lib.
    if(NOT _penstock_llvm_link)
      find_library(_penstock_llvm_shared_library
        NAMES LLVM "LLVM-${LLVM_MAJOR}" "LLVM-${LLVM_VERSION_MAJOR}"
        HINTS
          ${LLVM_LIBRARY_DIRS}
          "${PENSTOCK_PLUGIN_LLVM_ROOT}/lib"
          "${PENSTOCK_PLUGIN_LLVM_ROOT}/lib64"
          "${LLVM_ROOT}/lib"
          "${LLVM_ROOT}/lib64"
          "${LLVM_INSTALL_PREFIX}/lib"
          "${LLVM_INSTALL_PREFIX}/lib64"
        NO_DEFAULT_PATH)
      if(_penstock_llvm_shared_library)
        set(_penstock_llvm_link "${_penstock_llvm_shared_library}")
      endif()
    endif()

    if(NOT _penstock_llvm_link)
      message(FATAL_ERROR
        "PenstockOptPlugin requires the matching shared libLLVM used by opt. "
        "A static-only LLVM SDK is not safe for a loadable plugin because it "
        "duplicates LLVM global command-line option registrations. Install "
        "the matching shared LLVM development library or disable the plugin "
        "with -DPENSTOCK_BUILD_OPT_PLUGIN=OFF.")
    endif()

    target_link_libraries(PenstockOptPlugin PRIVATE "${_penstock_llvm_link}")
    message(STATUS "         LLVM shared library: ${_penstock_llvm_link}")
    target_include_directories(PenstockOptPlugin BEFORE PRIVATE
      "${_penstock_llvm_include_root}"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      ${LLVM_INCLUDE_DIRS})
    target_compile_features(PenstockOptPlugin PRIVATE cxx_std_17)
    target_compile_definitions(PenstockOptPlugin PRIVATE
      "PENSTOCK_PLUGIN_LLVM_ROOT=\"${PENSTOCK_PLUGIN_LLVM_ROOT}\""
      "PENSTOCK_PLUGIN_CLANG=\"${PENSTOCK_PLUGIN_LLVM_ROOT}/bin/clang\"")
    set_target_properties(PenstockOptPlugin PROPERTIES
      OUTPUT_NAME "PenstockOptPlugin"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/opt")
    set(PENSTOCK_OPT_PLUGIN_BUILT TRUE)
    penstock_bootstrap_ok("Penstock O4 pass plugin configured for LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS
      "Penstock opt plugin : enabled (LLVM ${LLVM_PACKAGE_VERSION}, "
      "SDK ${PENSTOCK_PLUGIN_LLVM_ROOT})")
  else()
    penstock_bootstrap_info("Matching development SDK unavailable; compiler backend remains usable")
    if(PENSTOCK_FETCH_LLVM)
      set(_plugin_sdk_hint
        "No supported release SDK is available for LLVM ${LLVM_MAJOR}. ")
    else()
      set(_plugin_sdk_hint
        "Enable automatic SDK download with -DPENSTOCK_FETCH_LLVM=ON, ")
    endif()
    message(WARNING
      "Penstock opt plugin not built: ${_plugin_sdk_hint}or install the matching "
      "LLVM development package and set LLVM_DIR explicitly. The penstock "
      "compiler remains usable; alternatively use -DPENSTOCK_BUILD_OPT_PLUGIN=OFF.")
  endif()
elseif(PENSTOCK_BUILD_OPT_PLUGIN AND NOT UNIX)
  penstock_bootstrap_info("Opt plugin integration is currently supported on POSIX hosts only")
  message(WARNING "Penstock's current opt plugin integration requires a POSIX host; disabled.")
else()
  penstock_bootstrap_info("Opt plugin explicitly disabled by PENSTOCK_BUILD_OPT_PLUGIN=OFF")
endif()
