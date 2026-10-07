#include <jni.h>
#include <android/log.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

// Change the hue (0..1), then Build and run: the fractal is recolored by C++.
constexpr float kHue = 0.55f;

extern "C" JNIEXPORT jstring JNICALL
Java_dev_arlinux_pocketstudio_NativeBridge_summary(JNIEnv* env, jobject) {
    const std::vector<int> values{3, 7, 11, 21};
    const auto total = std::accumulate(values.begin(), values.end(), 0);
    const std::string message = "C++ on Android: sum=" + std::to_string(total);
    __android_log_print(ANDROID_LOG_INFO, "PocketNative", "%s", message.c_str());
    return env->NewStringUTF(message.c_str());
}

static int channel(float hue, float offset, float light) {
    const float wave = 0.5f + 0.5f * std::cos(6.2831853f * (hue + offset));
    return static_cast<int>(std::clamp(wave * light, 0.0f, 1.0f) * 255.0f);
}

// Smooth-coloured Mandelbrot set (seahorse valley), one ARGB pixel per element.
extern "C" JNIEXPORT void JNICALL
Java_dev_arlinux_pocketstudio_NativeBridge_render(JNIEnv* env, jobject, jintArray pixels, jint width, jint height) {
    if (width <= 0 || height <= 0 || env->GetArrayLength(pixels) < width * height)
        return;
    std::vector<jint> image(static_cast<size_t>(width) * height);
    constexpr int kIterations = 200;
    constexpr double kCenterX = -0.7436, kCenterY = 0.1318, kSpan = 0.012;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double cr = kCenterX + (x - width * 0.5) * kSpan / height;
            const double ci = kCenterY + (y - height * 0.5) * kSpan / height;
            double zr = 0, zi = 0;
            int i = 0;
            while (i < kIterations && zr * zr + zi * zi < 256.0) {
                const double t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                ++i;
            }
            jint argb = static_cast<jint>(0xFF05080D);
            if (i < kIterations) {
                const float modulus = std::sqrt(static_cast<float>(zr * zr + zi * zi));
                const float smooth = i + 1.0f - std::log2(std::log2(modulus));
                const float spread = std::sqrt(std::clamp(smooth / kIterations, 0.0f, 1.0f));
                const float hue = kHue + spread;
                const float light = std::min(1.0f, spread * 1.8f);
                argb = static_cast<jint>(0xFF000000u | channel(hue, 0.0f, light) << 16 | channel(hue, 0.33f, light) << 8 | channel(hue, 0.67f, light));
            }
            image[static_cast<size_t>(y) * width + x] = argb;
        }
    }
    env->SetIntArrayRegion(pixels, 0, width * height, image.data());
}
