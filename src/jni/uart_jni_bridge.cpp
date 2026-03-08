#include "uart_jni_bridge.hpp"
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <cstring>

namespace cyberdeck {

// Global pointer for JNI callbacks
static UartJniBridge* g_bridge_instance = nullptr;

struct UartJniBridge::Impl {
    void* jvm_lib = nullptr;
    JavaVM* jvm = nullptr;
    JNIEnv* env = nullptr;
    jobject bridge_obj = nullptr;

    jclass bridge_class = nullptr;
    jmethodID init_method = nullptr;
    jmethodID connect_method = nullptr;
    jmethodID disconnect_method = nullptr;
    jmethodID send_message_method = nullptr;
    jmethodID send_message_blocking_method = nullptr;

    EndpointCallback endpoint_callback;
    ConnectionCallback connected_callback;
    ConnectionCallback session_opened_callback;
    ConnectionCallback disconnected_callback;

    std::string jar_path;
};

UartJniBridge::UartJniBridge(const std::string& jar_path)
    : impl_(std::make_unique<Impl>()) {
    impl_->jar_path = jar_path;
    g_bridge_instance = this;

    // Create JVM
    JavaVMInitArgs vm_args{};
    JavaVMOption options[2];

    // Set classpath
    std::string classpath = "-Djava.class.path=" + jar_path;
    options[0].optionString = const_cast<char*>(classpath.c_str());

    // Enable verbose output for debugging
    options[1].optionString = const_cast<char*>("-verbose:jni");

    vm_args.version = JNI_VERSION_1_8;
    vm_args.nOptions = 2;
    vm_args.options = options;
    vm_args.ignoreUnrecognized = JNI_FALSE;

    const char* java_home = getenv("JAVA_HOME");
    std::string libjvm_path;
    if (java_home) {
        libjvm_path = std::string(java_home) + "/lib/server/libjvm.so";
    } else {
        libjvm_path = "/usr/lib/jvm/default-java/lib/server/libjvm.so";
    }

    impl_->jvm_lib = dlopen(libjvm_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!impl_->jvm_lib) {
        throw std::runtime_error(std::string("dlopen libjvm.so failed: ") + dlerror());
    }

    using JNI_CreateJavaVM_fn = jint (*)(JavaVM**, void**, void*);
    auto create_jvm = reinterpret_cast<JNI_CreateJavaVM_fn>(
        dlsym(impl_->jvm_lib, "JNI_CreateJavaVM"));
    if (!create_jvm) {
        throw std::runtime_error("dlsym JNI_CreateJavaVM failed");
    }

    jint result = create_jvm(&impl_->jvm, (void**)&impl_->env, &vm_args);
    if (result != JNI_OK) {
        throw std::runtime_error("Failed to create JVM");
    }

    // Find bridge class
    impl_->bridge_class = impl_->env->FindClass("io/rebble/libpebblecommon/jni/UartJniBridge");
    if (!impl_->bridge_class) {
        throw std::runtime_error("Failed to find UartJniBridge class");
    }

    // Make class reference global
    impl_->bridge_class = (jclass)impl_->env->NewGlobalRef(impl_->bridge_class);

    // Get method IDs
    jmethodID constructor = impl_->env->GetMethodID(impl_->bridge_class, "<init>", "()V");
    if (!constructor) {
        throw std::runtime_error("Failed to find constructor");
    }

    impl_->init_method = impl_->env->GetMethodID(impl_->bridge_class, "init", "(Ljava/lang/String;)Z");
    impl_->connect_method = impl_->env->GetMethodID(impl_->bridge_class, "connect", "()Z");
    impl_->disconnect_method = impl_->env->GetMethodID(impl_->bridge_class, "disconnect", "()V");
    impl_->send_message_method = impl_->env->GetMethodID(impl_->bridge_class, "sendMessage", "(I[B)Z");
    impl_->send_message_blocking_method = impl_->env->GetMethodID(impl_->bridge_class, "sendMessageBlocking", "(I[B)Z");

    if (!impl_->init_method || !impl_->connect_method ||
        !impl_->disconnect_method || !impl_->send_message_method ||
        !impl_->send_message_blocking_method) {
        throw std::runtime_error("Failed to find methods");
    }

    // Register native methods BEFORE creating object
    // This must happen before NewObject() because the Kotlin companion object
    // initializer runs during object creation
    JNINativeMethod methods[] = {
        {
            const_cast<char*>("nativeOnEndpointMessage"),
            const_cast<char*>("(I[B)V"),
            (void*)&Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnEndpointMessage
        },
        {
            const_cast<char*>("nativeOnConnected"),
            const_cast<char*>("()V"),
            (void*)&Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnConnected
        },
        {
            const_cast<char*>("nativeOnSessionOpened"),
            const_cast<char*>("()V"),
            (void*)&Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnSessionOpened
        },
        {
            const_cast<char*>("nativeOnDisconnected"),
            const_cast<char*>("()V"),
            (void*)&Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnDisconnected
        }
    };

    if (impl_->env->RegisterNatives(impl_->bridge_class, methods, 4) < 0) {
        throw std::runtime_error("Failed to register native methods");
    }

    std::cout << "UartJniBridge: Native methods registered" << std::endl;

    // Create bridge object
    impl_->bridge_obj = impl_->env->NewObject(impl_->bridge_class, constructor);
    if (!impl_->bridge_obj) {
        throw std::runtime_error("Failed to create bridge object");
    }

    // Make object reference global
    impl_->bridge_obj = impl_->env->NewGlobalRef(impl_->bridge_obj);

    std::cout << "UartJniBridge: JVM created successfully" << std::endl;
}

UartJniBridge::~UartJniBridge() {
    disconnect();

    if (impl_->env) {
        if (impl_->bridge_obj) {
            impl_->env->DeleteGlobalRef(impl_->bridge_obj);
        }
        if (impl_->bridge_class) {
            impl_->env->DeleteGlobalRef(impl_->bridge_class);
        }
    }

    if (impl_->jvm) {
        impl_->jvm->DestroyJavaVM();
    }

    if (impl_->jvm_lib) {
        dlclose(impl_->jvm_lib);
    }

    g_bridge_instance = nullptr;
}

bool UartJniBridge::init(const std::string& device_path) {
    jstring jdevice_path = impl_->env->NewStringUTF(device_path.c_str());
    jboolean result = impl_->env->CallBooleanMethod(impl_->bridge_obj, impl_->init_method, jdevice_path);
    impl_->env->DeleteLocalRef(jdevice_path);

    if (impl_->env->ExceptionCheck()) {
        impl_->env->ExceptionDescribe();
        impl_->env->ExceptionClear();
        return false;
    }

    return result == JNI_TRUE;
}

bool UartJniBridge::connect() {
    jboolean result = impl_->env->CallBooleanMethod(impl_->bridge_obj, impl_->connect_method);

    if (impl_->env->ExceptionCheck()) {
        impl_->env->ExceptionDescribe();
        impl_->env->ExceptionClear();
        return false;
    }

    return result == JNI_TRUE;
}

void UartJniBridge::disconnect() {
    impl_->env->CallVoidMethod(impl_->bridge_obj, impl_->disconnect_method);

    if (impl_->env->ExceptionCheck()) {
        impl_->env->ExceptionDescribe();
        impl_->env->ExceptionClear();
    }
}

bool UartJniBridge::send_message(uint16_t endpoint, const std::vector<uint8_t>& data) {
    // Get JNIEnv for the current thread — send_message may be called from
    // a JVM callback thread (Dispatchers.Default), not the main thread.
    // Using impl_->env (main thread's env) on another thread is undefined.
    JNIEnv* env = nullptr;
    bool needs_detach = false;
    jint status = impl_->jvm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (status == JNI_EDETACHED) {
        if (impl_->jvm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK) {
            return false;
        }
        needs_detach = true;
    } else if (status != JNI_OK) {
        return false;
    }

    jbyteArray jdata = env->NewByteArray(data.size());
    env->SetByteArrayRegion(jdata, 0, data.size(), reinterpret_cast<const jbyte*>(data.data()));

    jboolean result = env->CallBooleanMethod(
        impl_->bridge_obj,
        impl_->send_message_method,
        (jint)endpoint,
        jdata
    );

    env->DeleteLocalRef(jdata);

    bool success = (result == JNI_TRUE);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        success = false;
    }

    if (needs_detach) {
        impl_->jvm->DetachCurrentThread();
    }

    return success;
}

bool UartJniBridge::send_message_blocking(uint16_t endpoint, const std::vector<uint8_t>& data) {
    JNIEnv* env = nullptr;
    bool needs_detach = false;
    jint status = impl_->jvm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (status == JNI_EDETACHED) {
        if (impl_->jvm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK) {
            return false;
        }
        needs_detach = true;
    } else if (status != JNI_OK) {
        return false;
    }

    jbyteArray jdata = env->NewByteArray(data.size());
    env->SetByteArrayRegion(jdata, 0, data.size(), reinterpret_cast<const jbyte*>(data.data()));

    jboolean result = env->CallBooleanMethod(
        impl_->bridge_obj,
        impl_->send_message_blocking_method,
        (jint)endpoint,
        jdata
    );

    env->DeleteLocalRef(jdata);

    bool success = (result == JNI_TRUE);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        success = false;
    }

    if (needs_detach) {
        impl_->jvm->DetachCurrentThread();
    }

    return success;
}

void UartJniBridge::set_endpoint_callback(EndpointCallback callback) {
    impl_->endpoint_callback = std::move(callback);
}

void UartJniBridge::set_connected_callback(ConnectionCallback callback) {
    impl_->connected_callback = std::move(callback);
}

void UartJniBridge::set_session_opened_callback(ConnectionCallback callback) {
    impl_->session_opened_callback = std::move(callback);
}

void UartJniBridge::set_disconnected_callback(ConnectionCallback callback) {
    impl_->disconnected_callback = std::move(callback);
}

} // namespace cyberdeck

// JNI callback implementations (called from Java/Kotlin)

extern "C" {

JNIEXPORT void JNICALL Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnEndpointMessage(
    JNIEnv* env, jobject obj, jint endpoint, jbyteArray data) {

    if (!cyberdeck::g_bridge_instance || !cyberdeck::g_bridge_instance->impl_->endpoint_callback) {
        return;
    }

    jsize len = env->GetArrayLength(data);
    std::vector<uint8_t> bytes(len);
    env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(bytes.data()));

    cyberdeck::g_bridge_instance->impl_->endpoint_callback(endpoint, bytes);
}

JNIEXPORT void JNICALL Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnConnected(
    JNIEnv* env, jobject obj) {

    if (!cyberdeck::g_bridge_instance || !cyberdeck::g_bridge_instance->impl_->connected_callback) {
        return;
    }

    cyberdeck::g_bridge_instance->impl_->connected_callback();
}

JNIEXPORT void JNICALL Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnSessionOpened(
    JNIEnv* env, jobject obj) {

    if (!cyberdeck::g_bridge_instance || !cyberdeck::g_bridge_instance->impl_->session_opened_callback) {
        return;
    }

    cyberdeck::g_bridge_instance->impl_->session_opened_callback();
}

JNIEXPORT void JNICALL Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnDisconnected(
    JNIEnv* env, jobject obj) {

    if (!cyberdeck::g_bridge_instance || !cyberdeck::g_bridge_instance->impl_->disconnected_callback) {
        return;
    }

    cyberdeck::g_bridge_instance->impl_->disconnected_callback();
}

} // extern "C"
