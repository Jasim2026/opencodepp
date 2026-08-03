# src/ -- OpenCode++ module sources.
#
# opencodepp_core   : OBJECT library of all engine TUs (PIC). Phases add their
#                     sources here as they land (core/util P1, msg P2, config/
#                     model/store P3, net P4, provider P5, prompt P6, graph P7,
#                     tools P8, verify P9, agent P10, memory P11, abi P12).
# opencodepp_static : static lib (embeddable; the default for hosts).
# opencodepp_shared : shared lib (dlopen-friendly; carries exported ABI symbol).

add_library(opencodepp_core OBJECT abi/version.cpp)
target_include_directories(opencodepp_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)

add_library(opencodepp_static STATIC $<TARGET_OBJECTS:opencodepp_core>)
add_library(opencodepp_shared SHARED $<TARGET_OBJECTS:opencodepp_core>)
set_target_properties(opencodepp_shared PROPERTIES OUTPUT_NAME opencodepp)

foreach(tgt opencodepp_static opencodepp_shared)
  target_include_directories(${tgt} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)
  target_compile_options(${tgt} PRIVATE $<$<CONFIG:Debug>:${OPENCODE_DEV_COMPILE_OPTIONS}>)
endforeach()
