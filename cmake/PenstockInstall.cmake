# ------------------------------------------------------------- installation
# ------------------------------------------------------------- installation
# Installed layout is relocatable for the default relative GNUInstallDirs:
#   <prefix>/bin/penstock
#   <prefix>/share/penstock/runtime/frt_rt.c
#   <prefix>/lib/libfsh.a + <prefix>/include/fsh/*.h
# Runtime scratch/cache files go to a per-user cache directory after install.
install(TARGETS penstock freshet-language-server
  EXPORT PenstockTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS libfsh
  EXPORT PenstockTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(PENSTOCK_OPT_PLUGIN_BUILT)
  install(TARGETS PenstockOptPlugin
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/penstock/plugins")
endif()
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/fsh/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/fsh"
  FILES_MATCHING PATTERN "*.h")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime/frt_rt.c"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/penstock/runtime")
# Phase 3 (report §6.3): the Freshet-facing stdlib module — installed where
# builtinModulesDir() finds it (<prefix>/share/penstock/modules/fsh.fsh)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/modules/fsh.fsh"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/penstock/modules")
# Long-form diagnostic pages for `penstock --explain` (docs/errors/<ID>.md);
# installed where explainCode() looks: <prefix>/share/penstock/errors
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/errors/"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/penstock/errors"
  FILES_MATCHING PATTERN "F*.md")

set(_penstock_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/Penstock")
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PenstockConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/PenstockConfig.cmake"
  INSTALL_DESTINATION "${_penstock_cmake_dir}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/PenstockConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(EXPORT PenstockTargets
  FILE PenstockTargets.cmake
  NAMESPACE Penstock::
  DESTINATION "${_penstock_cmake_dir}")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/PenstockConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/PenstockConfigVersion.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PenstockHelpers.cmake"
  DESTINATION "${_penstock_cmake_dir}")
