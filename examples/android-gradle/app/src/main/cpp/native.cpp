#include <jni.h>
#include <android/log.h>
#include <numeric>
#include <string>
#include <vector>

extern "C" JNIEXPORT jstring JNICALL
Java_dev_arlinux_pocketstudio_NativeBridge_summary(JNIEnv* env, jobject) {
    const std::vector<int> values{3, 7, 11, 21};
    const auto total = std::accumulate(values.begin(), values.end(), 0);
    const std::string message = "C++ on Android: sum=" + std::to_string(total);
    __android_log_print(ANDROID_LOG_INFO, "PocketNative", "%s", message.c_str());
    return env->NewStringUTF(message.c_str());
}
