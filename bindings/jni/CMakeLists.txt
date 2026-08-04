# bindings/jni/CMakeLists.txt -- JNI glue over the frozen C ABI (Phase 12
# task 6). Optional: only built when a JDK (JNI headers) is available.
find_package(JNI QUIET)
if(JNI_FOUND)
  add_library(opencodepp_jni SHARED jni_opencode.cpp)
  target_include_directories(opencodepp_jni PRIVATE ${JNI_INCLUDE_DIRS})
  target_link_libraries(opencodepp_jni PRIVATE opencodepp_shared)
  set_target_properties(opencodepp_jni PROPERTIES OUTPUT_NAME opencodepp_jni)
endif()
