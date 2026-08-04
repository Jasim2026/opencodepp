// examples/android/jni/opencode_host.cpp -- JNI shim over the frozen C ABI.
//
// A minimal single-activity host: create an engine with an event sink, run one
// task, and surface events as JNI string callbacks so the Activity can log
// them. The engine owns all networking and TLS (Phase 4); the host supplies
// only buffers and callbacks.
#include "opencode/opencode.h"

#include <jni.h>

#include <cstring>

namespace {

void throw_status(JNIEnv* env, const char* fn, opencode_status_t st) {
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s failed: opencode status %d", fn,
                  static_cast<int>(st));
    jclass ex = env->FindClass("java/lang/RuntimeException");
    if (ex != nullptr) env->ThrowNew(ex, msg);
}

struct Sink {
    JNIEnv* env;
    jobject activity;
    jmethodID onEvent;
};

opencode_status_t on_event(void* ud, const opencode_event_t* ev) {
    Sink* s = static_cast<Sink*>(ud);
    char text[192];
    text[0] = '\0';
    if (ev->text != nullptr && ev->text_len > 0) {
        const size_t n = ev->text_len < sizeof text - 1 ? ev->text_len
                                                        : sizeof text - 1;
        std::memcpy(text, ev->text, n);
        text[n] = '\0';
    }
    jstring jt = s->env->NewStringUTF(text);
    s->env->CallVoidMethod(s->activity, s->onEvent, ev->kind, jt);
    s->env->DeleteLocalRef(jt);
    return OPENCODE_OK;
}

} /* namespace */

extern "C" {

JNIEXPORT jint JNICALL Java_io_opencode_example_MainActivity_abiVersion(
    JNIEnv*, jclass) {
    return static_cast<jint>(opencode_abi_version());
}

JNIEXPORT jlong JNICALL
Java_io_opencode_example_MainActivity_engineCreate(JNIEnv* env, jobject thiz,
                                                   jstring jurl) {
    const char* url = env->GetStringUTFChars(jurl, nullptr);

    opencode_config_t cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.version = OPENCODE_CONFIG_VERSION;
    cfg.workspace = ".";
    cfg.base_url = url;
    cfg.model = "mock-model";
    cfg.tool_policy = OPENCODE_POLICY_ALLOW_READONLY;

    opencode_engine_t* eng = nullptr;
    const opencode_status_t st = opencode_engine_create(&cfg, &eng);
    env->ReleaseStringUTFChars(jurl, url);
    if (st != OPENCODE_OK) {
        throw_status(env, "engineCreate", st);
        return 0;
    }
    return reinterpret_cast<jlong>(eng);
}

JNIEXPORT void JNICALL
Java_io_opencode_example_MainActivity_engineRun(JNIEnv* env, jclass,
                                                jlong h, jstring jprompt) {
    const char* prompt = env->GetStringUTFChars(jprompt, nullptr);
    const opencode_status_t st = opencode_engine_run(
        reinterpret_cast<opencode_engine_t*>(h), prompt, nullptr, nullptr);
    env->ReleaseStringUTFChars(jprompt, prompt);
    if (st != OPENCODE_OK && st != OPENCODE_ERR_NETWORK)
        throw_status(env, "engineRun", st);
}

JNIEXPORT void JNICALL
Java_io_opencode_example_MainActivity_engineDestroy(JNIEnv*, jclass, jlong h) {
    if (h != 0) opencode_engine_destroy(reinterpret_cast<opencode_engine_t*>(h));
}

} /* extern "C" */
