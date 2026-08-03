# OpenCode++ -- hyper-efficient, embeddable C++ agentic-coding engine (libopencode).
# Dependency policy (see 04_DEPENDENCY_POLICY.md): stdlib-first; every third-party
# dep is optional, OFF by default, isolated behind an interface in its own TU.
cmake_minimum_required(VERSION 3.20)
project(opencodepp VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Optional-backend switches (all OFF by default per 04_DEPENDENCY_POLICY.md Section 3).
# A switch is wired to real code only when its phase lands:
#   mbedTLS (Phase 4), tree-sitter (Phase 9), sqlite (Phase 3), zstd (Phase 13).
# ---------------------------------------------------------------------------
option(OPENCODE_USE_MBEDTLS   "Use mbedTLS as the net/TlsBackend" OFF)
option(OPENCODE_USE_TREE_SITTER "Use tree-sitter as the verify/ParseBackend" OFF)
option(OPENCODE_USE_SQLITE    "Use SQLite as a durable store/Store backend" OFF)
option(OPENCODE_USE_ZSTD      "Use zstd as the util/CompressBackend" OFF)
option(OPENCODE_USE_DOCTEST   "Use vendored doctest for unit tests" OFF)
option(OPENCODE_USE_EIGEN     "Use Eigen (FORBIDDEN: no local ML)" OFF)
option(OPENCODE_ENABLE_SANITIZERS "Build with Address/UB sanitizers" OFF)

if(OPENCODE_USE_EIGEN)
  message(FATAL_ERROR
    "OPENCODE_USE_EIGEN is forbidden by design (no local ML). See 04_DEPENDENCY_POLICY.md.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warnings: dev builds treat warnings as errors (02_CODING_PROTOCOL.md Section 6).
# ---------------------------------------------------------------------------
if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(OPENCODE_DEV_COMPILE_OPTIONS -Werror)
  endif()
endif()

if(OPENCODE_ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
  message(STATUS "opencodepp: ASan/UBSan enabled")
endif()

include(CTest) # defines BUILD_TESTING

# ---------------------------------------------------------------------------
# Subprojects (libraries are defined in src/; include/ is the PUBLIC interface)
# ---------------------------------------------------------------------------
add_subdirectory(src)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
add_subdirectory(tools)

message(STATUS "opencodepp ${PROJECT_VERSION} configured "
               "(mbedtls=${OPENCODE_USE_MBEDTLS} tree_sitter=${OPENCODE_USE_TREE_SITTER} "
               "sqlite=${OPENCODE_USE_SQLITE} zstd=${OPENCODE_USE_ZSTD})")
