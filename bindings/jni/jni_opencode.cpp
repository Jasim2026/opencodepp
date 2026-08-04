/*
 * jni_opencode.cpp -- JNI glue for the io.opencode.Opencode class.
 *
 * Phase 12 task 6: the Java binding binds to the frozen C ABI
 * (include/opencode/opencode.h) -- the same ABI as the reference CLI and the
 * Python binding. No NDK networking deps are needed here: host networking and
 * TLS live in the engine (Phase 4). This file only converts Java strings to
 * C buffers, calls the ABI, and reports failures as Java RuntimeExceptions.
 *
 * Build (see bindings/jni/CMakeLists.txt):
 *   cc -fPIC -shared -I <jdk>/include -I <jdk>/include/linux \
 *      jni_opencode.cpp -L <build>/src -lopencodepp -o libopencodepp_jni.so
 */
#include "opencode/opencode.h"

#include <jni.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

void throw_status(JNIEnv* env, const char* fn, opencode_status_t st) {
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s failed: opencode status %d", fn,
                  static_cast<int>(st));
    jclass ex = env->FindClass("java/lang/RuntimeException");
    if (ex != nullptr) env->ThrowNew(ex, msg);
}

/* Count-only metrics sink: the harness just needs the snapshot count. */
void count_metric(void* ud, const char*, opencode_metric_kind_t, double,
                  uint64_t) {
    if (ud != nullptr) ++*static_cast<int*>(ud);
}

} /* namespace */

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL Java_io_opencode_Opencode_abiVersion(JNIEnv*, jclass) {
    return static_cast<jint>(opencode_abi_version());
}

JNIEXPORT jlong JNICALL Java_io_opencode_Opencode_engineCreate(
    JNIEnv* env, jclass, jstring jws, jstring jurl, jint policy) {
    const char* ws = env->GetStringUTFChars(jws, nullptr);
    const char* url =
        jurl != nullptr ? env->GetStringUTFChars(jurl, nullptr) : nullptr;

    opencode_config_t cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.version = OPENCODE_CONFIG_VERSION;
    cfg.workspace = ws;
    cfg.base_url = url;
    cfg.tool_policy = static_cast<opencode_tool_policy_t>(policy);

    opencode_engine_t* eng = nullptr;
    const opencode_status_t st = opencode_engine_create(&cfg, &eng);

    env->ReleaseStringUTFChars(jws, ws);
    if (url != nullptr) env->ReleaseStringUTFChars(jurl, url);

    if (st != OPENCODE_OK) {
        throw_status(env, "engineCreate", st);
        return 0;
    }
    return reinterpret_cast<jlong>(eng);
}

JNIEXPORT jint JNICALL Java_io_opencode_Opencode_engineRun(
    JNIEnv* env, jclass, jlong h, jstring jprompt) {
    const char* prompt = env->GetStringUTFChars(jprompt, nullptr);
    const opencode_status_t st =
        opencode_engine_run(reinterpret_cast<opencode_engine_t*>(h), prompt,
                            nullptr, nullptr);
    env->ReleaseStringUTFChars(jprompt, prompt);
    return static_cast<jint>(st);
}

JNIEXPORT jint JNICALL Java_io_opencode_Opencode_engineCancel(
    JNIEnv*, jclass, jlong h) {
    return static_cast<jint>(
        opencode_engine_cancel(reinterpret_cast<opencode_engine_t*>(h)));
}

JNIEXPORT jint JNICALL Java_io_opencode_Opencode_engineDestroy(
    JNIEnv*, jclass, jlong h) {
    return static_cast<jint>(
        opencode_engine_destroy(reinterpret_cast<opencode_engine_t*>(h)));
}

JNIEXPORT jint JNICALL Java_io_opencode_Opencode_metricsSnapshot(
    JNIEnv* env, jclass, jlong h) {
    int count = 0;
    const opencode_status_t st = opencode_metrics_snapshot(
        reinterpret_cast<opencode_engine_t*>(h), &count_metric, &count,
        nullptr);
    if (st != OPENCODE_OK) {
        throw_status(env, "metricsSnapshot", st);
        return 0;
    }
    return static_cast<jint>(count);
}

JNIEXPORT jstring JNICALL Java_io_opencode_Opencode_memoryWrite(
    JNIEnv* env, jclass, jlong h, jint kind, jstring jkey, jstring jvalue) {
    const char* key = env->GetStringUTFChars(jkey, nullptr);
    const char* value = env->GetStringUTFChars(jvalue, nullptr);
    char id[64];
    std::memset(id, 0, sizeof id);
    const opencode_status_t st = opencode_memory_write(
        reinterpret_cast<opencode_engine_t*>(h),
        static_cast<opencode_memory_kind_t>(kind), key, value, "[]", id,
        sizeof id);
    env->ReleaseStringUTFChars(jkey, key);
    env->ReleaseStringUTFChars(jvalue, value);
    if (st != OPENCODE_OK) {
        throw_status(env, "memoryWrite", st);
        return nullptr;
    }
    return env->NewStringUTF(id);
}

} /* extern "C" */
