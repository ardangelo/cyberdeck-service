#pragma once

#include <jni.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cyberdeck {

/**
 * JNI bridge for libpebble3 UART communication.
 *
 * This class manages the JVM lifecycle and provides a C++ interface
 * to the Kotlin UartJniBridge class.
 *
 * Usage:
 * ```cpp
 * UartJniBridge bridge("/path/to/libpebble3-jvm-fat.jar");
 * bridge.init("/dev/ttyS1");
 * bridge.set_endpoint_callback([](uint16_t endpoint, const std::vector<uint8_t>& data) {
 *     // Handle message
 * });
 * bridge.connect();
 * ```
 */
class UartJniBridge {
public:
    using EndpointCallback = std::function<void(uint16_t endpoint, const std::vector<uint8_t>& data)>;
    using ConnectionCallback = std::function<void()>;

    /**
     * Construct bridge with path to JAR file.
     *
     * @param jar_path Path to libpebble3-jvm-fat.jar
     */
    explicit UartJniBridge(const std::string& jar_path);

    /**
     * Destructor - cleans up JVM and resources.
     */
    ~UartJniBridge();

    // Non-copyable, non-movable
    UartJniBridge(const UartJniBridge&) = delete;
    UartJniBridge& operator=(const UartJniBridge&) = delete;

    /**
     * Initialize libpebble3 with UART device.
     *
     * @param device_path Path to UART device (e.g., "/dev/ttyS1")
     * @return true if initialization succeeded
     */
    bool init(const std::string& device_path);

    /**
     * Connect to Pebble via UART.
     * Starts PPoG handshake and opens protocol session.
     *
     * @return true if connection succeeded
     */
    bool connect();

    /**
     * Disconnect from Pebble and cleanup.
     */
    void disconnect();

    /**
     * Send message to endpoint (async, fire-and-forget).
     *
     * @param endpoint Endpoint number
     * @param data Message data
     * @return true if queued successfully
     */
    bool send_message(uint16_t endpoint, const std::vector<uint8_t>& data);

    /**
     * Send message to endpoint and block until written to UART.
     * Do NOT call from within a JNI callback — use send_message instead.
     *
     * @param endpoint Endpoint number
     * @param data Message data
     * @return true if written to UART, false on error or timeout
     */
    bool send_message_blocking(uint16_t endpoint, const std::vector<uint8_t>& data);

    /**
     * Set callback for endpoint messages.
     *
     * @param callback Function to call when endpoint message is received
     */
    void set_endpoint_callback(EndpointCallback callback);

    /**
     * Set callback for UART connection (before PPoG handshake).
     *
     * @param callback Function to call when UART opens
     */
    void set_connected_callback(ConnectionCallback callback);

    /**
     * Set callback for session opened (after PPoG handshake).
     *
     * @param callback Function to call when session is ready
     */
    void set_session_opened_callback(ConnectionCallback callback);

    /**
     * Set callback for disconnection.
     *
     * @param callback Function to call when disconnected
     */
    void set_disconnected_callback(ConnectionCallback callback);

    // Public to allow JNI callbacks to access
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cyberdeck

// JNI callback functions (must be in global scope with extern "C")
extern "C" {
    JNIEXPORT void JNICALL Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnEndpointMessage(
        JNIEnv* env, jobject obj, jint endpoint, jbyteArray data);
    JNIEXPORT void JNICALL Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnConnected(
        JNIEnv* env, jobject obj);
    JNIEXPORT void JNICALL Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnSessionOpened(
        JNIEnv* env, jobject obj);
    JNIEXPORT void JNICALL Java_io_rebble_libpebblecommon_jni_UartJniBridge_nativeOnDisconnected(
        JNIEnv* env, jobject obj);
}
