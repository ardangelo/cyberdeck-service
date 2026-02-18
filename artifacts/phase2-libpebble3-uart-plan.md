# Plan: libpebble3 UART Transport with JNI Bridge for cyberdeck_service

## Context

Extend libpebble3 with a complete UART+COBS transport implementation (mirroring the existing Bluetooth transport), and create a minimal JNI bridge for cyberdeck_service to use. This consolidates all protocol logic in libpebble3 and removes the need for uart.cpp/cobs.c in the C++ service.

**Why this architecture:**
- libpebble3 already has complete PPoG/PP implementation in commonMain
- Existing BLE transport (PebbleBle.kt) provides perfect template
- Keeps all protocol logic in one place (single source of truth)
- C++ becomes minimal - just JNI glue + UInput forwarding
- Eliminates duplicate UART/COBS implementations

**Why JNI instead of GraalVM:**
- GraalVM Native Image unavailable for RISC-V64 (K230 SoC)
- JVM readily available for RISC-V64

## Current State

**Phase 1 (Completed):**
- PPOUART in firmware: `cyberdeck-firmware/src/fw/comm/ppouart/`
- Firmware acts as PPoG server, handles reset handshake
- Full state machine, ack protocol, retransmission

**libpebble3 (Existing):**
- Location: `mobileapp/libpebble3/`
- commonMain: Complete PPoG, PP parsing, endpoint routing
- Existing transports: PebbleBle (BLE GATT), PebbleBtClassic
- jvmMain: Minimal stubs (platformModule is TODO)

**cyberdeck_service (Existing - TO BE SIMPLIFIED):**
- Current: UART I/O (uart.cpp), COBS framing (cobs.c), nanopb protocol
- Future: Minimal JNI bridge + UInput forwarding (~100 lines)

## Design Decisions

1. ✅ **UART ownership:** libpebble3 handles ALL UART+COBS+protocol logic
2. ✅ **Reference implementation:** Model after existing PebbleBle transport
3. ✅ **Architecture:** JVM + JNI (GraalVM unavailable)
4. ✅ **C++ scope:** Minimal - JNI initialization + UInput forwarding only
5. ✅ **Integration:** Replace entire uart.cpp/cobs.c/nanopb stack

## Final Architecture

```
┌────────────────────────────────────────────────────┐
│ C++ cyberdeck_service (main.cpp) - MINIMAL        │
│  ├─ JNI initialization (create JVM)               │
│  ├─ Register endpoint callbacks                   │
│  ├─ UInput device (keyboard/touchpad forwarding)  │
│  └─ ~100 lines total                              │
└──────────────┬─────────────────────────────────────┘
               │ JNI boundary
               ↓
┌────────────────────────────────────────────────────┐
│ Java Bridge (LibPebble3Bridge.java)               │
│  - Thin JNI wrapper around Kotlin API             │
│  - Native callbacks to C++                        │
└──────────────┬─────────────────────────────────────┘
               │ Kotlin/Java interop
               ↓
┌────────────────────────────────────────────────────┐
│ libpebble3 jvmMain - NEW UART TRANSPORT           │
│                                                     │
│  PebbleUart (TransportConnector)                  │
│    ├─ UART I/O (FileInputStream/OutputStream)    │
│    ├─ Termios configuration                       │
│    └─ Lifecycle management                        │
│                                                     │
│  UartPPoGPacketSender (PPoGPacketSender)          │
│    └─ Sends PPoG packets → COBS encode → UART    │
│                                                     │
│  UartPPoGReceiver                                  │
│    └─ UART read → COBS decode → PPoGStream       │
│                                                     │
│  Cobs.kt                                           │
│    ├─ encode(data: ByteArray): ByteArray          │
│    └─ CobsStreamDecoder (streaming decode)        │
└──────────────┬─────────────────────────────────────┘
               │ Uses existing commonMain
               ↓
┌────────────────────────────────────────────────────┐
│ libpebble3 commonMain (EXISTING - NO CHANGES)     │
│  ├─ PPoG.kt - State machine                      │
│  ├─ PPoGPacket.kt - Serialization                │
│  ├─ PebbleProtocolRunner.kt - PP parsing         │
│  ├─ TransportConnector interface                  │
│  └─ Endpoint routing                              │
└────────────────────────────────────────────────────┘
```

## Reference: Existing BLE Transport (Template)

The existing BLE transport provides the exact pattern to follow:

**File:** `/store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/pebble/PebbleBle.kt`

```kotlin
class PebbleBle(
    private val identifier: PebbleBleIdentifier,
    private val scope: ConnectionCoroutineScope,
    private val gattConnector: GattConnector,
    private val ppog: PPoG,
    private val ppogPacketSender: PPoGPacketSender,
    private val pPoGStream: PPoGStream,
    // ...
) : TransportConnector {

    override suspend fun connect(lastError: ConnectionFailureReason?): PebbleConnectionResult {
        // 1. Connect to GATT device
        // 2. Subscribe to characteristics
        // 3. Start PPoG
        // 4. Wait for session to open
    }

    override suspend fun disconnect() {
        ppog.close()
        // Close GATT connection
    }
}
```

**UART equivalent:** `PebbleUart` replaces GATT operations with UART operations

## Implementation Plan

### Phase 1: COBS Implementation in Kotlin

**Goal:** Port COBS encoding/decoding to pure Kotlin.

**Reference:** `/store/git/cyberdeck-firmware/src/fw/console/cobs.c` (100 lines)

#### File: `jvmMain/kotlin/io/rebble/libpebblecommon/util/Cobs.kt`

```kotlin
package io.rebble.libpebblecommon.util

object Cobs {
    /**
     * COBS encode data. Output does NOT include frame delimiters (0x00).
     * Caller must add 0x00 before and after encoded data.
     */
    fun encode(data: ByteArray): ByteArray {
        if (data.isEmpty()) return byteArrayOf()

        val output = ByteArray(data.size + (data.size / 254) + 1)
        var writeIndex = 1
        var codeIndex = 0
        var code = 1

        for (byte in data) {
            if (byte.toInt() == 0) {
                output[codeIndex] = code.toByte()
                codeIndex = writeIndex++
                code = 1
            } else {
                output[writeIndex++] = byte
                code++
                if (code == 0xFF) {
                    output[codeIndex] = code.toByte()
                    codeIndex = writeIndex++
                    code = 1
                }
            }
        }

        output[codeIndex] = code.toByte()
        return output.copyOf(writeIndex)
    }

    /**
     * COBS decode data. Input should NOT include frame delimiters.
     * Returns null if decode fails (invalid COBS data).
     */
    fun decode(data: ByteArray): ByteArray? {
        if (data.isEmpty()) return byteArrayOf()

        val output = ByteArray(data.size)
        var writeIndex = 0
        var readIndex = 0

        while (readIndex < data.size) {
            val code = data[readIndex++].toInt() and 0xFF
            if (code == 0) return null  // Invalid COBS

            for (i in 1 until code) {
                if (readIndex >= data.size) return null
                output[writeIndex++] = data[readIndex++]
            }

            if (code < 0xFF && readIndex < data.size) {
                output[writeIndex++] = 0
            }
        }

        return output.copyOf(writeIndex)
    }
}

/**
 * Streaming COBS decoder for byte-by-byte input.
 * Used by UART receiver to decode frames on the fly.
 */
class CobsStreamDecoder {
    private val buffer = mutableListOf<Byte>()
    private var state = State.AWAITING_DELIMITER

    private enum class State {
        AWAITING_DELIMITER,
        READING_FRAME
    }

    /**
     * Feed one byte. Returns decoded frame when complete (on 0x00 delimiter).
     */
    fun feed(byte: Byte): ByteArray? {
        when (state) {
            State.AWAITING_DELIMITER -> {
                if (byte.toInt() == 0) {
                    state = State.READING_FRAME
                    buffer.clear()
                }
            }
            State.READING_FRAME -> {
                if (byte.toInt() == 0) {
                    // Frame complete
                    val encoded = buffer.toByteArray()
                    buffer.clear()
                    state = State.AWAITING_DELIMITER
                    return Cobs.decode(encoded)
                } else {
                    buffer.add(byte)
                }
            }
        }
        return null
    }

    fun reset() {
        buffer.clear()
        state = State.AWAITING_DELIMITER
    }
}
```

**Verification:** Unit test COBS round-trip with various payloads

---

### Phase 2: UART I/O in jvmMain

**Goal:** Create UART device abstraction using Java FileInputStream/OutputStream.

#### File: `jvmMain/kotlin/io/rebble/libpebblecommon/connection/uart/UartDevice.kt`

```kotlin
package io.rebble.libpebblecommon.connection.uart

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException

class UartDevice(private val devicePath: String) : AutoCloseable {
    private var inputStream: FileInputStream? = null
    private var outputStream: FileOutputStream? = null

    suspend fun open() = withContext(Dispatchers.IO) {
        // Configure UART with stty (alternative to termios JNI)
        // This sets: 115200 baud, 8N1, raw mode, no flow control
        val sttyProcess = ProcessBuilder(
            "stty", "-F", devicePath,
            "115200", "raw", "-echo", "-echoe", "-echok"
        ).start()
        sttyProcess.waitFor()

        // Open as file streams
        inputStream = FileInputStream(devicePath)
        outputStream = FileOutputStream(devicePath)
    }

    suspend fun read(): Byte = withContext(Dispatchers.IO) {
        val stream = inputStream ?: throw IOException("UART not open")
        val byte = stream.read()
        if (byte == -1) throw IOException("UART EOF")
        byte.toByte()
    }

    suspend fun write(data: ByteArray) = withContext(Dispatchers.IO) {
        val stream = outputStream ?: throw IOException("UART not open")
        stream.write(data)
        stream.flush()
    }

    override fun close() {
        inputStream?.close()
        outputStream?.close()
        inputStream = null
        outputStream = null
    }
}
```

**Note:** Using `stty` for termios config is simpler than JNI. Alternatively, could use JNI wrapper around termios if needed.

---

### Phase 3: UartPPoGPacketSender

**Goal:** Implement PPoGPacketSender that sends packets via UART+COBS.

**Reference:** `/store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/transport/PpogClient.kt`

#### File: `jvmMain/kotlin/io/rebble/libpebblecommon/connection/uart/UartPPoGPacketSender.kt`

```kotlin
package io.rebble.libpebblecommon.connection.uart

import io.rebble.libpebblecommon.connection.bt.ble.ppog.PPoGPacketSender
import io.rebble.libpebblecommon.util.Cobs

class UartPPoGPacketSender(
    private val uart: UartDevice
) : PPoGPacketSender {

    override suspend fun sendPacket(packet: ByteArray): Boolean {
        return try {
            // COBS encode
            val encoded = Cobs.encode(packet)

            // Frame: 0x00 + encoded + 0x00
            val frame = ByteArray(encoded.size + 2)
            frame[0] = 0x00
            encoded.copyInto(frame, destinationOffset = 1)
            frame[frame.size - 1] = 0x00

            // Write to UART
            uart.write(frame)
            true
        } catch (e: Exception) {
            false
        }
    }

    override fun wasRestoredWithSubscribedCentral(): Boolean = false
}
```

---

### Phase 4: UartPPoGReceiver

**Goal:** Background coroutine that reads UART, COBS-decodes, feeds to PPoGStream.

**Reference:** `/store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/transport/PpogClient.kt` (characteristic notifications)

#### File: `jvmMain/kotlin/io/rebble/libpebblecommon/connection/uart/UartPPoGReceiver.kt`

```kotlin
package io.rebble.libpebblecommon.connection.uart

import io.rebble.libpebblecommon.connection.bt.ble.ppog.PPoGStream
import io.rebble.libpebblecommon.util.CobsStreamDecoder
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

class UartPPoGReceiver(
    private val uart: UartDevice,
    private val pPoGStream: PPoGStream,
    private val scope: CoroutineScope
) {
    private val decoder = CobsStreamDecoder()

    fun start() {
        scope.launch(Dispatchers.IO) {
            while (isActive) {
                try {
                    val byte = uart.read()
                    val frame = decoder.feed(byte)

                    if (frame != null) {
                        // Send to PPoG for processing
                        pPoGStream.inboundPPoGBytesChannel.send(frame)
                    }
                } catch (e: Exception) {
                    // Handle error (log, retry, etc.)
                    break
                }
            }
        }
    }
}
```

---

### Phase 5: PebbleUart TransportConnector

**Goal:** Implement TransportConnector for UART (equivalent to PebbleBle).

**Reference:** `/store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/pebble/PebbleBle.kt`

#### File: `jvmMain/kotlin/io/rebble/libpebblecommon/connection/uart/PebbleUart.kt`

```kotlin
package io.rebble.libpebblecommon.connection.uart

import io.rebble.libpebblecommon.connection.*
import io.rebble.libpebblecommon.connection.bt.ble.ppog.*
import kotlinx.coroutines.*

class PebbleUart(
    private val devicePath: String,
    private val scope: ConnectionCoroutineScope,
    private val ppog: PPoG,
    private val pPoGStream: PPoGStream,
) : TransportConnector {

    private val uart = UartDevice(devicePath)
    private var receiver: UartPPoGReceiver? = null
    private var sender: UartPPoGPacketSender? = null

    private val _disconnected = CompletableDeferred<ConnectionFailureReason>()
    override val disconnected: Deferred<ConnectionFailureReason> = _disconnected

    override suspend fun connect(lastError: ConnectionFailureReason?): PebbleConnectionResult {
        return try {
            // 1. Open UART device
            uart.open()

            // 2. Create sender (wired into PPoG via DI)
            sender = UartPPoGPacketSender(uart)

            // 3. Start receiver
            receiver = UartPPoGReceiver(uart, pPoGStream, scope)
            receiver?.start()

            // 4. Start PPoG state machine
            // This will:
            // - Initiate or wait for ResetRequest
            // - Complete reset handshake
            // - Open PebbleProtocolRunner
            scope.launch {
                try {
                    ppog.run()
                } catch (e: Exception) {
                    _disconnected.complete(ConnectionFailureReason.FailedToConnect)
                }
            }

            PebbleConnectionResult.Success
        } catch (e: Exception) {
            PebbleConnectionResult.Failure(ConnectionFailureReason.FailedToConnect)
        }
    }

    override suspend fun disconnect() {
        ppog.close()
        uart.close()
        _disconnected.complete(ConnectionFailureReason.Disconnected)
    }
}
```

---

### Phase 6: Minimal jvmMain Platform Stubs

**Goal:** Implement minimal platformModule for UART use case.

#### File: `jvmMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.jvm.kt`

```kotlin
package io.rebble.libpebblecommon.di

import io.rebble.libpebblecommon.connection.bt.BlePlatformConfig
import io.rebble.libpebblecommon.connection.bt.BluetoothState
import io.rebble.libpebblecommon.connection.uart.PebbleUart
import io.rebble.libpebblecommon.connection.uart.UartPPoGPacketSender
import io.rebble.libpebblecommon.connection.TransportConnector
import io.rebble.libpebblecommon.connection.bt.ble.ppog.PPoGPacketSender
import kotlinx.coroutines.flow.MutableStateFlow
import org.koin.dsl.module

actual val platformModule = module {
    // UART-specific config
    single {
        BlePlatformConfig(
            pinAddress = false,
            phoneRequestsPairing = false,
            writeConnectivityTrigger = false,
            initialMtu = 256 + 4,  // No MTU limit for UART
            desiredTxWindow = 4,
            desiredRxWindow = 4,
            useNativeMtu = false,
            sendPpogResetOnDisconnection = false,
            delayBleConnectionsAfterAppStart = false,
            delayBleDisconnections = false,
            fallbackToResetRequest = true,  // K230 initiates reset
            closeGattServerWhenBtDisabled = false,
            supportsBtClassic = false,
        )
    }

    // Bluetooth state stub (always enabled for UART mode)
    single<BluetoothState> {
        object : BluetoothState {
            override val isEnabled = MutableStateFlow(true)
        }
    }

    // In ConnectionScope: provide UART-specific implementations
    scope<ConnectionScope> {
        scoped<TransportConnector> {
            PebbleUart(
                devicePath = "/dev/ttyS2",  // TODO: make configurable
                scope = get(),
                ppog = get(),
                pPoGStream = get()
            )
        }

        scoped<PPoGPacketSender> {
            get<PebbleUart>().sender ?: error("Sender not initialized")
        }
    }
}
```

**Note:** This requires minor refactoring to expose sender from PebbleUart, or use a factory pattern.

---

### Phase 7: Phone Client Recognition (Endpoint 0x0011)

**Goal:** Ensure libpebble3 responds to firmware's version request so it's recognized as a phone client.

**Critical endpoint:** Session Remote Version (0x0011)

When the firmware opens a CommSession, it immediately sends a **Version Request** (command byte 0x00) to endpoint 0x0011. libpebble3 **must respond** with phone capabilities.

**Response format (V3 - 20 bytes recommended):**
```kotlin
data class SessionRemoteVersionResponse(
    val platformBitfield: UInt,        // 1 = Android, 2 = iOS, etc.
    val responseVersion: UByte = 2u,   // V3 format
    val majorVersion: UByte,           // e.g., 4
    val minorVersion: UByte,           // e.g., 5
    val bugfixVersion: UByte,          // e.g., 0
    val protocolCapabilities: UInt     // Bitfield of supported endpoints
)

fun serializeV3(): ByteArray {
    return ByteBuffer.allocate(20).apply {
        order(ByteOrder.BIG_ENDIAN)
        putInt(0)  // Deprecated library version
        putInt(0)  // Deprecated session capabilities
        putInt(platformBitfield.toInt())
        put(responseVersion.toByte())
        put(majorVersion.toByte())
        put(minorVersion.toByte())
        put(bugfixVersion.toByte())
        putInt(protocolCapabilities.toInt())
    }.array()
}
```

**Note:** libpebble3 commonMain likely **already implements this** as part of its phone app functionality. The UART transport should automatically route endpoint messages through the existing `PebbleProtocolRunner`.

**Verification:** After PPOUART opens, firmware will send version request. Check libpebble3 logs for:
1. Received message on endpoint 0x0011
2. Sent response with version info
3. Firmware logs should show "System session opened" or similar

**If not implemented:** Add endpoint handler in `LibPebble3UartWrapper`:
```kotlin
init {
    // Register version endpoint handler
    registerSystemEndpoint(0x0011u) { data ->
        if (data.isNotEmpty() && data[0] == 0x00.toByte()) {
            // Respond with phone version
            val response = SessionRemoteVersionResponse(
                platformBitfield = 1u,  // Android
                majorVersion = 4u,
                minorVersion = 5u,
                bugfixVersion = 0u,
                protocolCapabilities = 0xFFFFFFFFu  // All endpoints supported
            ).serializeV3()

            sendMessage(0x0011u, response)
        }
    }
}
```

---

### Phase 8: Meta Protocol Handler (Endpoint 0x0000)

**Goal:** Handle unknown endpoints gracefully.

**Optional but recommended:** Respond to endpoint 0x0000 (meta protocol) to handle unknown endpoint errors.

When libpebble3 receives a message on an unregistered endpoint, it can either:
1. Ignore it (simpler)
2. Send error code 0xdc (Unhandled) back on endpoint 0x0000

**Implementation** (optional):
```kotlin
// In LibPebble3UartWrapper
registerSystemEndpoint(0x0000u) { data ->
    // Meta protocol error handling
    // For now, just log unknown endpoints
}
```

This is not critical for Phase 2a - can be deferred.

---

### Phase 9: Java JNI Bridge

**Goal:** Minimal Java class exposing libpebble3 to C++ via JNI.

#### File: `jvmMain/java/io/rebble/libpebblecommon/bridge/LibPebble3Bridge.java`

```java
package io.rebble.libpebblecommon.bridge;

import io.rebble.libpebblecommon.LibPebble;
// Import other required classes

public class LibPebble3Bridge {
    private LibPebble libPebble;

    // Native callbacks (implemented in C++)
    private native void nativeOnEndpointMessage(int endpoint, byte[] data);

    static {
        System.loadLibrary("pebble3_jni");
    }

    public void init() {
        // Initialize Koin and LibPebble
        // Connect to UART transport
        // libPebble = LibPebble.create(...);
    }

    public void registerEndpoint(int endpoint) {
        // Register endpoint with callback to nativeOnEndpointMessage
    }

    public void sendMessage(int endpoint, byte[] data) {
        // libPebble.sendMessage(...);
    }

    public void close() {
        // Disconnect and cleanup
    }
}
```

---

### Phase 8: C++ JNI Bridge

**Goal:** Minimal C++ JNI wrapper.

#### File: `cyberdeck-service/src/jni/libpebble3_jni.hpp`

```cpp
#pragma once
#include <jni.h>
#include <functional>
#include <map>
#include <span>
#include <cstdint>

class LibPebble3JNI {
public:
    using EndpointCallback = std::function<void(uint16_t endpoint, std::span<const uint8_t> data)>;

    LibPebble3JNI(const char* jar_path);
    ~LibPebble3JNI();

    void init();
    void register_endpoint(uint16_t endpoint, EndpointCallback callback);
    void send_message(uint16_t endpoint, std::span<const uint8_t> data);
    void close();

private:
    JavaVM* jvm_;
    JNIEnv* env_;
    jobject bridge_obj_;
    std::map<uint16_t, EndpointCallback> endpoint_callbacks_;

    void create_jvm(const char* jar_path);

    friend void JNICALL Java_io_rebble_libpebblecommon_bridge_LibPebble3Bridge_nativeOnEndpointMessage(
        JNIEnv* env, jobject obj, jint endpoint, jbyteArray data);
};
```

#### File: `cyberdeck-service/src/jni/libpebble3_jni.cpp`

```cpp
#include "libpebble3_jni.hpp"
#include <stdexcept>

LibPebble3JNI* g_libpebble3_jni = nullptr;

LibPebble3JNI::LibPebble3JNI(const char* jar_path) {
    g_libpebble3_jni = this;
    create_jvm(jar_path);
    // Find LibPebble3Bridge class, create instance, cache method IDs
}

void LibPebble3JNI::init() {
    env_->CallVoidMethod(bridge_obj_, init_method_);
    // This opens UART, starts PPoG, etc.
}

void LibPebble3JNI::register_endpoint(uint16_t endpoint, EndpointCallback callback) {
    endpoint_callbacks_[endpoint] = callback;
    env_->CallVoidMethod(bridge_obj_, register_endpoint_method_, (jint)endpoint);
}

// JNI callback from Java
extern "C" JNIEXPORT void JNICALL
Java_io_rebble_libpebblecommon_bridge_LibPebble3Bridge_nativeOnEndpointMessage(
    JNIEnv* env, jobject obj, jint endpoint, jbyteArray data) {

    if (!g_libpebble3_jni) return;

    auto it = g_libpebble3_jni->endpoint_callbacks_.find(endpoint);
    if (it != g_libpebble3_jni->endpoint_callbacks_.end()) {
        jsize len = env->GetArrayLength(data);
        std::vector<uint8_t> bytes(len);
        env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(bytes.data()));
        it->second(endpoint, bytes);
    }
}
```

---

### Phase 9: Minimal C++ main.cpp (Test Version)

**Goal:** Minimal C++ to verify libpebble3 communication using firmware test commands.

**For initial testing, defer keyboard/touchpad endpoints.** Use firmware's built-in test infrastructure.

#### File: `src/main.cpp` (NEW - minimal test version)

```cpp
#include "jni/libpebble3_jni.hpp"
#include <iostream>
#include <csignal>
#include <iomanip>

volatile sig_atomic_t g_stop = 0;

void signal_handler(int sig) {
    g_stop = 1;
}

void print_hex(const std::span<const uint8_t>& data) {
    for (auto byte : data) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(byte) << " ";
    }
    std::cout << std::dec << std::endl;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const char* jar_path = "/opt/cyberdeck/libpebble3-jvm.jar";

    try {
        // Initialize libpebble3 (this opens UART internally)
        LibPebble3JNI pebble(jar_path);

        // Register test data endpoint
        // Firmware test command sends to this endpoint with 0xBEEFCAFE prefix
        pebble.register_endpoint(0x0000, [](uint16_t ep, auto data) {
            std::cout << "Received on endpoint " << ep << " (" << data.size() << " bytes): ";
            print_hex(data);

            // Check for firmware test prefix: 0xBEEFCAFE
            if (data.size() >= 4) {
                if (data[0] == 0xBE && data[1] == 0xEF &&
                    data[2] == 0xCA && data[3] == 0xFE) {
                    std::cout << "  ✓ Test prefix detected!" << std::endl;

                    // Print message (skip 4-byte prefix)
                    if (data.size() > 4) {
                        std::cout << "  Message: ";
                        for (size_t i = 4; i < data.size(); i++) {
                            std::cout << static_cast<char>(data[i]);
                        }
                        std::cout << std::endl;
                    }
                }
            }
        });

        // Start libpebble3 (opens UART, starts PPoG)
        pebble.init();

        std::cout << "libpebble3 initialized and waiting for test data..." << std::endl;
        std::cout << "\nFirmware test commands:" << std::endl;
        std::cout << "  k230 ppouart test       - Send test message with 0xBEEFCAFE prefix" << std::endl;
        std::cout << "  k230 ppouart data 0     - Send data packet with SN=0" << std::endl;
        std::cout << "  k230 stats              - Show TX/RX statistics" << std::endl;
        std::cout << "\nPress Ctrl+C to exit\n" << std::endl;

        // Main loop - just wait for signals
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        pebble.close();
        std::cout << "Shutting down..." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
```

**Total C++ code: ~80 lines**

**Testing workflow:**
```bash
# Terminal 1: Run service
./cyberdeck_daemon

# Terminal 2: Firmware console
k230 ppouart test    # Should see "✓ Test prefix detected!" + "Hello PPOUART!"
k230 stats           # Verify tx_frames > 0

# Expected output in Terminal 1:
# Received on endpoint 0 (18 bytes): BE EF CA FE 48 65 6C 6C 6F 20 50 50 4F 55 41 52 54 21
#   ✓ Test prefix detected!
#   Message: Hello PPOUART!
```

---

## Critical Files Summary

| File | Purpose | Lines | Type |
|------|---------|-------|------|
| **libpebble3 (Kotlin/Java)** |
| `jvmMain/kotlin/.../util/Cobs.kt` | COBS encoder/decoder | ~100 | New |
| `jvmMain/kotlin/.../uart/UartDevice.kt` | UART I/O wrapper | ~50 | New |
| `jvmMain/kotlin/.../uart/UartPPoGPacketSender.kt` | PPoG packet sender | ~30 | New |
| `jvmMain/kotlin/.../uart/UartPPoGReceiver.kt` | UART receiver loop | ~40 | New |
| `jvmMain/kotlin/.../uart/PebbleUart.kt` | TransportConnector impl | ~60 | New |
| `jvmMain/kotlin/.../di/LibPebbleModule.jvm.kt` | Platform module | ~50 | Modify |
| `jvmMain/java/.../bridge/LibPebble3Bridge.java` | JNI bridge | ~50 | New |
| **cyberdeck-service (C++)** |
| `src/jni/libpebble3_jni.hpp` | JNI wrapper header | ~50 | New |
| `src/jni/libpebble3_jni.cpp` | JNI wrapper impl | ~150 | New |
| `src/main.cpp` | Main entry point | ~150 | Replace |
| `Makefile` | Build configuration | - | Modify |
| **REMOVED** |
| ~~`src/uart.hpp/cpp`~~ | ~~UART I/O~~ | - | DELETE |
| ~~`src/pebble/cobs.c`~~ | ~~COBS framing~~ | - | DELETE |
| ~~`src/pebble/k230_protocol.proto`~~ | ~~nanopb protocol~~ | - | DELETE |

---

## Firmware Test Infrastructure (Existing)

The firmware already has comprehensive test commands for PPOUART:

**Console commands** (from `k230_manager.c`):
- `k230 ppouart test` - **Primary test command**
  - Sends data packet with prefix `0xBEEFCAFE` (4 bytes)
  - Followed by message "Hello PPOUART!" (14 bytes)
  - Total payload: 18 bytes
  - Automatically manages sequence numbers

- `k230 ppouart data <sn>` - Send data packet with specific sequence number
- `k230 ppouart ack <sn>` - Send ack for sequence number
- `k230 ppouart handshake` - Guided reset handshake
- `k230 stats` - Show TX/RX frame counts and error statistics
- `k230 stats_reset` - Reset counters

**Test prefix constant:**
```c
// From k230_manager.c line 28
static const uint8_t s_ppouart_test_prefix[] = {0xBE, 0xEF, 0xCA, 0xFE};
```

This allows libpebble3 to easily identify and verify test messages without implementing custom endpoints yet.

---

## Build and Verification Plan

### Step 1: Build libpebble3 with UART transport
```bash
cd /store/git/cyberdeck-service/mobileapp/libpebble3
./gradlew :libpebble3:jvmCompileKotlin  # Verify compilation
./gradlew :libpebble3:jvmJar            # Build JAR with all dependencies
# Output: build/libs/libpebble3-jvm.jar
```

### Step 2: Unit test COBS
```bash
./gradlew :libpebble3:jvmTest --tests "*CobsTest"
# Verify: encode/decode round-trip
# Verify: streaming decoder handles frame boundaries
```

### Step 3: Build JNI bridge
```bash
cd /store/git/cyberdeck-service
export JAVA_HOME=/usr/lib/jvm/java-11-openjdk-riscv64
make libpebble3_jni.so
# Verify: ldd libpebble3_jni.so shows libjvm.so
```

### Step 4: Build simplified cyberdeck_daemon
```bash
make clean
make cyberdeck_daemon
# Much smaller binary now - no UART/COBS code
```

### Step 5: Test on K230 (Two Terminals)

```bash
# Terminal 1: Deploy and run service
scp build/libs/libpebble3-jvm.jar k230:/opt/cyberdeck/
scp libpebble3_jni.so k230:/usr/lib/
scp cyberdeck_daemon k230:/opt/cyberdeck/

cd /opt/cyberdeck
export LD_LIBRARY_PATH=/usr/lib
./cyberdeck_daemon
```

Expected output in Terminal 1:
```
libpebble3 initialized and waiting for test data...

Firmware test commands:
  k230 ppouart test       - Send test message with 0xBEEFCAFE prefix
  k230 ppouart data 0     - Send data packet with SN=0
  k230 stats              - Show TX/RX statistics

Press Ctrl+C to exit
```

```bash
# Terminal 2: Firmware console
# Send test message
k230 ppouart test
```

Expected output in Terminal 1:
```
Received on endpoint 0 (18 bytes): BE EF CA FE 48 65 6C 6C 6F 20 50 50 4F 55 41 52 54 21
  ✓ Test prefix detected!
  Message: Hello PPOUART!
```

```bash
# Terminal 2: Verify statistics
k230 stats
```

Expected output in Terminal 2:
```
K230 State: Waiting
PPOUART Stats: tx=1 rx=0 cobs_err=0 ack_to=0
```

### Verification Checklist:

**Basic Communication:**
1. ✅ JVM starts without errors
2. ✅ UART opens successfully (/dev/ttyS2)
3. ✅ PPoG reset handshake completes (ResetRequest → ResetComplete exchange)
4. ✅ CommSession opens on firmware side
5. ✅ Firmware can send test data (`k230 ppouart test`)
6. ✅ libpebble3 receives and decodes test data
7. ✅ Test prefix (0xBEEFCAFE) detected correctly
8. ✅ Message payload ("Hello PPOUART!") extracted
9. ✅ Firmware stats show tx_frames incremented

**Phone Client Recognition:**
10. ✅ Firmware sends version request to endpoint 0x0011
11. ✅ libpebble3 responds with phone version/capabilities
12. ✅ Firmware recognizes K230 as phone client (check firmware logs for "System session opened")
13. ✅ `comm_session_get_system_session()` returns valid session
14. ✅ System services initialize (optional: check service logs)
15. ✅ No crashes or errors during operation

---

## Success Criteria (Phase 2a: Phone Client Recognition)

**Core Communication:**
1. ✅ COBS round-trip unit tests pass
2. ✅ libpebble3-jvm.jar builds with UART transport
3. ✅ JNI bridge compiles and links
4. ✅ cyberdeck_daemon simplified to ~80 lines (test version)
5. ✅ UART opens successfully from Kotlin (/dev/ttyS2)
6. ✅ PPoG handshake completes successfully
7. ✅ CommSession opens on firmware side

**Phone Client Recognition:**
8. ✅ Firmware sends version request (endpoint 0x0011)
9. ✅ libpebble3 responds with phone capabilities
10. ✅ Firmware recognizes K230 as valid phone client
11. ✅ System services initialize (notifications, timeline, etc.)

**Testing:**
12. ✅ Firmware test command `k230 ppouart test` sends data
13. ✅ libpebble3 receives test data with 0xBEEFCAFE prefix
14. ✅ Firmware `k230 stats` shows tx_frames > 0
15. ✅ Stable operation, no crashes or memory leaks

**Phase 2a includes:**
- Endpoint 0x0011 (Session Remote Version) response for phone client recognition
- Meta protocol (0x0000) basic error handling

**What happens when firmware recognizes K230 as phone:**
- ✅ System services auto-initialize (no firmware changes needed)
- ✅ Notifications service becomes available
- ✅ Timeline/calendar service becomes available
- ✅ Music control service becomes available
- ✅ Phone call service becomes available
- ✅ All existing PP endpoints work transparently over UART
- ✅ Firmware treats UART connection identically to BLE phone connection

**Deferred to Phase 2b:**
- Custom keyboard/touchpad endpoints (0x1000, 0x1001)
- UInput device integration
- Endpoint message format definition
- Time sync implementation (endpoint 0x000b)
- Phone service handlers (endpoint 0x0021)
- Actual notification/timeline message handling

---

## Benefits of This Architecture

**Before:**
- C++ handles: UART, COBS, nanopb, protocol logic (500+ lines)
- libpebble3: Unused in this context
- Duplicate implementations (COBS in firmware, COBS in service)

**After:**
- C++ handles: JNI glue + UInput only (~150 lines)
- libpebble3: Complete protocol stack (UART+COBS+PPoG+PP)
- Single source of truth, reusable across platforms

**Key Wins:**
1. **Simplicity:** C++ code reduced by 70%
2. **Maintainability:** All protocol logic in one place
3. **Reusability:** UART transport can be used by other Kotlin applications
4. **Testability:** Kotlin code easier to unit test than C++
5. **Consistency:** Mirrors existing BLE transport architecture

---

## Dependencies

**On K230:**
- OpenJDK 11+ for RISC-V64
- libjvm.so
- stty (for termios configuration)

**Build-time:**
- Java JDK with JNI headers
- Gradle
- g++ with C++20 support
