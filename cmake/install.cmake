# cmake/install.cmake -- install/export rules for the opencodepp package.
#
# Included from the root CMakeLists.txt after all targets exist. Produces:
#   <prefix>/include/opencode/opencode.h         the frozen public header
#   <prefix>/lib/libopencodepp.{a,so}            static + shared engine
#   <prefix>/bin/opencodepp_cli                  the reference CLI
#   <prefix>/lib/cmake/opencodepp/               find_package(opencodepp CONFIG)
#   <prefix>/lib/pkgconfig/opencodepp{,-static}.pc
#
# Feature switches (OPENCODE_USE_*) are baked into the package config and the
# exported targets carry the corresponding link requirements, so a consumer
# that links opencodepp::opencodepp_static gets the right -l flags for the
# optional backends it was built with (04_DEPENDENCY_POLICY.md Section 3).

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# pkg-config private deps (used only when the optional backends are enabled).
# ---------------------------------------------------------------------------
set(OPENCODE_PKG_PRIVATE_LIBS "")
if(OPENCODE_USE_MBEDTLS)
  list(APPEND OPENCODE_PKG_PRIVATE_LIBS mbedtls)
endif()
if(OPENCODE_USE_SQLITE)
  list(APPEND OPENCODE_PKG_PRIVATE_LIBS sqlite3)
endif()
if(OPENCODE_USE_ZSTD)
  list(APPEND OPENCODE_PKG_PRIVATE_LIBS libzstd)
endif()
string(JOIN " " OPENCODE_PKG_PRIVATE_LIBS ${OPENCODE_PKG_PRIVATE_LIBS})

# ---------------------------------------------------------------------------
# Package config + version file (find_package(opencodepp CONFIG)).
# ---------------------------------------------------------------------------
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/opencodeppConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/opencodeppConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/opencodepp)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/opencodeppConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/opencodeppConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/opencodeppConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/opencodepp)

# ---------------------------------------------------------------------------
# pkg-config entries (relocatable: prefix derived from pcfiledir).
# ---------------------------------------------------------------------------
file(RELATIVE_PATH OPENCODE_PC_TO_PREFIX
  "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig"
  "${CMAKE_INSTALL_PREFIX}")
string(REGEX REPLACE "/$" "" OPENCODE_PC_TO_PREFIX "${OPENCODE_PC_TO_PREFIX}")
configure_file(cmake/opencodepp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/opencodepp.pc @ONLY)
configure_file(cmake/opencodepp-static.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/opencodepp-static.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/opencodepp.pc
              ${CMAKE_CURRENT_BINARY_DIR}/opencodepp-static.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ---------------------------------------------------------------------------
# Exported targets (opencodepp_static/shared, installed by src/CMakeLists.txt)
# + the reference CLI binary + the public header.
# ---------------------------------------------------------------------------
install(EXPORT opencodeppTargets
  FILE opencodeppTargets.cmake
  NAMESPACE opencodepp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/opencodepp)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")
