// HTTP(S) fetcher for Android: java.net.HttpURLConnection over JNI (system TLS, no extra deps).
// The download thread attaches itself to the JavaVM the platform layer hands over.
#include <jni.h>

#include <cstdio>

#include "ThreadDownload.h"

namespace rts::pack {

namespace {
JavaVM* g_vm = nullptr;

bool check(JNIEnv* env, ThreadDownload& d, const char* what) {
    if (!env->ExceptionCheck()) return true;
    env->ExceptionClear();
    d.fail(std::string(what) + " failed");
    return false;
}
} // namespace

void setAndroidJavaVm(void* vm) { g_vm = static_cast<JavaVM*>(vm); }

std::unique_ptr<Download> fetchHttp(const std::string& url, const std::string& destPath, std::string& error) {
    if (!g_vm) { error = "HTTP downloads: JavaVM not set"; return nullptr; }
    auto dl = std::make_unique<ThreadDownload>();
    dl->run([url, destPath](ThreadDownload& d) {
        JNIEnv* env = nullptr;
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) { d.fail("cannot attach to the JavaVM"); return; }
        uint64_t have = 0;
        if (std::FILE* f = std::fopen(destPath.c_str(), "rb")) { std::fseek(f, 0, SEEK_END); have = uint64_t(std::ftell(f)); std::fclose(f); }
        env->PushLocalFrame(32);
        [&] {
            jclass urlCls = env->FindClass("java/net/URL");
            jobject u = env->NewObject(urlCls, env->GetMethodID(urlCls, "<init>", "(Ljava/lang/String;)V"), env->NewStringUTF(url.c_str()));
            if (!check(env, d, "URL")) return;
            jobject conn = env->CallObjectMethod(u, env->GetMethodID(urlCls, "openConnection", "()Ljava/net/URLConnection;"));
            if (!check(env, d, "openConnection")) return;
            jclass hc = env->FindClass("java/net/HttpURLConnection");
            env->CallVoidMethod(conn, env->GetMethodID(hc, "setConnectTimeout", "(I)V"), jint(20000));
            env->CallVoidMethod(conn, env->GetMethodID(hc, "setReadTimeout", "(I)V"), jint(60000));
            if (have) {
                std::string range = "bytes=" + std::to_string(have) + "-";
                env->CallVoidMethod(conn, env->GetMethodID(hc, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"),
                                    env->NewStringUTF("Range"), env->NewStringUTF(range.c_str()));
            }
            jint code = env->CallIntMethod(conn, env->GetMethodID(hc, "getResponseCode", "()I"));
            if (!check(env, d, "HTTP request")) return;
            if (code == 416) { d.succeed(); return; }
            if (code != 200 && code != 206) { d.fail("HTTP " + std::to_string(int(code))); return; }
            if (code == 200) have = 0;
            jobject in = env->CallObjectMethod(conn, env->GetMethodID(hc, "getInputStream", "()Ljava/io/InputStream;"));
            if (!check(env, d, "getInputStream")) return;
            jclass isCls = env->FindClass("java/io/InputStream");
            jmethodID readM = env->GetMethodID(isCls, "read", "([B)I");
            jbyteArray buf = env->NewByteArray(1 << 16);
            std::FILE* f = std::fopen(destPath.c_str(), have ? "ab" : "wb");
            if (!f) { d.fail("cannot write " + destPath); return; }
            d.bytes_ = have;
            for (;;) {
                jint n = env->CallIntMethod(in, readM, buf);
                if (!check(env, d, "read")) break;
                if (n < 0) { d.succeed(); break; }
                jbyte* p = env->GetByteArrayElements(buf, nullptr);
                size_t w = std::fwrite(p, 1, size_t(n), f);
                env->ReleaseByteArrayElements(buf, p, JNI_ABORT);
                if (w != size_t(n)) { d.fail("cannot write " + destPath); break; }
                d.bytes_ += uint64_t(n);
            }
            std::fclose(f);
            env->CallVoidMethod(in, env->GetMethodID(isCls, "close", "()V"));
            env->ExceptionClear();
        }();
        env->PopLocalFrame(nullptr);
        g_vm->DetachCurrentThread();
    });
    return dl;
}

} // namespace rts::pack
