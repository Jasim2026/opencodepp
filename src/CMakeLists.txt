# src/ -- OpenCode++ module sources.
#
# opencodepp_core   : OBJECT library of all engine TUs (PIC). Phases add their
#                     sources here as they land (core/util P1, msg P2, config/
#                     model/store P3, net P4, provider P5, prompt P6, graph P7,
#                     tools P8, verify P9, agent P10, memory P11, abi P12).
# opencodepp_static : static lib (embeddable; the default for hosts).
# opencodepp_shared : shared lib (dlopen-friendly; carries exported ABI symbol).

set(OPENCODE_CORE_SOURCES
  abi/version.cpp
  config/config.cpp
  config/env_snapshot.cpp
  core/arena.cpp
  core/event_loop.cpp
  core/log.cpp
  core/metrics.cpp
  core/channel.cpp
  core/task.cpp
  graph/callgraph.cpp
  graph/index.cpp
  graph/lazy.cpp
  graph/queries.cpp
  graph/symbols.cpp
  msg/codec.cpp
  msg/message.cpp
  msg/tokens.cpp
  net/http1.cpp
  net/meter.cpp
  net/offline.cpp
  net/policy.cpp
  net/pool.cpp
  net/socket.cpp
  net/sse.cpp
  net/tls.cpp
  net/tls_host.cpp
  net/transport.cpp
  provider/anthropic.cpp
  provider/factory.cpp
  provider/gemini.cpp
  provider/openai.cpp
  provider/openai_compat.cpp
  provider/resolver.cpp
  prompt/budget.cpp
  prompt/compiler.cpp
  prompt/context.cpp
  prompt/registry.cpp
  store/mem_store.cpp
  tools/exec/patch.cpp
  tools/exec/read_only.cpp
  tools/exec/util.cpp
  tools/exec/write.cpp
  tools/permission.cpp
  tools/registry.cpp
  tools/schema.cpp
  util/base64.cpp
  util/json.cpp
  util/sha1.cpp)

# Optional durable store backend: compiled and linked only when enabled.
# sqlite3 is never imported by any other TU (04_DEPENDENCY_POLICY.md Section 3).
if(OPENCODE_USE_SQLITE)
  list(APPEND OPENCODE_CORE_SOURCES store/sqlite_store.cpp)
endif()

# Optional mbedTLS TLS backend (04_DEPENDENCY_POLICY.md Section 3). The host
# backend is always compiled; only this one needs the mbedtls headers/lib.
if(OPENCODE_USE_MBEDTLS)
  list(APPEND OPENCODE_CORE_SOURCES net/tls_mbedtls.cpp)
endif()

add_library(opencodepp_core OBJECT ${OPENCODE_CORE_SOURCES})
target_include_directories(opencodepp_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)

# tls.cpp dispatches on OPENCODE_USE_MBEDTLS, so the definition must travel
# with the object library's compilation (not just select the extra TU).
if(OPENCODE_USE_MBEDTLS)
  target_compile_definitions(opencodepp_core PUBLIC OPENCODE_USE_MBEDTLS=1)
endif()

add_library(opencodepp_static STATIC $<TARGET_OBJECTS:opencodepp_core>)
add_library(opencodepp_shared SHARED $<TARGET_OBJECTS:opencodepp_core>)
set_target_properties(opencodepp_shared PROPERTIES OUTPUT_NAME opencodepp)

# The static/shared libs use $<TARGET_OBJECTS>, so the object lib's link usage
# requirements do NOT propagate; link the optional backends explicitly here.
if(OPENCODE_USE_SQLITE)
  target_link_libraries(opencodepp_static PUBLIC sqlite3)
  target_link_libraries(opencodepp_shared PUBLIC sqlite3)
endif()
if(OPENCODE_USE_MBEDTLS)
  target_link_libraries(opencodepp_static PUBLIC mbedtls mbedx509 mbedcrypto)
  target_link_libraries(opencodepp_shared PUBLIC mbedtls mbedx509 mbedcrypto)
endif()

foreach(tgt opencodepp_static opencodepp_shared)
  target_include_directories(${tgt} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)
  target_compile_options(${tgt} PRIVATE $<$<CONFIG:Debug>:${OPENCODE_DEV_COMPILE_OPTIONS}>)
endforeach()
