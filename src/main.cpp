// main.cpp
// UART-framed MessagePack consumer -> uinput key injection
//
// Options:
//   --uart=<path>    UART device path (e.g., /dev/pts/N)
//   --uinput=<path>  uinput path (e.g., /dev/uinput or FIFO in test mode)
//   --test           test mode: skip uinput ioctls; write struct input_event only
//
// Framing (little-endian):
//   magic[2] | version | msg_type | payload_len | payload (msgpack) | crc32
//
// MSG_KEY_EVENT payload (msgpack map):
//   2 -> bin(RawKeyboardEvent)

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include <variant>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

#include <msgpack.hpp>

#include "event.hpp"

// ------------------------------------------------------------
// Utilities
// ------------------------------------------------------------
static void die_perror(const char* what) {
    perror(what);
    exit(1);
}

static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static bool read_full(int fd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            die_perror("read");
        }
        off += (size_t)n;
    }
    return true;
}

static void write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            die_perror("write");
        }
        off += (size_t)n;
    }
}

// ------------------------------------------------------------
// UART reader
// ------------------------------------------------------------
static bool find_magic(int fd) {
    uint8_t b;
    for (;;) {
        ssize_t n = read(fd, &b, 1);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            die_perror("read(uart)");
        }
        if (b != MAGIC0) continue;

        uint8_t b2;
        if (!read_full(fd, &b2, 1)) return false;
        if (b2 == MAGIC1) return true;
    }
}

static std::optional<Event> read_event_from_uart(int fd) {
    if (!find_magic(fd))
        return std::nullopt;

    uint8_t hdr[6]; // version, msg_type, payload_len(u32)
    if (!read_full(fd, hdr, sizeof(hdr)))
        return std::nullopt;

    uint8_t version   = hdr[0];
    uint8_t msg_type  = hdr[1];
    uint32_t plen     = read_u32_le(hdr + 2);

    std::vector<uint8_t> payload(plen);
    if (plen && !read_full(fd, payload.data(), plen))
        return std::nullopt;

    uint8_t crc_bytes[4];
    if (!read_full(fd, crc_bytes, 4))
        return std::nullopt;

    // Verify CRC
    std::vector<uint8_t> crc_buf;
    crc_buf.reserve(2 + sizeof(hdr) + payload.size());
    crc_buf.push_back(MAGIC0);
    crc_buf.push_back(MAGIC1);
    crc_buf.insert(crc_buf.end(), hdr, hdr + sizeof(hdr));
    crc_buf.insert(crc_buf.end(), payload.begin(), payload.end());

    uint32_t got_crc = read_u32_le(crc_bytes);
    uint32_t calc_crc = crc32_ieee(crc_buf.data(), crc_buf.size());

    if (got_crc != calc_crc || version != PROTO_VERSION)
        return std::nullopt;

    if (msg_type != MSG_KEY_EVENT)
        return std::nullopt;

    msgpack::object_handle oh =
        msgpack::unpack((const char*)payload.data(), payload.size());
    msgpack::object obj = oh.get();

    if (obj.type != msgpack::type::MAP)
        return std::nullopt;

    for (uint32_t i = 0; i < obj.via.map.size; ++i) {
        const auto& kv = obj.via.map.ptr[i];
        if (kv.key.as<int>() != 2)
            continue;

        if (kv.val.type != msgpack::type::BIN)
            return std::nullopt;

        if (kv.val.via.bin.size != sizeof(RawKeyboardEvent))
            return std::nullopt;

        RawKeyboardEvent ev{};
        memcpy(&ev, kv.val.via.bin.ptr, sizeof(ev));
        return Event{ev};
    }

    return std::nullopt;
}

// ------------------------------------------------------------
// uinput
// ------------------------------------------------------------
struct UInput {
    int fd;
    bool test;
};

static void emit_event(UInput& ui, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    write_all(ui.fd, &ev, sizeof(ev));
}

static void emit_syn(UInput& ui) {
    emit_event(ui, EV_SYN, SYN_REPORT, 0);
}

static void setup_uinput_real(int fd) {
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) die_perror("UI_SET_EVBIT");

    for (int k = 0; k < 256; ++k)
        ioctl(fd, UI_SET_KEYBIT, k);

    struct uinput_user_dev uidev{};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "uart-test-keyboard");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    write_all(fd, &uidev, sizeof(uidev));

    if (ioctl(fd, UI_DEV_CREATE) < 0)
        die_perror("UI_DEV_CREATE");
}

// ------------------------------------------------------------
// main
// ------------------------------------------------------------
int main(int argc, char** argv) {
    const char* uart_path = nullptr;
    const char* uinput_path = nullptr;
    bool test = false;

    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "--uart=", 7))
            uart_path = argv[i] + 7;
        else if (!strncmp(argv[i], "--uinput=", 9))
            uinput_path = argv[i] + 9;
        else if (!strcmp(argv[i], "--test"))
            test = true;
    }

    if (!uart_path || !uinput_path) {
        fprintf(stderr, "Usage: %s --uart=<path> --uinput=<path> [--test]\n", argv[0]);
        return 2;
    }

    int uart_fd = open(uart_path, O_RDONLY | O_NOCTTY);
    if (uart_fd < 0) die_perror("open(uart)");

    int uinput_fd = open(uinput_path, O_WRONLY | O_NOCTTY);
    if (uinput_fd < 0) die_perror("open(uinput)");

    UInput ui{uinput_fd, test};

    if (!test)
        setup_uinput_real(uinput_fd);

    for (;;) {
        auto ev = read_event_from_uart(uart_fd);
        if (!ev.has_value())
            break;

        std::visit([&](auto&& e) {
            emit_event(ui, EV_KEY, e.c, e.is_down ? 1 : 0);
            emit_syn(ui);
        }, *ev);
    }

    return 0;
}
