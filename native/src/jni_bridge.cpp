// JNI entry points called by the Kotlin launcher. The native engine exposes a
// flat C API; this file only converts jstring/jfloatArray data.

#include "engine.h"

#include <jni.h>
#include <string>
#include <vector>

extern "C" {

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeInit(JNIEnv* env, jclass,
                                                   jstring gameRoot,
                                                   jint width, jint height) {
    const char* chars = env->GetStringUTFChars(gameRoot, nullptr);
    if (!chars) return 0;
    const int result = bs_engine_init(chars, width, height);
    env->ReleaseStringUTFChars(gameRoot, chars);
    return result;
}

JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeShutdown(JNIEnv*, jclass) {
    bs_engine_shutdown();
}

JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeResize(JNIEnv*, jclass,
                                                     jint width, jint height) {
    bs_engine_resize(width, height);
}

JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeRender(JNIEnv*, jclass) {
    bs_engine_render();
}

JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeSetControls(JNIEnv* env, jclass,
                                                          jfloatArray axes,
                                                          jbyteArray buttons) {
    std::vector<float> a;
    std::vector<uint8_t> b;
    if (axes) {
        const jsize n = env->GetArrayLength(axes);
        a.resize(n);
        env->GetFloatArrayRegion(axes, 0, n, a.data());
    }
    if (buttons) {
        const jsize n = env->GetArrayLength(buttons);
        b.resize(n);
        env->GetByteArrayRegion(buttons, 0, n,
                                reinterpret_cast<jbyte*>(b.data()));
    }
    bs_engine_set_controls(a.data(), static_cast<int>(a.size()),
                           b.data(), static_cast<int>(b.size()));
}

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeSubmitCmds(JNIEnv* env, jclass,
                                                         jbyteArray buffer) {
    if (!buffer) return 0;
    const jsize n = env->GetArrayLength(buffer);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    env->GetByteArrayRegion(buffer, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    return static_cast<jint>(bs_engine_submit_cmds(bytes.data(),
                                                   static_cast<uint32_t>(n)));
}

JNIEXPORT jstring JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF(bs_engine_status_json());
}

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeHleImportCount(JNIEnv*, jclass) {
    return static_cast<jint>(bs_engine_hle_import_count());
}

} // extern "C"
