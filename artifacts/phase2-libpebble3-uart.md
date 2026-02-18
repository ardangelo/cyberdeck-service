# Phase 2 Spec: libpebble3 UART Transport on K230

## Goal

Create a **UART-based PPoG transport** on the K230 Linux side using **libpebble3's jvmMain target**, replacing the current C++ nanopb/COBS cyberdeck-service. After this phase, the K230 initiates a PPoG session over UART to the nRF firmware (which runs the Phase 1 PPOUART transport), and a full Pebble Protocol session is established end-to-end.

---

## Architecture Overview

### Current state (cyberdeck-service)

```
K230 Linux:  C++ daemon (main.cpp) → nanopb encode → COBS → UART → nRF
             Uart class (uart.cpp) handles framing
             UInputDevice forwards keyboard/touchpad to Linux evdev
```

### Target state

```
K230 Linux:  JVM process (libpebble3 native-image)
                 │
            UartTransportConnector
                 │
            ┌────┴────┐
            │  PPoG    │ ← existing commonMain state machine
            │  engine  │
            └────┬────┘
                 │
            UartPPoGPacketSender ──→ COBS encode ──→ UART fd write
                                                          │
            UartPPoGReceiver    ←── COBS decode ←── UART fd read
                 │
            PPoGStream.inboundPPoGBytesChannel
                 │
            PebbleProtocolRunner (existing) → endpoint routing
```

---

## Reference Code Map

### libpebble3 files to study

| File | Purpose | Key things |
|------|---------|------------|
| [PPoG.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt) | PPoG state machine (326 lines) | [PPoGPacketSender](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#22-26) interface, [PPoGStream](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#27-28), [initWithResetRequest()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#87-118) (K230 initiates), [runConnection()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#148-274) (data/ack loop) |
| [PPoGPacket.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoGPacket.kt) | Packet format | [serialize()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoGPacket.kt#29-56), [asPPoGPacket()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoGPacket.kt#79-102), [PPoGVersion](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoGPacket.kt#58-66) |
| [TransportConnector.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/TransportConnector.kt) | **Transport interface** (3 methods) | [connect()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/TransportConnector.kt#112-113), [disconnect()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/pebble/PebbleBle.kt#135-140), `disconnected: Deferred<ConnectionFailureReason>` |
| [PebbleBle.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/pebble/PebbleBle.kt) | **Reference** BLE TransportConnector | Study: GATT setup → PPoG start → PebbleProtocolRunner launch. UART version skips BLE-specific steps |
| [PebbleProtocolRunner.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/PebbleProtocolRunner.kt) | PP packet deserialization | Reads from `PebbleProtocolStreams.inboundPPBytes`, parses 4-byte PP headers. **Reuse as-is** |
| [BlePlatformConfig.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/BlePlatformConfig.kt) | Config class | MTU, window sizes, flags. Provide UART-appropriate values |
| [LibPebbleModule.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.kt) | **DI wiring** (580 lines) | [TransportConnector](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/TransportConnector.kt#74-79) resolved at lines 470-482, [PPoGPacketSender](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#22-26) at lines 505-510, [ConnectionScope](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.kt#205-217) factory |
| [LibPebbleModule.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.jvm.kt) | **jvmMain DI stub** — TODO | Must implement `platformModule` to provide UART-specific bindings |

### cyberdeck-service files to study/replace

| File | Purpose | Notes |
|------|---------|-------|
| [main.cpp](file:///store/git/cyberdeck-service/src/main.cpp) | Current daemon (344 lines) | nanopb decode loop, UInput forwarding. **Will be replaced** by JVM process |
| [uart.hpp](file:///store/git/cyberdeck-service/src/uart.hpp) / [uart.cpp](file:///store/git/cyberdeck-service/src/uart.cpp) | COBS-framed UART I/O | Reference for UART setup (termios config at 115200, raw mode). UART config logic can be ported to JVM/native |
| [Makefile](file:///store/git/cyberdeck-service/Makefile) | Build system | Currently C++ with nanopb/COBS. Will need Gradle/GraalVM build instead |

### jvmMain stubs requiring implementation

Found via listing [jvmMain directory](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon) — 25 [.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/main.kt) files, many with `TODO("Not yet implemented")`:

| File | Required Action |
|------|----------------|
| [di/LibPebbleModule.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.jvm.kt) | **Implement** `platformModule` with UART-specific bindings |
| [connection/bt/ble/pebble/PebbleBle.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/pebble/PebbleBle.jvm.kt) | Stub or remove (not used for UART) |
| [connection/bt/ble/transport/GattServer.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/transport/GattServer.jvm.kt) | Stub (not used for UART) |
| [connection/bt/ble/transport/impl/KableBleScanner.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/transport/impl/KableBleScanner.jvm.kt) | Stub (not used for UART) |
| [connection/bt/ble/transport/impl/KableGattClient.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/transport/impl/KableGattClient.jvm.kt) | Stub (not used for UART) |
| [connection/bt/BluetoothState.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/BluetoothState.jvm.kt) | Stub or provide always-on state |
| [connection/bt/Pairing.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/Pairing.jvm.kt) | Stub (no pairing for UART) |
| [database/Database.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/database/Database.jvm.kt) | **Implement** — Room database for JVM (SQLite) |
| Others (`locker/`, [notification/](file:///store/git/cyberdeck-firmware/src/fw/comm/ble/kernel_le_client/ppogatt/ppogatt.c#668-707), etc.) | Provide minimal stubs to satisfy compilation |

---

## Components to Implement

### 1. UartPPoGPacketSender

Implements [PPoGPacketSender](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#22-26) to send PPoG packets over UART with COBS framing.

```kotlin
// New file: jvmMain/.../connection/bt/ble/transport/UartPPoGPacketSender.kt

class UartPPoGPacketSender(
    private val uartOutputStream: OutputStream,  // UART fd wrapped as stream
) : PPoGPacketSender {
    
    override suspend fun sendPacket(packet: ByteArray): Boolean {
        // 1. COBS encode the packet
        // 2. Write: 0x00 + cobsEncoded + 0x00
        // 3. Flush
        return true
    }
    
    override fun wasRestoredWithSubscribedCentral(): Boolean = false
}
```

> [!IMPORTANT]
> **COBS implementation**: You need a Kotlin/JVM COBS encoder/decoder. Options:
> 1. **Port from C** — the existing [cobs.h](file:///store/git/cyberdeck-firmware/src/fw/console/cobs.h) / [cobs.c](file:///store/git/cyberdeck-firmware/src/fw/console/cobs.c) is ~100 lines. Port to Kotlin.
> 2. **JNI** — call the existing C implementation via JNI (more complex, but zero porting effort).
> 3. **Pure Kotlin** — write COBS from spec (simple algorithm, ~30 lines).
>
> Recommendation: **Pure Kotlin** — COBS is trivial to implement and avoids native dependencies.

### 2. UartPPoGReceiver

Background coroutine that reads UART bytes, COBS-decodes frames, and pushes to `PPoGStream.inboundPPoGBytesChannel`.

```kotlin
// New file: jvmMain/.../connection/bt/ble/transport/UartPPoGReceiver.kt

class UartPPoGReceiver(
    private val uartInputStream: InputStream,  // UART fd wrapped as stream
    private val ppoGStream: PPoGStream,
    private val scope: ConnectionCoroutineScope,
) {
    fun start() {
        scope.launch(Dispatchers.IO) {
            val cobsDecoder = CobsStreamDecoder()
            while (isActive) {
                val byte = uartInputStream.read()
                if (byte == -1) break  // EOF
                
                if (byte == 0x00) {
                    // Frame delimiter
                    val frame = cobsDecoder.finish()
                    if (frame != null && frame.isNotEmpty()) {
                        ppoGStream.inboundPPoGBytesChannel.send(frame)
                    }
                    cobsDecoder.reset()
                } else {
                    cobsDecoder.decode(byte.toByte())
                }
            }
        }
    }
}
```

### 3. UartTransportConnector

Implements [TransportConnector](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/TransportConnector.kt#74-79) to manage the UART connection lifecycle and PPoG session.

```kotlin
// New file: jvmMain/.../connection/bt/ble/pebble/PebbleUart.kt

class PebbleUart(
    private val scope: ConnectionCoroutineScope,
    private val ppog: PPoG,
    private val pPoGStream: PPoGStream,
    private val uartConfig: UartConfig,  // path, baud rate
) : TransportConnector {
    
    private val _disconnected = CompletableDeferred<ConnectionFailureReason>()
    override val disconnected: Deferred<ConnectionFailureReason> = _disconnected
    
    private var uartFd: FileDescriptor? = null
    private var receiver: UartPPoGReceiver? = null
    
    override suspend fun connect(lastError: ConnectionFailureReason?): PebbleConnectionResult {
        // 1. Open UART device (termios config: 115200, raw, no flow control)
        // 2. Create InputStream/OutputStream from fd
        // 3. Create UartPPoGReceiver, start it
        // 4. Start PPoG (ppog.run()) — this does the reset handshake
        // 5. Return Success
        return PebbleConnectionResult.Success
    }
    
    override suspend fun disconnect() {
        ppog.close()
        receiver?.stop()
        // Close UART fd
        _disconnected.complete(ConnectionFailureReason.FailedToConnect)
    }
}
```

> [!NOTE]
> **Opening UART on JVM**: Use `java.io.FileInputStream`/`FileOutputStream` on the device path (e.g., [/dev/ttyS2](file:///dev/ttyS2)). For termios configuration (baud rate, raw mode), you'll need JNI or a small native helper. Reference the termios setup in [main.cpp:82-111](file:///store/git/cyberdeck-service/src/main.cpp#L82-L111). Alternatively, configure the terminal from the shell before launching the JVM process (`stty -F /dev/ttyS2 115200 raw`).

### 4. COBS Encoder/Decoder (Kotlin)

Port the COBS algorithm to pure Kotlin:

```kotlin
// New file: jvmMain/.../util/Cobs.kt or commonMain if desired

object Cobs {
    fun encode(data: ByteArray): ByteArray { ... }
    fun decode(data: ByteArray): ByteArray? { ... }
}

class CobsStreamDecoder {
    fun decode(byte: Byte) { ... }
    fun finish(): ByteArray? { ... }
    fun reset() { ... }
}
```

Reference: [COBS Wikipedia](https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing)

### 5. UartConfig

Simple data class for UART configuration:

```kotlin
data class UartConfig(
    val devicePath: String,   // e.g., "/dev/ttyS2"
    val baudRate: Int = 115200,
)
```

---

## DI Wiring

### [LibPebbleModule.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.jvm.kt) — Implement `platformModule`

The `platformModule` is `expect`ed in commonMain and `includes`d at [LibPebbleModule.kt:323](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.kt#L323). It provides platform-specific bindings.

```kotlin
actual val platformModule: Module
    get() = module {
        // UART configuration
        single { UartConfig(devicePath = "/dev/ttyS2") }
        
        // Platform stubs for unused BLE functionality
        single<BlePlatformConfig> {
            BlePlatformConfig(
                pinAddress = false,
                phoneRequestsPairing = false,
                writeConnectivityTrigger = false,
                initialMtu = 256 + 4,  // No BLE MTU limit
                desiredTxWindow = 4,
                desiredRxWindow = 4,
                useNativeMtu = false,
                sendPpogResetOnDisconnection = false,
                delayBleConnectionsAfterAppStart = false,
                delayBleDisconnections = false,
                fallbackToResetRequest = true,  // K230 initiates
                closeGattServerWhenBtDisabled = false,
                supportsBtClassic = false,
            )
        }
    }
```

### Connection Scope modifications

In the `scope<ConnectionScope>` block at [LibPebbleModule.kt:450-574](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.kt#L450-L574), the [TransportConnector](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/TransportConnector.kt#74-79) is resolved at lines 470-482 and [PPoGPacketSender](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#22-26) at lines 505-510.

**Strategy**: Modify the [TransportConnector](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/TransportConnector.kt#74-79) resolution to support a UART path:

```kotlin
// Option A: Add PebbleUartIdentifier type
scoped<TransportConnector> {
    val identifier = get<PebbleIdentifier>()
    when (identifier) {
        is PebbleBleIdentifier -> ... // existing BLE path
        is PebbleUartIdentifier -> get<PebbleUart>()
        else -> TODO("not implemented")
    }
}

// Option B: Simpler — just check if UartConfig is available
scoped<TransportConnector> { get<PebbleUart>() }  // Always UART on JVM
```

For [PPoGPacketSender](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#22-26), wire in `UartPPoGPacketSender`:

```kotlin
scoped<PPoGPacketSender> {
    // On JVM/UART, always use UartPPoGPacketSender
    get<UartPPoGPacketSender>()
}
```

> [!IMPORTANT]
> **Decision**: Whether to modify the commonMain DI (affecting all platforms) or override per-platform in `platformModule`. The cleaner approach is to put UART-specific scope bindings in `platformModule` and have them override the commonMain defaults. This keeps commonMain untouched. However, Koin scope overrides require care — test this.
>
> The simplest approach for a first pass: modify the `scope<ConnectionScope>` block in commonMain to add a UART branch alongside BLE, gated by identifier type.

---

## PPoG Handshake: K230 as Initiator

The PPoG state machine in [PPoG.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt) supports two init modes:

1. **[initWaitingForResetRequest()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#119-147)** — Wait for watch to send ResetRequest (phone-as-server, normal BLE)
2. **[initWithResetRequest()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#87-118)** — Send ResetRequest ourselves (phone-as-client, reversed PPoG)

For UART, the K230 should **initiate** ([initWithResetRequest](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#87-118)). This is controlled by:

```kotlin
// In PPoG.run(), line 42-53:
val params = withTimeoutOrNull(12.seconds) {
    initWaitingForResetRequest()
} ?: withTimeoutOrNull(5.seconds) {
    if (blePlatformConfig.fallbackToResetRequest) {
        initWithResetRequest()
    } else { null }
} ?: throw ConnectionException(...)
```

Set `BlePlatformConfig.fallbackToResetRequest = true`. The PPoG engine will first wait 12s for a ResetRequest from the watch (firmware). If the firmware doesn't initiate, the K230 will initiate after timeout.

> [!TIP]
> For faster startup, consider making the K230 **always** initiate. You can modify `PPoG.run()` to skip [initWaitingForResetRequest()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#119-147) when using UART, or reduce the timeout. A simple approach: set `fallbackToResetRequest = true` and accept the 12-second initial delay. Optimize later.

---

## JVM Entry Point

The libpebble3 JVM target needs a [main()](file:///store/git/cyberdeck-service/src/main.cpp#249-344) function that:

1. Parses command-line args (UART path, uinput path)
2. Calls [initKoin()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.kt#309-580) with appropriate stubs
3. Creates a [ConnectionScope](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.kt#205-217) for the UART connection
4. Starts the connection

The existing [main.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/main.kt) only has [getPlatform()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/main.kt#5-6) and [performPlatformSpecificInit()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/main.kt#7-8). A new entry point is needed:

```kotlin
// New or modified: jvmMain/kotlin/main.kt

fun main(args: Array<String>) {
    val uartPath = args[0]  // e.g., "/dev/ttyS2"
    val uinputPath = args[1]  // e.g., "/dev/uinput"
    
    // Init Koin with stub services
    val koin = initKoin(
        defaultConfig = LibPebbleConfig(...),
        webServices = StubWebServices(),
        appContext = JvmAppContext(...),
        tokenProvider = StubTokenProvider(),
        // ... other stubs
    )
    
    // Get LibPebble3 instance and connect
    val libPebble = koin.get<LibPebble>()
    
    runBlocking {
        // Create connection scope for UART
        val identifier = PebbleUartIdentifier(uartPath)
        libPebble.connectTo(identifier)
        
        // Block until disconnected
        // ...
    }
}
```

> [!WARNING]
> **Stub dependencies**: [initKoin()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.kt#309-580) requires many dependencies (`WebServices`, `TokenProvider`, `TranscriptionProvider`, etc.) that don't apply to the K230 use case. These all need stub implementations. This is significant boilerplate work but is straightforward — each stub just provides no-op implementations.

---

## GraalVM Native Image (Stretch Goal)

For Phase 2, focus on getting a **standard JVM JAR running on the K230**. GraalVM native-image compilation is a stretch goal because:

1. libpebble3 uses reflection-heavy libraries (Koin DI, Ktor, Room/SQLite)
2. Native-image requires explicit reflection configuration
3. Debug iteration is faster with JVM

**Phase 2 deliverable**: A runnable JVM application (`java -jar cyberdeck-service.jar --uart /dev/ttyS2`)

**Future**: Profile startup time and memory. If the JVM is too heavy for K230, investigate:
- GraalVM native-image with reflection configs
- Stripping unused dependencies (Ktor HTTP client, Room DB, etc.)
- Replacing Koin with manual DI for native-image compatibility

---

## Build System

### Gradle

libpebble3 uses Gradle with KMP. The jvmMain target needs:

```kotlin
// In build.gradle.kts for libpebble3
kotlin {
    jvm {
        mainRun {
            mainClass.set("io.rebble.libpebblecommon.MainKt") // or wherever main() lives
        }
    }
}
```

Build and run commands:
```bash
cd mobileapp/libpebble3
./gradlew jvmRun --args="/dev/ttyS2 /dev/uinput"
# OR build a fat JAR:
./gradlew jvmJar
```

### Cross-compilation for K230

The K230 runs Linux on RISC-V or ARM. If building a JVM app, ensure the target has a JRE installed. If using GraalVM native-image, cross-compile for the target architecture.

---

## Handling Existing Service Functionality

The current [cyberdeck_daemon](file:///store/git/cyberdeck-service/cyberdeck_daemon) handles:

| Feature | Current implementation | Phase 2 approach |
|---------|----------------------|-------------------|
| Keyboard events | nanopb `NrfToK230.keyboard` → uinput | **Defer to Phase 3** — will use a custom PP endpoint |
| Touchpad events | nanopb `NrfToK230.touchpad` → uinput | **Defer to Phase 3** |
| Heartbeat | nanopb `NrfToK230.heartbeat` → ack | **Replaced** by PPoG ack mechanism |
| Shutdown | nanopb `K230ToNrf.shutdown` | Can be a PP endpoint or lifecycle event |

For Phase 2, the goal is purely to establish the Pebble Protocol session. Input forwarding will be re-implemented via custom PP endpoints in Phase 3.

---

## Key Simplifications vs BLE

| BLE complexity | UART equivalent |
|---|---|
| BLE scanning + discovery | Not needed — fixed UART path |
| GATT service/characteristic setup | Not needed |
| MTU negotiation | Fixed large MTU (256+) |
| Pairing / bonding | Not needed |
| Connection params subscription | Not needed |
| Multiple concurrent connections | Single UART connection |
| Battery characteristic monitoring | Not needed (or via PP endpoint) |
| Connectivity trigger write | Not needed |

---

## Files to Create

| Path | Purpose |
|------|---------|
| `jvmMain/.../connection/bt/ble/transport/UartPPoGPacketSender.kt` | PPoGPacketSender over UART+COBS |
| `jvmMain/.../connection/bt/ble/transport/UartPPoGReceiver.kt` | UART RX → COBS decode → PPoGStream |
| `jvmMain/.../connection/bt/ble/pebble/PebbleUart.kt` | TransportConnector for UART |
| `jvmMain/.../connection/UartConfig.kt` | UART configuration data class |
| `jvmMain/.../util/Cobs.kt` | Pure Kotlin COBS encoder/decoder |
| `jvmMain/.../di/LibPebbleModule.jvm.kt` | **Modify** — implement `platformModule` |

### Files to stub

Many jvmMain files have `TODO("Not yet implemented")` that will crash at runtime. Provide no-op implementations for:

| File | Stub Strategy |
|------|---------------|
| [GattServer.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/transport/GattServer.jvm.kt) | No-op implementations (not used for UART) |
| [KableBleScanner.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/transport/impl/KableBleScanner.jvm.kt) | Return empty scanner |
| [KableGattClient.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/transport/impl/KableGattClient.jvm.kt) | No-op (not used for UART) |
| [BluetoothState.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/BluetoothState.jvm.kt) | Return always-enabled state |
| [Pairing.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/Pairing.jvm.kt) | No-op |
| [Database.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/database/Database.jvm.kt) | SQLite via JDBC or in-memory stub |
| [PebbleBle.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/pebble/PebbleBle.jvm.kt) | Keep TODO (not reached for UART identifier) |

---

## Verification Plan

### 1. Build verification

```bash
cd /store/git/cyberdeck-service/mobileapp/libpebble3
./gradlew jvmCompileKotlin
```

Ensure all jvmMain sources compile without errors.

### 2. Unit test: COBS round-trip

Write a Kotlin test that verifies COBS encode → decode produces the original data for various payloads (empty, single byte, contains 0x00, max length).

### 3. Unit test: PPoG packet serialization

Verify that `PPoGPacket.serialize()` and [asPPoGPacket()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoGPacket.kt#79-102) work correctly for all packet types over COBS framing.

### 4. Integration test: Loopback

Create a test that:
1. Creates a `UartPPoGPacketSender` and `UartPPoGReceiver` connected to a loopback (pipe or socketpair)
2. Sends PPoG packets through the sender
3. Verifies they arrive at the receiver's [PPoGStream](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/bt/ble/ppog/PPoG.kt#27-28)

### 5. End-to-end test with firmware

Connect to the nRF running Phase 1 PPOUART:
1. Start the JVM service: `java -jar cyberdeck-service.jar --uart /dev/ttyS2`
2. Verify PPoG handshake completes (logs show ResetRequest → ResetComplete)
3. Verify Pebble Protocol negotiation succeeds (WatchInfo received)
4. Verify `CommSession` is open on both sides (firmware prompt: `k230 state` shows Open)

---

## Out of Scope for Phase 2

- **Custom PP endpoint for keyboard/touchpad** — Phase 3
- **UInput forwarding via PP** — Phase 3
- **GraalVM native-image** — Stretch goal; standard JVM first
- **Production Koin/DI cleanup** — Stubs are acceptable for now
- **libpebble3 service initialization** (BlobDB sync, timeline, etc.) — Will work automatically once session is established, but not the focus of testing
