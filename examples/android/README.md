// examples/android -- a single-activity harness for libopencodepp.
//
// This is the Android counterpart of examples/event_host: it binds the same
// frozen C ABI (include/opencode/opencode.h) through a thin JNI shim. It is
// source-only in this repo: CI builds the JNI path via bindings/jni (when a
// JDK is present), and the NDK build for this harness is done out-of-tree.
//
// Networking + TLS live inside the engine, so the JNI shim needs no sockets.
// The TLS backend is pluggable (net/tls_host.cpp -- the host backend is always
// compiled); an Android app can supply the platform TLS through that internal
// hook without touching the public ABI.

# Build
# -------
# 1. Install the NDK + cmake; define ANDROID_NDK_HOME.
# 2. Configure with the repo toolchain (already referenced by the "android"
#    CMake preset):
#      cmake -S <opencodepp> -B build-android \
#        -DCMAKE_TOOLCHAIN_FILE=<opencodepp>/cmake/android.toolchain.cmake
# 3. Compile the engine lib (libopencodepp.so), then compile this shim against
#    it and your JNI headers:
#      $CXX -fPIC -shared -I<jdk>/include -I<jdk>/include/linux \
#           -I<opencodepp>/include jni/opencode_host.cpp \
#           -L build-android/src -lopencodepp -o libopencode_host.so
# 4. Drop libopencodepp.so + libopencode_host.so into your APK's libs
#    (abiFilters armeabi-v7a / arm64-v8a) and load them from MainActivity.
