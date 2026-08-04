# bindings/CMakeLists.txt -- optional host bindings (Phase 12).
# The Python binding is a pure-ctypes module (no build step); only the JNI
# glue needs a native library, and only when a JDK is present.
add_subdirectory(jni)
