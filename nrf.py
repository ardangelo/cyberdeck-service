# code.py
# CircuitPython: UART framed protocol (magic+hdr+msgpack+crc32) + bitbanged SPI "slave" for bulk.
#
# CircuitPython msgpack requires stream objects:
#   msgpack.pack(obj, stream)
#   msgpack.unpack(stream)
#
# Framing (little-endian):
#   magic[2]=0xA5,0x5A | version(u8)=1 | msg_type(u8) | payload_len(u16) | payload(msgpack) | crc32(u32)
# CRC32 is IEEE (zlib.crc32) over: version|msg_type|payload_len_le|payload

import time
import digitalio
import microcontroller
import busio
import supervisor

import io
import msgpack
import binascii

supervisor.runtime.autoreload = False

# ---------------- Board-specific enables (from your example) ----------------

nrf_periph = digitalio.DigitalInOut(microcontroller.pin.P1_10)
nrf_periph.direction = digitalio.Direction.OUTPUT
nrf_periph.value = 1

k230_pw_en = digitalio.DigitalInOut(microcontroller.pin.P0_28)
k230_pw_en.direction = digitalio.Direction.OUTPUT
k230_pw_en.value = 1
time.sleep(0.1)

# ---------------- UART ----------------

uart = busio.UART(
    microcontroller.pin.P0_25,  # TX
    microcontroller.pin.P0_24,  # RX
    baudrate=115200,
    timeout=0,                  # non-blocking
)

# ---------------- Bitbanged SPI "slave" pins ----------------

CLK = digitalio.DigitalInOut(microcontroller.pin.P0_14)   # SCLK from host
MISO = digitalio.DigitalInOut(microcontroller.pin.P0_13)  # MISO to host
MOSI = digitalio.DigitalInOut(microcontroller.pin.P0_15)  # MOSI from host
CS = digitalio.DigitalInOut(microcontroller.pin.P0_26)    # CS from host

for pin in (CLK, MOSI, CS):
    pin.direction = digitalio.Direction.INPUT
    pin.pull = digitalio.Pull.UP

MISO.direction = digitalio.Direction.OUTPUT
MISO.value = 0

# ---------------- Protocol constants ----------------

MAGIC0 = 0xA5
MAGIC1 = 0x5A
PROTO_VERSION = 1

MSG_KEY_EVENT = 0x01
MSG_STARTUP = 0x02
MSG_BULK_AVAILABLE = 0x10
MSG_BULK_START = 0x11
MSG_BULK_DONE = 0x12
MSG_BULK_REQUEST_TX = 0x13

# ---------------- msgpack stream helpers ----------------

class _ByteArrayWriteStream:
    def __init__(self):
        self.buf = bytearray()
    def write(self, b):
        # msgpack may pass bytes-like objects
        self.buf.extend(b)
        return len(b)

class _BytesReadStream:
    def __init__(self, b):
        self._b = b
        self._i = 0
    def read(self, n=-1):
        if n is None or n < 0:
            n = len(self._b) - self._i
        if self._i >= len(self._b):
            return b""
        chunk = self._b[self._i:self._i + n]
        self._i += len(chunk)
        return chunk

def msgpack_pack_bytes(obj) -> bytes:
    bio = io.BytesIO()
    msgpack.pack(obj, bio)
    return bio.getvalue()

def msgpack_unpack_bytes(b: bytes):
    bio = io.BytesIO(b)
    return msgpack.unpack(bio)

# ---------------- Utilities ----------------

def crc32_ieee(b: bytes) -> int:
    return binascii.crc32(b) & 0xFFFFFFFF

def u16le(n: int) -> bytes:
    return bytes((n & 0xFF, (n >> 8) & 0xFF))

def u32le(n: int) -> bytes:
    return bytes((n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, (n >> 24) & 0xFF))

def crc32_bytes(b: bytes) -> bytes:
    return u32le(crc32_ieee(b))

def u32_to_i32(x: int) -> int:
    x &= 0xFFFFFFFF
    return x if x < 0x80000000 else x - 0x100000000

def try_read_frame(rxbuf: bytearray):
    # Resync to magic
    while True:
        if len(rxbuf) < 2:
            return None
        if rxbuf[0] == MAGIC0 and rxbuf[1] == MAGIC1:
            break
        rxbuf.pop(0)

    if len(rxbuf) < 2 + 1 + 1 + 2 + 4:
        return None

    version = rxbuf[2]
    msg_type = rxbuf[3]
    payload_len = rxbuf[4] | (rxbuf[5] << 8)
    total_len = 2 + 1 + 1 + 2 + payload_len + 4
    if len(rxbuf) < total_len:
        return None

    payload = bytes(rxbuf[6:6 + payload_len])
    crc_recv = (
        rxbuf[6 + payload_len] |
        (rxbuf[7 + payload_len] << 8) |
        (rxbuf[8 + payload_len] << 16) |
        (rxbuf[9 + payload_len] << 24)
    )

    crc_input = bytes((version, msg_type)) + u16le(payload_len) + payload
    crc_calc = crc32_ieee(crc_input)

    if crc_calc != crc_recv:
        rxbuf.pop(0)
        return None

    del rxbuf[:total_len]
    return (version, msg_type, payload)

def send_frame(msg_type: int, mp_obj) -> None:
    payload = msgpack_pack_bytes(mp_obj)
    if len(payload) > 0xFFFF:
        raise ValueError("payload too large")
    hdr = bytes((MAGIC0, MAGIC1, PROTO_VERSION, msg_type)) + u16le(len(payload))
    crc = crc32_ieee(bytes((PROTO_VERSION, msg_type)) + u16le(len(payload)) + payload)
    uart.write(hdr + payload + u32le(crc))

# ---------------- Bitbanged SPI engine ----------------

def spi_wait_cs_low():
    while CS.value:
        pass

def spi_exchange_bytes(tx_bytes: bytes, rx_len: int = 0) -> bytes:
    spi_wait_cs_low()

    tx_i = 0
    tx_byte = tx_bytes[0] if len(tx_bytes) else 0
    tx_bit = 7

    MISO.value = 1 if (tx_byte >> tx_bit) & 1 else 0

    last_clk = CLK.value
    rx = bytearray()
    rx_byte = 0
    rx_bit = 7
    want_bits = rx_len * 8
    got_bits = 0

    while not CS.value:
        clk = CLK.value

        if (not last_clk) and clk:
            if got_bits < want_bits:
                if MOSI.value:
                    rx_byte |= (1 << rx_bit)
                rx_bit -= 1
                got_bits += 1
                if rx_bit < 0:
                    rx.append(rx_byte)
                    rx_byte = 0
                    rx_bit = 7

        if last_clk and (not clk):
            tx_bit -= 1
            if tx_bit < 0:
                tx_i += 1
                tx_byte = tx_bytes[tx_i] if tx_i < len(tx_bytes) else 0
                tx_bit = 7
            MISO.value = 1 if (tx_byte >> tx_bit) & 1 else 0

        last_clk = clk

    return bytes(rx)

# ---------------- Application logic ----------------

bulk_tx_buf = b""
bulk_rx_buf = b""
bulk_id = 1

def wait_startup_confirm(timeout_s=10.0):
    # Announce startup; payload can be simple and packable
    send_frame(MSG_STARTUP, ["nrf-ready", int(time.monotonic())])
    print("Startup: sent announce, waiting for confirm...")

    rxbuf = bytearray()
    t0 = time.monotonic()
    while True:
        chunk = uart.read(256)
        if chunk:
            rxbuf.extend(chunk)

        parsed = try_read_frame(rxbuf)
        if parsed:
            version, msg_type, payload = parsed
            if version == PROTO_VERSION and msg_type == MSG_STARTUP:
                try:
                    obj = msgpack_unpack_bytes(payload)
                except Exception:
                    obj = None
                print("Startup: confirmed:", obj)
                return True

        if time.monotonic() - t0 > timeout_s:
            raise TimeoutError("Startup confirm timeout")

        time.sleep(0.001)

def make_test_bulk(n: int) -> bytes:
    return bytes((i & 0xFF) for i in range(n))

def send_key_event(keycode: int, value: int) -> None:
    send_frame(MSG_KEY_EVENT, [int(keycode), int(value)])

def announce_bulk_available(bulk: bytes, tx_id: int) -> None:
    send_frame(MSG_BULK_AVAILABLE, [int(tx_id), int(len(bulk)), crc32_bytes(bulk)])

def request_host_bulk_tx(n: int, tx_id: int) -> bytes:
    data = make_test_bulk(n)
    crc_b = crc32_bytes(data)
    send_frame(MSG_BULK_REQUEST_TX, [int(tx_id), int(n), crc_b])
    return crc_b

# ---------------- Main loop ----------------

rxbuf = bytearray()

print("nRF: ready (UART framed + msgpack stream API, SPI bitbang slave)")
wait_startup_confirm()

# Send a test key event (KEY_ENTER down+up)
send_key_event(28, 1)
time.sleep(0.02)
send_key_event(28, 0)

# Serve a bulk buffer to host
bulk_tx_buf = make_test_bulk(256)
announce_bulk_available(bulk_tx_buf, bulk_id)

# Request host->MCU bulk transfer (we will receive on MOSI)
bulk_id += 1
expected_rx_crc_b = request_host_bulk_tx(128, bulk_id)

while True:
    chunk = uart.read(256)
    if chunk:
        rxbuf.extend(chunk)

    parsed = try_read_frame(rxbuf)
    if parsed:
        version, msg_type, payload = parsed
        if version != PROTO_VERSION:
            continue

        try:
            obj = msgpack_unpack_bytes(payload)
        except Exception as e:
            print("msgpack unpack error:", e)
            continue

        if msg_type == MSG_BULK_START:
            tx_id = int(obj[0])

            # If tx_id corresponds to served bulk (previous BULK_AVAILABLE)
            if tx_id == bulk_id - 1:
                print("SPI: serving bulk id", tx_id, "len", len(bulk_tx_buf))
                _ = spi_exchange_bytes(bulk_tx_buf, rx_len=0)

            # If tx_id corresponds to host-tx request we made
            elif tx_id == bulk_id:
                print("SPI: receiving bulk from host id", tx_id)
                bulk_rx_buf = spi_exchange_bytes(b"\x00", rx_len=128)
                obs = crc32_ieee(bulk_rx_buf)
                obs_b = u32le(obs)
                print("RX bulk crc obs=%s exp=%s" % (obs_b, expected_rx_crc_b))

            else:
                print("BULK_START for unknown id", tx_id)

        elif msg_type == MSG_BULK_DONE:
            tx_id = int(obj[0])
            status = int(obj[1])
            crc_obs = int(obj[2])
            print("BULK_DONE id=%d status=%d crc=0x%08X" % (tx_id, status, crc_obs))

        else:
            print("RX msg_type=0x%02X obj=%s" % (msg_type, obj))

    time.sleep(0.001)
