# Helpers provided by the installed Penstock CMake package.
#
# penstock_add_executable(<target> <source.fsh>
#   [OUTPUT_NAME <name>]
#   [OPTIONS <penstock-option>...]
#   [DEPENDS <file>...])
#
# Creates a build target which asks the Penstock compiler to build the
# Freshet source into penstock-bin/ under the current binary directory.
function(penstock_add_executable target source)
  if(NOT TARGET Penstock::compiler)
    message(FATAL_ERROR "penstock_add_executable requires find_package(Penstock CONFIG)")
  endif()
  if(NOT target OR NOT source)
    message(FATAL_ERROR "usage: penstock_add_executable(target source.fsh [...])")
  endif()

  set(_one_value_args OUTPUT_NAME)
  set(_multi_value_args OPTIONS DEPENDS)
  cmake_parse_arguments(PA "" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

  if(IS_ABSOLUTE "${source}")
    set(_source "${source}")
  else()
    get_filename_component(_source "${source}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  if(NOT EXISTS "${_source}")
    message(FATAL_ERROR "Penstock source does not exist: ${_source}")
  endif()

  if(PA_OUTPUT_NAME)
    set(_output_name "${PA_OUTPUT_NAME}")
  else()
    set(_output_name "${target}")
  endif()
  # Keep the generated file separate from the phony custom target name;
  # otherwise Make generators can report a circular target/file dependency.
  set(_output_dir "${CMAKE_CURRENT_BINARY_DIR}/penstock-bin")
  set(_output "${_output_dir}/${_output_name}${CMAKE_EXECUTABLE_SUFFIX}")

  # Make local Freshet includes work from the source tree without requiring
  # every project to add its own -I for the source directory.
  add_custom_command(
    OUTPUT "${_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_dir}"
    COMMAND "$<TARGET_FILE:Penstock::compiler>"
            "-I${CMAKE_CURRENT_SOURCE_DIR}"
            ${PA_OPTIONS}
            "${_source}" -o "${_output}"
    DEPENDS "${_source}" ${PA_DEPENDS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Penstock compiling ${source} -> ${_output_name}"
    VERBATIM
    COMMAND_EXPAND_LISTS)
  add_custom_target(${target} ALL DEPENDS "${_output}")
  set_property(TARGET ${target} PROPERTY PENSTOCK_OUTPUT "${_output}")
  set(${target}_OUTPUT "${_output}" PARENT_SCOPE)
endfunction()
