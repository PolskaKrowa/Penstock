# ------------------------------------------------------------- test / bench
# make-parity entry points: `check` runs the regression suite (tests/run.sh),
# `bench` the AoS/SoA/C benchmarks (tests/bench.sh).  Both export the
# resolved LLVM_ROOT so penstock and the C baseline in bench.sh run against
# the same backend the build used.  CTest also reserves a `test` target,
# hence the name `check`.
# -------------------------------------------------------- test/bench targets
# Same entry points as the old `make test` / `make bench`.  CTest also
# reserves a `test` target (ctest), hence the make-parity name `check`.
# The suite drives bin/freshet-language-server (LSP navigation smoke test,
# tests/run.sh), so `check` must depend on it too: a fresh-tree
# `cmake --build build --target check` builds ONLY what `check` depends on,
# and a missing dependency surfaces as a single misleading
# "FAIL LSP: hover/definition/references/rename" instead of a build error.
add_custom_target(check
  COMMAND "${CMAKE_COMMAND}" -E env "LLVM_ROOT=${LLVM_ROOT}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh"
  DEPENDS penstock rt_ir freshet-language-server
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "freshet regression suite"
  VERBATIM)

add_custom_target(bench
  COMMAND "${CMAKE_COMMAND}" -E env "LLVM_ROOT=${LLVM_ROOT}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/bench.sh"
  DEPENDS penstock rt_ir
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "freshet benchmarks (AoS vs SoA vs C baseline, same backend)"
  VERBATIM)

enable_testing()
add_test(NAME freshet_suite
         COMMAND tests/run.sh
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(freshet_suite PROPERTIES
  ENVIRONMENT "LLVM_ROOT=${LLVM_ROOT}")
if(PENSTOCK_OPT_PLUGIN_BUILT AND EXISTS "${PENSTOCK_OPT_EXECUTABLE}")
  add_test(NAME penstock_opt_plugin
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/opt_plugin.sh"
            "${PENSTOCK_OPT_EXECUTABLE}"
            "$<TARGET_FILE:PenstockOptPlugin>"
            "${CMAKE_CURRENT_BINARY_DIR}/penstock-opt-plugin-test")
endif()
