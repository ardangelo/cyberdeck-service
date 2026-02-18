# Phase 3 Spec: Custom PP Endpoint for Input Events

## Goal

Create a **custom Pebble Protocol endpoint** for forwarding keyboard, formatted keyboard, and touchpad events from the nRF firmware to the K230 Linux side over the PPOUART transport established in Phases 1 & 2. On the K230, deserialize these events and inject them into Linux via [uinput](file:///store/git/cyberdeck-service/fake_uinput), replacing the old nanopb-based forwarding in [k230_manager.c](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c).

---

## Architecture Overview

```
┌─────── nRF Firmware ───────┐         ┌──────── K230 Linux (libpebble3) ────────┐
│                             │         │                                          │
│  keyboard/touchpad driver   │         │   K230InputService                       │
│         │                   │         │       │                                  │
│  event_service (PEBBLE_     │         │  PebbleProtocolHandler                   │
│   RAW_KEYBOARD_EVENT, etc.) │         │       │.inboundMessages                  │
│         │                   │         │       ▼                                  │
│  k230_input_endpoint.c      │         │  deserialize K230InputPacket             │
│    subscribe to events      │         │       │                                  │
│    serialize to PP payload  │         │       ▼                                  │
│    comm_session_send_data   │  UART   │  UInputDevice                            │
│    ──────────────────────── │ ──────► │    emit_key / emit_rel / emit_sync       │
│  Endpoint 0xCD01            │  PPoG   │                                          │
│                             │         │                                          │
│  (also receive commands     │  ◄───── │  K230InputService.sendCommand(...)       │
│   from K230 → nRF)          │         │                                          │
└─────────────────────────────┘         └──────────────────────────────────────────┘
```

---

## Wire Format

### Endpoint ID

Use **`0xCD01`** (52481 decimal) — in the private custom range, unlikely to collide with existing Pebble endpoints.

### Message Types

The endpoint carries a 1-byte message type followed by type-specific payload:

| Type byte | Name | Direction | Payload |
|-----------|------|-----------|---------|
| `0x01` | RawKeyboard | nRF → K230 | `uint8 keycode` + `uint8 is_down` |
| `0x02` | FormattedKeyboard | nRF → K230 | `uint8[4] utf8` + `uint8 control` + `uint8 alt` |
| `0x03` | RawTouchpad | nRF → K230 | `int8 delta_x` + `int8 delta_y` + `uint8 surface_qual` |
| `0x10` | EnableKeyForward | K230 → nRF | *(empty)* |
| `0x11` | DisableKeyForward | K230 → nRF | *(empty)* |
| `0x12` | EnableTouchpadForward | K230 → nRF | *(empty)* |
| `0x13` | DisableTouchpadForward | K230 → nRF | *(empty)* |

All multi-byte integers are **big-endian** (Pebble Protocol convention).

### PP packet layout

```
┌──────────────┬──────────────┬────────────┬────────────────┐
│ PP length    │ PP endpoint  │ msg type   │ payload...     │
│ (2B BE)      │ 0xCD01 (2B)  │ (1B)       │ (variable)     │
└──────────────┴──────────────┴────────────┴────────────────┘
       ▲ standard PP header         ▲ our custom payload
```

> [!NOTE]
> The PP header (length + endpoint) is handled automatically by `CommSession`/[PebbleProtocolStreams](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/PebbleProtocolRunner.kt#37-45). The endpoint callback on firmware receives only the payload bytes (msg type + data). Similarly, `PebbleProtocolHandler.send(packet)` handles the header.

---

## Reference Code Map

### Firmware files to study

| File | Purpose | What to look for |
|------|---------|-------------------|
| [music_endpoint.c](file:///store/git/cyberdeck-firmware/src/fw/services/normal/music_endpoint.c) | **Reference endpoint** (304 lines) | Callback pattern: `void cb(CommSession*, const uint8_t*, size_t)`, switch on first byte, `comm_session_send_data()` for outbound |
| [protocol_endpoints_table.json](file:///store/git/cyberdeck-firmware/src/fw/services/common/comm_session/protocol_endpoints_table.json) | Endpoint registration | Add `[52481, "0xcd01", "private", "k230_input_protocol_msg_callback", null, null]` to `normal_fw_only` |
| [keyboard_manager.h](file:///store/git/cyberdeck-firmware/src/fw/services/common/keyboard_manager.h) | Event subscribe API | `sys_keyboard_manager_raw_event_subscribe()`, `sys_keyboard_manager_formatted_event_subscribe()`, `sys_keyboard_manager_raw_touchpad_event_subscribe()` |
| [keyboard_manager_types.h](file:///store/git/cyberdeck-firmware/src/fw/services/common/keyboard_manager_types.h) | Event structs | [RawKeyboardEvent](file:///store/git/cyberdeck-service/src/pebble/k230_protocol.proto#26-30) {uint8 c, bool is_down}, `FormattedKeyboardEvent` {uint8[4] utf8, bool control, bool alt}, [RawTouchpadEvent](file:///store/git/cyberdeck-service/src/pebble/k230_protocol.proto#31-36) {int8 delta_x, int8 delta_y, uint8 surface_qual} |
| [k230_manager.c](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c) | **Current key forwarding** (lines 228-278) | [prv_key_forward_event_handler()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#230-238) subscribes to `PEBBLE_RAW_KEYBOARD_EVENT` via event_service, calls [k230_manager_send_keyboard_event()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#92-104). Replace this with PP endpoint sends |
| [session.h](file:///store/git/cyberdeck-firmware/src/fw/services/common/comm_session/session.h) | CommSession API | `comm_session_send_data()`, `comm_session_get_system_session()` |

### Firmware files to modify

| File | Modification |
|------|-------------|
| [protocol_endpoints_table.json](file:///store/git/cyberdeck-firmware/src/fw/services/common/comm_session/protocol_endpoints_table.json) | Add new endpoint entry |
| [k230_manager.c](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c) | Remove nanopb-based key forwarding, replace with calls to new endpoint module. Remove [k230_manager_send_keyboard_event()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#92-104) |
| [k230_manager.h](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.h) | Remove [k230_manager_send_keyboard_event()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#92-104) declaration |

### Firmware files to create

| File | Purpose |
|------|---------|
| `src/fw/services/normal/k230_input_endpoint.c` | New endpoint callback + event forwarding |
| `src/fw/services/normal/k230_input_endpoint.h` | Public API: `k230_input_endpoint_init()`, `k230_input_endpoint_deinit()` |

### libpebble3 files to study

| File | Purpose | What to look for |
|------|---------|-------------------|
| [System.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/packets/System.kt) | **Reference** packet definitions | [PingPong](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/packets/System.kt#502-525) class: `PebblePacket` subclass, `StructMapper` fields (`SUByte`, `SUInt`), `PacketRegistry.register()` |
| [ProtocolEndpoint.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/protocolhelpers/ProtocolEndpoint.kt) | Endpoint enum | Add `K230_INPUT(0xCD01u)` entry |
| [PebbleProtocolHandler.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/PebbleProtocolHandler.kt) | Handler interface | [send(PebblePacket)](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/connection/PebbleProtocolHandler.kt#37-41), `inboundMessages: SharedFlow<PebblePacket>` |
| [SystemService.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/services/SystemService.kt) | **Reference** service pattern | [init()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/services/SystemService.kt#151-189) collects `protocolHandler.inboundMessages`, [sendPing()](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/services/SystemService.kt#125-132) sends via `protocolHandler.send()` |
| [main.cpp](file:///store/git/cyberdeck-service/src/main.cpp) | **Reference** uinput handling | [UInputDevice](file:///store/git/cyberdeck-service/src/main.cpp#184-185) class (lines 115-232): ioctl setup, [emit_key()](file:///store/git/cyberdeck-service/src/main.cpp#203-206), [emit_rel()](file:///store/git/cyberdeck-service/src/main.cpp#207-210), [emit_sync()](file:///store/git/cyberdeck-service/src/main.cpp#211-214). Port to Kotlin/JVM |

### libpebble3 files to modify

| File | Modification |
|------|-------------|
| [ProtocolEndpoint.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/protocolhelpers/ProtocolEndpoint.kt) | Add `K230_INPUT(0xCD01u)` |

### libpebble3 files to create

| File | Purpose |
|------|---------|
| `commonMain/.../packets/K230Input.kt` | Packet classes for all message types |
| `commonMain/.../services/K230InputService.kt` | Service: receives input events, exposes Flows |
| `jvmMain/.../input/UInputDevice.kt` | JVM uinput wrapper (port from C++) |
| `jvmMain/.../input/K230InputManager.kt` | Connects `K230InputService` → [UInputDevice](file:///store/git/cyberdeck-service/src/main.cpp#184-185) |

---

## Firmware Implementation

### New file: `k230_input_endpoint.c`

```c
// src/fw/services/normal/k230_input_endpoint.c

#include "k230_input_endpoint.h"
#include "services/common/comm_session/session.h"
#include "services/common/keyboard_manager.h"
#include "services/common/keyboard_manager_types.h"
#include "applib/event_service_client.h"
#include "kernel/event_loop.h"
#include "system/logging.h"

#define K230_INPUT_ENDPOINT 0xCD01

// Message types
typedef enum {
  K230InputMsgType_RawKeyboard = 0x01,
  K230InputMsgType_FormattedKeyboard = 0x02,
  K230InputMsgType_RawTouchpad = 0x03,
  K230InputMsgType_EnableKeyForward = 0x10,
  K230InputMsgType_DisableKeyForward = 0x11,
  K230InputMsgType_EnableTouchpadForward = 0x12,
  K230InputMsgType_DisableTouchpadForward = 0x13,
} K230InputMsgType;

// State
static EventServiceInfo s_key_event_info;
static EventServiceInfo s_touchpad_event_info;
static bool s_key_fwd_active = false;
static bool s_touchpad_fwd_active = false;

// ---- Outbound: nRF → K230 ----

static void prv_send_raw_keyboard(const RawKeyboardEvent *rke) {
  CommSession *session = comm_session_get_system_session();
  if (!session) return;
  
  uint8_t payload[3] = {
    K230InputMsgType_RawKeyboard,
    rke->c,
    rke->is_down ? 1 : 0,
  };
  comm_session_send_data(session, K230_INPUT_ENDPOINT,
                         payload, sizeof(payload),
                         COMM_SESSION_DEFAULT_TIMEOUT);
}

static void prv_send_formatted_keyboard(const FormattedKeyboardEvent *fke) {
  CommSession *session = comm_session_get_system_session();
  if (!session) return;

  uint8_t payload[7] = {
    K230InputMsgType_FormattedKeyboard,
    fke->utf8[0], fke->utf8[1], fke->utf8[2], fke->utf8[3],
    fke->control ? 1 : 0,
    fke->alt ? 1 : 0,
  };
  comm_session_send_data(session, K230_INPUT_ENDPOINT,
                         payload, sizeof(payload),
                         COMM_SESSION_DEFAULT_TIMEOUT);
}

static void prv_send_raw_touchpad(const RawTouchpadEvent *rte) {
  CommSession *session = comm_session_get_system_session();
  if (!session) return;

  uint8_t payload[4] = {
    K230InputMsgType_RawTouchpad,
    (uint8_t)rte->delta_x,
    (uint8_t)rte->delta_y,
    rte->surface_qual,
  };
  comm_session_send_data(session, K230_INPUT_ENDPOINT,
                         payload, sizeof(payload),
                         COMM_SESSION_DEFAULT_TIMEOUT);
}

// ---- Event service handlers ----

static void prv_key_event_handler(PebbleEvent *e, void *context) {
  prv_send_raw_keyboard(&e->raw_keyboard_event.rke);
}

static void prv_touchpad_event_handler(PebbleEvent *e, void *context) {
  prv_send_raw_touchpad(&e->raw_touchpad_event.rte);
}

// ---- Inbound: K230 → nRF (commands) ----

void k230_input_protocol_msg_callback(CommSession *session,
                                       const uint8_t *msg, size_t length) {
  if (length < 1) return;
  
  switch (msg[0]) {
    case K230InputMsgType_EnableKeyForward:
      if (!s_key_fwd_active) {
        s_key_event_info = (EventServiceInfo) {
          .type = PEBBLE_RAW_KEYBOARD_EVENT,
          .handler = prv_key_event_handler,
        };
        event_service_client_subscribe(&s_key_event_info);
        s_key_fwd_active = true;
      }
      break;
    case K230InputMsgType_DisableKeyForward:
      if (s_key_fwd_active) {
        event_service_client_unsubscribe(&s_key_event_info);
        s_key_fwd_active = false;
      }
      break;
    case K230InputMsgType_EnableTouchpadForward:
      if (!s_touchpad_fwd_active) {
        s_touchpad_event_info = (EventServiceInfo) {
          .type = PEBBLE_RAW_TOUCHPAD_EVENT,
          .handler = prv_touchpad_event_handler,
        };
        event_service_client_subscribe(&s_touchpad_event_info);
        s_touchpad_fwd_active = true;
      }
      break;
    case K230InputMsgType_DisableTouchpadForward:
      if (s_touchpad_fwd_active) {
        event_service_client_unsubscribe(&s_touchpad_event_info);
        s_touchpad_fwd_active = false;
      }
      break;
    default:
      PBL_LOG(LOG_LEVEL_WARNING, "K230Input: unknown msg type 0x%02x", msg[0]);
      break;
  }
}

void k230_input_endpoint_init(void) {
  s_key_fwd_active = false;
  s_touchpad_fwd_active = false;
}

void k230_input_endpoint_deinit(void) {
  if (s_key_fwd_active) {
    event_service_client_unsubscribe(&s_key_event_info);
    s_key_fwd_active = false;
  }
  if (s_touchpad_fwd_active) {
    event_service_client_unsubscribe(&s_touchpad_event_info);
    s_touchpad_fwd_active = false;
  }
}
```

> [!IMPORTANT]
> **Task context for event_service**: The `event_service_client_subscribe` calls must happen on `KernelMain` task. The `k230_input_protocol_msg_callback` is called from the CommSession receiver context. Study how [k230_manager.c:239-261](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#L239-L261) uses `launcher_task_add_callback()` to trampoline to KernelMain before subscribing. Apply the same pattern here.
>
> However, if the endpoint callback runs on KernelMain (via `g_default_kernel_receiver_opt_main` context in the endpoints table), you can subscribe directly. Check which receiver context is configured.

### Modify: [protocol_endpoints_table.json](file:///store/git/cyberdeck-firmware/src/fw/services/common/comm_session/protocol_endpoints_table.json)

Add to `normal_fw_only` array:

```diff
+    [ 52481, "0xcd01", "private", "k230_input_protocol_msg_callback", null, "g_default_kernel_receiver_opt_main" ]
```

Using `g_default_kernel_receiver_opt_main` ensures the callback runs on KernelMain, making event_service subscribe/unsubscribe safe to call directly without trampolining.

### Modify: [k230_manager.c](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c)

Remove the existing key forwarding code (lines 228-278):
- [prv_key_forward_event_handler()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#230-238)
- [prv_key_forward_start_kernel_main()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#239-246)
- [prv_key_forward_stop_kernel_main()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#247-250)
- [k230_manager_key_forward_start()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#251-265)
- [k230_manager_key_forward_stop()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#266-275)
- [k230_manager_key_forward_active()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#276-279)
- `s_key_fwd_event_info` and `s_key_fwd_active` state variables

These are replaced by the new endpoint module. The K230 side explicitly enables/disables forwarding via commands sent through PP.

---

## libpebble3 Implementation

### New file: `K230Input.kt` (Packet definitions)

```kotlin
// commonMain/.../packets/K230Input.kt

package io.rebble.libpebblecommon.packets

import io.rebble.libpebblecommon.protocolhelpers.PebblePacket
import io.rebble.libpebblecommon.protocolhelpers.ProtocolEndpoint
import io.rebble.libpebblecommon.structmapper.SByte
import io.rebble.libpebblecommon.structmapper.SUByte
import io.rebble.libpebblecommon.structmapper.SBytes

sealed class K230InputPacket(type: MessageType) : PebblePacket(endpoint) {
    enum class MessageType(val value: UByte) {
        RawKeyboard(0x01u),
        FormattedKeyboard(0x02u),
        RawTouchpad(0x03u),
        EnableKeyForward(0x10u),
        DisableKeyForward(0x11u),
        EnableTouchpadForward(0x12u),
        DisableTouchpadForward(0x13u),
    }

    val messageType = SUByte(m, type.value)

    init {
        this.type = messageType.get()
    }

    // ---- nRF → K230 (inbound to libpebble3) ----

    class RawKeyboard(
        keycode: UByte = 0u,
        isDown: UByte = 0u,
    ) : K230InputPacket(MessageType.RawKeyboard) {
        val keycode = SUByte(m, keycode)
        val isDown = SUByte(m, isDown)
    }

    class FormattedKeyboard(
        utf8Bytes: UByteArray = UByteArray(4),
        control: UByte = 0u,
        alt: UByte = 0u,
    ) : K230InputPacket(MessageType.FormattedKeyboard) {
        val utf8 = SBytes(m, 4, utf8Bytes)
        val control = SUByte(m, control)
        val alt = SUByte(m, alt)
    }

    class RawTouchpad(
        deltaX: UByte = 0u,  // signed, cast on use
        deltaY: UByte = 0u,
        surfaceQual: UByte = 0u,
    ) : K230InputPacket(MessageType.RawTouchpad) {
        val deltaX = SUByte(m, deltaX)
        val deltaY = SUByte(m, deltaY)
        val surfaceQual = SUByte(m, surfaceQual)
    }

    // ---- K230 → nRF (outbound from libpebble3) ----

    class EnableKeyForward : K230InputPacket(MessageType.EnableKeyForward)
    class DisableKeyForward : K230InputPacket(MessageType.DisableKeyForward)
    class EnableTouchpadForward : K230InputPacket(MessageType.EnableTouchpadForward)
    class DisableTouchpadForward : K230InputPacket(MessageType.DisableTouchpadForward)

    companion object {
        val endpoint = ProtocolEndpoint.K230_INPUT
    }
}

fun k230InputPacketsRegister() {
    PacketRegistry.register(K230InputPacket.endpoint,
        K230InputPacket.MessageType.RawKeyboard.value) { K230InputPacket.RawKeyboard() }
    PacketRegistry.register(K230InputPacket.endpoint,
        K230InputPacket.MessageType.FormattedKeyboard.value) { K230InputPacket.FormattedKeyboard() }
    PacketRegistry.register(K230InputPacket.endpoint,
        K230InputPacket.MessageType.RawTouchpad.value) { K230InputPacket.RawTouchpad() }
}
```

> [!TIP]
> The `SByte` mapper handles signed bytes. For `RawTouchpad.deltaX/deltaY`, use `SUByte` and cast `.toByte()` when reading, since the firmware sends int8 values. Alternatively, check if libpebble3's structmapper has a signed byte type (`SByte`). Study the existing structmapper types in `structmapper/` before deciding.

### Modify: [ProtocolEndpoint.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/protocolhelpers/ProtocolEndpoint.kt)

```diff
 HEALTH_SYNC(911u),
+K230_INPUT(0xCD01u),
 INVALID_ENDPOINT(0xffffu);
```

### New file: `K230InputService.kt`

```kotlin
// commonMain/.../services/K230InputService.kt

package io.rebble.libpebblecommon.services

import io.rebble.libpebblecommon.connection.PebbleProtocolHandler
import io.rebble.libpebblecommon.di.ConnectionCoroutineScope
import io.rebble.libpebblecommon.packets.K230InputPacket
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.launch

class K230InputService(
    private val protocolHandler: PebbleProtocolHandler,
    private val scope: ConnectionCoroutineScope,
) {
    private val _rawKeyboardEvents = MutableSharedFlow<K230InputPacket.RawKeyboard>()
    val rawKeyboardEvents: SharedFlow<K230InputPacket.RawKeyboard> = _rawKeyboardEvents

    private val _formattedKeyboardEvents = MutableSharedFlow<K230InputPacket.FormattedKeyboard>()
    val formattedKeyboardEvents: SharedFlow<K230InputPacket.FormattedKeyboard> = _formattedKeyboardEvents

    private val _rawTouchpadEvents = MutableSharedFlow<K230InputPacket.RawTouchpad>()
    val rawTouchpadEvents: SharedFlow<K230InputPacket.RawTouchpad> = _rawTouchpadEvents

    fun init() {
        scope.launch {
            protocolHandler.inboundMessages.collect { packet ->
                when (packet) {
                    is K230InputPacket.RawKeyboard -> _rawKeyboardEvents.emit(packet)
                    is K230InputPacket.FormattedKeyboard -> _formattedKeyboardEvents.emit(packet)
                    is K230InputPacket.RawTouchpad -> _rawTouchpadEvents.emit(packet)
                    else -> {} // Ignore outbound command types
                }
            }
        }
    }

    suspend fun enableKeyForward() {
        protocolHandler.send(K230InputPacket.EnableKeyForward())
    }

    suspend fun disableKeyForward() {
        protocolHandler.send(K230InputPacket.DisableKeyForward())
    }

    suspend fun enableTouchpadForward() {
        protocolHandler.send(K230InputPacket.EnableTouchpadForward())
    }

    suspend fun disableTouchpadForward() {
        protocolHandler.send(K230InputPacket.DisableTouchpadForward())
    }
}
```

### New file: `UInputDevice.kt` (JVM only)

Port the [UInputDevice](file:///store/git/cyberdeck-service/src/main.cpp#184-185) class from [main.cpp:115-232](file:///store/git/cyberdeck-service/src/main.cpp#L115-L232) to Kotlin/JVM:

```kotlin
// jvmMain/.../input/UInputDevice.kt

class UInputDevice(devicePath: String, skipSetup: Boolean = false) : Closeable {
    private val fd: Int  // File descriptor from native open()
    
    init {
        // Open /dev/uinput via JNI or JNA
        // Configure with ioctl: UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT
        // UI_DEV_SETUP with device name "K230 Virtual Input"
        // UI_DEV_CREATE
    }
    
    fun emitKey(code: Int, value: Int) { /* write input_event */ }
    fun emitRel(code: Int, value: Int) { /* write input_event */ }
    fun emitSync() { /* write EV_SYN */ }
    
    override fun close() { /* UI_DEV_DESTROY, close fd */ }
}
```

> [!IMPORTANT]
> **JNI requirement**: Linux uinput requires `ioctl()` calls that are not available from pure Java. Options:
> 1. **JNA** — `com.sun.jna` library, no native code needed. Can call [open()](file:///store/git/cyberdeck-service/src/main.cpp#82-112), `ioctl()`, [write()](file:///store/git/cyberdeck-firmware/src/fw/drivers/k230/k230.c#137-145) directly. **Recommended**.
> 2. **JNI** — Write a small C wrapper. More work but no additional dependency.
> 3. **Process exec** — Shell out to a helper binary. Fragile.
> 4. **Keep the C++ uinput helper** — Build [fake_uinput](file:///store/git/cyberdeck-service/fake_uinput) or a stripped C helper, communicate via stdin/stdout. Simplest if avoiding JNA dependency.
>
> Keep in mind the target runs on K230 Linux, verify JNA is available for the target arch, or use approach 4.

### New file: `K230InputManager.kt` (JVM only)

Bridges `K230InputService` events to [UInputDevice](file:///store/git/cyberdeck-service/src/main.cpp#184-185):

```kotlin
// jvmMain/.../input/K230InputManager.kt

class K230InputManager(
    private val inputService: K230InputService,
    private val uinputDevice: UInputDevice,
    private val scope: ConnectionCoroutineScope,
) {
    fun start() {
        scope.launch {
            inputService.rawKeyboardEvents.collect { event ->
                val code = event.keycode.get().toInt()
                val value = if (event.isDown.get() > 0u) 1 else 0
                uinputDevice.emitKey(code, value)
                uinputDevice.emitSync()
            }
        }
        scope.launch {
            inputService.rawTouchpadEvents.collect { event ->
                val dx = event.deltaX.get().toByte().toInt()  // Sign-extend
                val dy = event.deltaY.get().toByte().toInt()
                if (dx != 0) uinputDevice.emitRel(REL_X, dx)
                if (dy != 0) uinputDevice.emitRel(REL_Y, dy)
                uinputDevice.emitSync()
            }
        }
    }
}
```

### DI Wiring

Add to [LibPebbleModule.jvm.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/jvmMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.jvm.kt) `platformModule`:

```kotlin
single { UInputDevice(get<UInputConfig>().devicePath) }
single { UInputConfig(devicePath = "/dev/uinput") }
```

Add to [LibPebbleModule.kt](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.kt) `scope<ConnectionScope>`:

```kotlin
scoped { K230InputService(get(), get()) }
scoped { K230InputManager(get(), get(), get()) }
```

### Initialization Sequence

In the JVM entry point (or in `RealPebbleConnector.doAfterConnection()`), after the PP session is established:

```kotlin
// After negotiate() succeeds:
val inputService = koin.get<K230InputService>()
inputService.init()

val inputManager = koin.get<K230InputManager>()
inputManager.start()

// Tell firmware to start sending events
inputService.enableKeyForward()
inputService.enableTouchpadForward()
```

---

## Lifecycle & Connection Management

### Enable on connect

After the Pebble Protocol session is negotiated, the K230 sends `EnableKeyForward` and `EnableTouchpadForward` commands. The firmware starts subscribing to events and forwarding.

### Disable on disconnect

When the PP session closes (transport disconnect):
- **Firmware side**: `k230_input_endpoint_deinit()` called from session close handler, unsubscribes from events
- **K230 side**: `K230InputManager` coroutines cancel when [ConnectionScope](file:///store/git/cyberdeck-service/mobileapp/libpebble3/src/commonMain/kotlin/io/rebble/libpebblecommon/di/LibPebbleModule.kt#205-217) is cancelled

### Reconnect

On reconnect, the K230 sends the enable commands again. The firmware re-subscribes.

> [!WARNING]
> The firmware endpoint callback will be called by CommSession, which means it's coupled to the session lifecycle. When the session closes, pendning `comm_session_send_data` calls will fail gracefully (returns error). But make sure `event_service_client_unsubscribe` is called on session close to stop event accumulation when there's no session to send on.
>
> Register a session close callback in the endpoint init to handle cleanup:
> ```c
> // In k230_input_endpoint_init(), register for session close events
> // to automatically unsubscribe when the PP session drops.
> ```

---

## Cleanup: Files to Remove After Phase 3

Once the new PP endpoint fully replaces the old nanopb forwarding:

| File/Code | Action |
|-----------|--------|
| [k230_manager.c](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c) lines 228-278 | Remove (key forwarding code) |
| [k230_manager.c](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c) [k230_manager_send_keyboard_event()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#92-104) | Remove |
| [k230_manager.c](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c) [k230_manager_send_touchpad_event()](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.c#105-118) | Remove (if exists) |
| [k230_manager.h](file:///store/git/cyberdeck-firmware/src/fw/services/common/k230_manager.h) corresponding declarations | Remove |
| [k230_protocol.proto](file:///store/git/cyberdeck-service/src/pebble/k230_protocol.proto) [RawKeyboardEvent](file:///store/git/cyberdeck-service/src/pebble/k230_protocol.proto#26-30), [RawTouchpadEvent](file:///store/git/cyberdeck-service/src/pebble/k230_protocol.proto#31-36) | Remove (replaced by PP endpoint) |
| [cyberdeck-service/src/main.cpp](file:///store/git/cyberdeck-service/src/main.cpp) entire file | Remove — replaced by JVM process |
| `cyberdeck-service/src/uart.cpp/hpp` | Remove — replaced by Phase 2 UART transport |

> [!CAUTION]
> **Do NOT remove nanopb/COBS infrastructure** from k230_manager.c if there are *other* messages still using the old protocol (e.g., heartbeat, display control, shutdown). Only remove the keyboard/touchpad event-related code. The full removal of nanopb happens when ALL k230_manager functionality is migrated to PP endpoints.

---

## Verification Plan

### 1. Build verification

```bash
# Firmware
cd /store/git/cyberdeck-firmware
./waf build_silk_cyberdeck

# libpebble3
cd /store/git/cyberdeck-service/mobileapp/libpebble3
./gradlew jvmCompileKotlin
```

### 2. Unit test: Packet serialization round-trip

Write a Kotlin test in `commonTest`:
```kotlin
@Test
fun testRawKeyboardRoundTrip() {
    val packet = K230InputPacket.RawKeyboard(keycode = 42u, isDown = 1u)
    val bytes = packet.serialize()
    val decoded = bytes.asPebblePacket<K230InputPacket.RawKeyboard>()
    assertEquals(42u, decoded.keycode.get())
    assertEquals(1u, decoded.isDown.get())
}
```

### 3. Integration test: End-to-end with firmware

1. Flash firmware with new endpoint to nRF
2. Start JVM service on K230: `java -jar cyberdeck-service.jar --uart /dev/ttyS2 --uinput /dev/uinput`
3. Verify PP session established (check logs for WatchInfo)
4. Verify `EnableKeyForward` command sent (check firmware logs)
5. Press keys on the cyberdeck keyboard
6. Verify `evtest /dev/input/eventN` shows key events on K230

### 4. Firmware-only test: Serial console

After flashing, use the firmware prompt:
```
> k230 start_input
> # press keys
# Verify logs show: "K230Input: sending key XX down/up"
> k230 stop_input
```

> [!NOTE]
> If the firmware has a prompt command system, add `k230 start_input` / `k230 stop_input` commands that send appropriate messages to the endpoint callback for testing without the K230 side. Alternatively, the endpoint could auto-start forwarding when the session opens.

---

## Out of Scope for Phase 3

- **Display control** (backlight, power) — Future PP endpoint
- **Shutdown command** — Future PP endpoint  
- **Formatted keyboard → full IME pipeline** — `FormattedKeyboardEvent` is forwarded raw; higher-level IME processing is deferred
- **Touchpad gesture recognition** — Raw deltas only; gesture recognition is a K230 userspace concern
- **Multiple keyboards** — Single input device; multi-device support deferred
