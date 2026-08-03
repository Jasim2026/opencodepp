# Stub NDK toolchain (Phase 0). Real Android packaging lands with the JNI
# bindings (Phase 12). This file exists so the `android` preset configures
# cleanly once an NDK is provided; CMake's built-in Android support does the
# heavy lifting.
if(NOT DEFINED ANDROID_NDK)
  message(FATAL_ERROR
    "android preset requires -DANDROID_NDK=<path-to-ndk> (Phase 12 installs it).")
endif()

set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 26)
set(CMAKE_ANDROID_NDK ${ANDROID_NDK})
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(CMAKE_ANDROID_STL_TYPE c++_static)
set(CMAKE_ANDROID_API 26)
