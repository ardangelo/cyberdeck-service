import time
import digitalio
import microcontroller
import busio
import supervisor

import io
import msgpack
import binascii

supervisor.runtime.autoreload = False

# ---------------- Debug controls ----------------
DEBUG = True
DUMP_RX_BYTES = True          # print raw RX chunks (hex) as they arrive
DUMP_RX_FRAMES = True         # print parsed frame headers + CRC status
DUMP_RX_MSGPACK = True        # try to decode msgpack payloads and print map
DUMP_TX_FRAMES = True         # print TX frames (type/len/crc)
MAX_HEX = 96                  # max bytes to print per raw dump line

def _ts_ms():
    return int(time.monotonic() * 1000)

def dbg(msg):
    if DEBUG:
        print("[{:>10} ms] {}".format(_ts_ms(), msg))

def hex_bytes(b: bytes, max_len=MAX_HEX):
    if not b:
        return ""
    if len(b) > max_len:
        return binascii.hexlify(b[:max_len]).decode() + "..."
    return binascii.hexlify(b).decode()

# ---------------- Power / UART (required header) ----------------
nrf_periph = digitalio.DigitalInOut(microcontroller.pin.P1_10)
nrf_periph.direction = digitalio.Direction.OUTPUT
nrf_periph.value = 1

k230_pw_en = digitalio.DigitalInOut(microcontroller.pin.P0_28)
k230_pw_en.direction = digitalio.Direction.OUTPUT
k230_pw_en.value = 1
time.sleep(0.1)

uart = busio.UART(
    microcontroller.pin.P0_25,
    microcontroller.pin.P0_24,
    baudrate=115200,
    timeout=0,
)

# ---------------- TCA8418 keypad scan IC ----------------
TCA8418_ADDR = 0x34

i2c = busio.I2C(microcontroller.pin.P0_11, microcontroller.pin.P0_12)
int_pin = digitalio.DigitalInOut(microcontroller.pin.P1_14)
int_pin.direction = digitalio.Direction.INPUT
int_pin.pull = digitalio.Pull.UP

def write_register(reg, value):
    while not i2c.try_lock():
        pass
    i2c.writeto(TCA8418_ADDR, bytes([reg, value]))
    i2c.unlock()
    time.sleep(0.001)

def read_register(reg):
    while not i2c.try_lock():
        pass
    i2c.writeto_then_readfrom(TCA8418_ADDR, bytes([reg]), result := bytearray(1))
    i2c.unlock()
    return result[0]

def init_scan_ic(rows=7, cols=7):
    write_register(0x23, 0x00)
    write_register(0x24, 0x00)
    write_register(0x25, 0x00)

    write_register(0x20, 0xff)
    write_register(0x21, 0xff)
    write_register(0x22, 0xff)

    write_register(0x26, 0x00)
    write_register(0x27, 0x00)
    write_register(0x28, 0x00)

    write_register(0x1a, 0xff)
    write_register(0x1b, 0xff)
    write_register(0x1c, 0xff)

    rmask = (1 << rows) - 1
    cmask = (1 << cols) - 1
    write_register(0x1d, rmask)
    write_register(0x1e, cmask)

    val = read_register(0x01)
    val = val | 0x02 | 0x01
    write_register(0x01, val)

def event_avail():
    return read_register(0x03) & 0x0f

def get_event():
    return read_register(0x04)

def flush_events():
    count = 0
    while get_event():
        count += 1
    read_register(0x11)
    read_register(0x12)
    read_register(0x13)
    write_register(0x02, 3)
    return count

# ---------------- UART framing + msgpack ----------------
MAGIC0 = 0xA5
MAGIC1 = 0x5A
PROTO_VERSION = 1

# NOTE: These constants currently match your pasted test code.
# If daemon uses different values, the debug below will show unexpected msg_type values.
MSG_STARTUP = 0x01
MSG_STARTUP_ACK = 0x02
MSG_SHUTDOWN = 0x03
MSG_KEY_EVENT = 0x10

def crc32_ieee(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            mask = -(crc & 1)
            crc = (crc >> 1) ^ (0xEDB88320 & mask)
    return (~crc) & 0xFFFFFFFF

def u32_le(v: int) -> bytes:
    return bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF))

def build_frame(msg_type: int, payload_bytes: bytes) -> bytes:
    hdr = bytes((MAGIC0, MAGIC1, PROTO_VERSION, msg_type)) + u32_le(len(payload_bytes))
    body = hdr + payload_bytes
    crc = crc32_ieee(body)
    return body + u32_le(crc)

def send_msgpack_map(msg_type: int, m: dict):
    bio = io.BytesIO()
    msgpack.pack(m, bio)              # CircuitPython requires a stream
    payload = bio.getvalue()

    frame = build_frame(msg_type, payload)

    if DUMP_TX_FRAMES:
        dbg("TX frame: msg_type=0x{:02x} payload_len={} crc=0x{:08x}".format(
            msg_type, len(payload), crc32_ieee(frame[:-4])
        ))
        dbg("TX raw: {}".format(hex_bytes(frame)))
        if DUMP_RX_MSGPACK:
            dbg("TX msgpack: {}".format(m))

    uart.write(frame)

def read_some():
    # Drain whatever is waiting (best effort).
    n = getattr(uart, "in_waiting", 0)
    if n and n > 0:
        b = uart.read(n)
        if b and DUMP_RX_BYTES:
            dbg("RX chunk ({}): {}".format(len(b), hex_bytes(b)))
        return b or b""
    # Fallback: try a small read
    b = uart.read(64)
    if b and DUMP_RX_BYTES:
        dbg("RX chunk ({}): {}".format(len(b), hex_bytes(b)))
    return b or b""

def read_exact(n: int, timeout_s: float) -> bytes:
    buf = bytearray()
    t0 = time.monotonic()
    while len(buf) < n:
        if timeout_s is not None and (time.monotonic() - t0) > timeout_s:
            if DEBUG:
                dbg("read_exact timeout: need={}, got={}".format(n, len(buf)))
            return b""
        chunk = uart.read(n - len(buf))
        if chunk:
            if DUMP_RX_BYTES:
                dbg("RX exact chunk ({}): {}".format(len(chunk), hex_bytes(chunk)))
            buf.extend(chunk)
        else:
            time.sleep(0.005)
    return bytes(buf)

def find_magic(timeout_s: float) -> bool:
    t0 = time.monotonic()
    scanned = 0
    while True:
        if timeout_s is not None and (time.monotonic() - t0) > timeout_s:
            dbg("find_magic timeout after scanning {} bytes".format(scanned))
            return False
        b = uart.read(1)
        if not b:
            time.sleep(0.005)
            continue
        scanned += 1
        if DUMP_RX_BYTES:
            dbg("RX byte: {}".format(hex_bytes(b)))
        if b[0] != MAGIC0:
            continue
        b2 = read_exact(1, timeout_s)
        if not b2:
            return False
        if b2[0] == MAGIC1:
            return True

def read_frame(timeout_s: float):
    if not find_magic(timeout_s):
        return None

    hdr = read_exact(1 + 1 + 4, timeout_s)  # version, msg_type, payload_len
    if not hdr:
        return None

    ver = hdr[0]
    msg_type = hdr[1]
    plen = hdr[2] | (hdr[3] << 8) | (hdr[4] << 16) | (hdr[5] << 24)

    payload = read_exact(plen, timeout_s) if plen else b""
    if plen and not payload:
        return None

    crc_bytes = read_exact(4, timeout_s)
    if not crc_bytes:
        return None
    got_crc = crc_bytes[0] | (crc_bytes[1] << 8) | (crc_bytes[2] << 16) | (crc_bytes[3] << 24)

    body = bytes((MAGIC0, MAGIC1)) + hdr + payload
    calc_crc = crc32_ieee(body)

    ok = (ver == PROTO_VERSION) and (got_crc == calc_crc)

    if DUMP_RX_FRAMES:
        dbg("RX frame: ver={} msg_type=0x{:02x} plen={} crc_ok={} calc=0x{:08x} got=0x{:08x}".format(
            ver, msg_type, plen, ok, calc_crc, got_crc
        ))
        if plen:
            dbg("RX payload hex: {}".format(hex_bytes(payload)))
        else:
            dbg("RX payload: <empty>")

    if ver != PROTO_VERSION:
        dbg("RX drop: version mismatch (got {}, want {})".format(ver, PROTO_VERSION))
        return None
    if got_crc != calc_crc:
        dbg("RX drop: CRC mismatch")
        return None

    if DUMP_RX_MSGPACK and plen:
        try:
            m = msgpack.unpack(io.BytesIO(payload))
            dbg("RX msgpack decoded: {}".format(m))
        except Exception as e:
            dbg("RX msgpack decode failed: {}".format(repr(e)))

    return (msg_type, payload)

# ---------------- Keymap: row/col -> letter (testing order) ----------------
def build_test_keymap(rows=7, cols=7):
    km = {}
    ch = ord('a')
    for r in range(rows):
        for c in range(cols):
            km[(r, c)] = chr(ch)
            ch += 1
            if ch > ord('z'):
                ch = ord('a')
    return km

ROWS = 7
COLS = 7
keymap = build_test_keymap(ROWS, COLS)

# ---------------- Startup / shutdown handshake ----------------
def wait_for_startup_and_ack():
    dbg("Waiting for MSG_STARTUP from daemon (expect msg_type=0x{:02x})".format(MSG_STARTUP))
    while True:
        fr = read_frame(timeout_s=10.0)
        if not fr:
            continue
        msg_type, payload = fr

        if msg_type != MSG_STARTUP:
            dbg("Ignoring msg_type=0x{:02x} (wanted 0x{:02x})".format(msg_type, MSG_STARTUP))
            continue

        try:
            m = msgpack.unpack(io.BytesIO(payload))
        except Exception as e:
            dbg("MSG_STARTUP payload msgpack decode failed: {}".format(repr(e)))
            continue

        sid = m.get(1, None)
        flags = m.get(2, None)
        dbg("Got MSG_STARTUP sid={} flags={}".format(sid, flags))

        if sid is None:
            dbg("MSG_STARTUP missing key 1 (session_id); continuing")
            continue

        send_msgpack_map(MSG_STARTUP_ACK, {1: int(sid)})
        dbg("Sent MSG_STARTUP_ACK for sid={}".format(int(sid)))
        return int(sid)

def maybe_handle_shutdown(session_id: int):
    fr = read_frame(timeout_s=0.0)
    if not fr:
        return False
    msg_type, payload = fr

    if msg_type != MSG_SHUTDOWN:
        dbg("RX non-shutdown msg_type=0x{:02x} while running".format(msg_type))
        return False

    try:
        m = msgpack.unpack(io.BytesIO(payload)) if payload else {}
    except Exception as e:
        dbg("Got MSG_SHUTDOWN but msgpack decode failed: {}".format(repr(e)))
        return True

    sid = m.get(1, None)
    reason = m.get(2, "")
    dbg("Got MSG_SHUTDOWN sid={} reason={!r} (local session_id={})".format(sid, reason, session_id))
    return True

# ---------------- Key event sending ----------------
def send_raw_key_event(byte_val: int, is_down: bool):
    ev_bin = bytes((byte_val & 0xFF, 1 if is_down else 0))
    if DEBUG:
        dbg("TX KEY_EVENT: c=0x{:02x} is_down={}".format(byte_val & 0xFF, is_down))
    send_msgpack_map(MSG_KEY_EVENT, {2: ev_bin})

# ---------------- Main ----------------
init_scan_ic(rows=ROWS, cols=COLS)
dbg("Scan IC initialized")

flushed = flush_events()
dbg("Flushed {} events".format(flushed))

# Optional: show if UART already has data pending (useful for "only appears on exit" symptoms)
try:
    dbg("UART in_waiting at start: {}".format(getattr(uart, "in_waiting", "N/A")))
except Exception:
    pass

session_id = wait_for_startup_and_ack()
dbg("Entering keypad loop (session_id={})".format(session_id))

while True:
    if maybe_handle_shutdown(session_id):
        dbg("Shutdown requested; stopping.")
        break

    if int_pin.value:
        # Periodically drain any stray UART bytes for visibility
        if DEBUG and DUMP_RX_BYTES:
            _ = read_some()
        time.sleep(0.01)
        continue

    dbg("Keypad interrupt asserted; draining events")

    while event_avail():
        key_event = get_event()
        if not key_event:
            dbg("get_event returned 0; breaking")
            break

        write_register(0x02, 0x01)

        pressed = (key_event & 0x80) != 0
        key_code = key_event & 0x7F

        row = (key_code >> 3) & 0x0F
        col = (key_code >> 0) & 0x07

        ch = keymap.get((row, col), None)
        dbg("TCA key_event=0x{:02x} pressed={} key_code={} row={} col={} ch={!r}".format(
            key_event, pressed, key_code, row, col, ch
        ))

        if ch is None:
            continue

        b = ord(ch) & 0xFF
        send_raw_key_event(b, pressed)

    time.sleep(0.005)
