#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

#include <random>
#include <optional>
#include <string>
#include <vector>
#include <variant>

#include <msgpack.hpp>

#include "event.hpp"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) {
    g_stop = 1;
}

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
        if (n == 0) return false; // EOF
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop) {
                    return false;
                }
                continue;
            }
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

static uint32_t random_u32() {
    std::random_device rd;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (v << 8) ^ (uint32_t)(rd() & 0xFF);
    }
    return v ? v : 1u;
}

static bool find_magic(int fd) {
    uint8_t b = 0;

    for (;;) {
        ssize_t n = read(fd, &b, 1);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop) {
                    return false;
                }
                continue;
            }
            die_perror("read(uart)");
        }

        if (b != MAGIC0) {
            continue;
        }

        uint8_t b2 = 0;
        if (!read_full(fd, &b2, 1)) {
            return false;
        }
        if (b2 == MAGIC1) {
            return true;
        }
    }
}

struct RxFrame {
    uint8_t msg_type;
    std::vector<uint8_t> payload;
};

static std::optional<RxFrame> uart_read_frame(int fd) {
    if (!find_magic(fd)) return std::nullopt;

    uint8_t hdr[6]; // version, msg_type, payload_len(u32)
    if (!read_full(fd, hdr, sizeof(hdr))) return std::nullopt;

    const uint8_t version = hdr[0];
    const uint8_t msg_type = hdr[1];
    const uint32_t payload_len = read_u32_le(hdr + 2);

    std::vector<uint8_t> payload(payload_len);
    if (payload_len && !read_full(fd, payload.data(), payload.size())) return std::nullopt;

    uint8_t crc_bytes[4];
    if (!read_full(fd, crc_bytes, sizeof(crc_bytes))) return std::nullopt;

    // Verify CRC over magic+hdr+payload
    std::vector<uint8_t> crc_buf;
    crc_buf.reserve(2 + sizeof(hdr) + payload.size());
    crc_buf.push_back(MAGIC0);
    crc_buf.push_back(MAGIC1);
    crc_buf.insert(crc_buf.end(), hdr, hdr + sizeof(hdr));
    crc_buf.insert(crc_buf.end(), payload.begin(), payload.end());

    const uint32_t got_crc = read_u32_le(crc_bytes);
    const uint32_t calc_crc = crc32_ieee(crc_buf.data(), crc_buf.size());

    if (version != PROTO_VERSION) {
        fprintf(stderr, "WARN: version mismatch (got %u, want %u)\n",
                (unsigned)version, (unsigned)PROTO_VERSION);
        return std::nullopt;
    }
    if (got_crc != calc_crc) {
        fprintf(stderr, "WARN: CRC mismatch (calc=0x%08x got=0x%08x)\n", calc_crc, got_crc);
        return std::nullopt;
    }

    return RxFrame{msg_type, std::move(payload)};
}

static std::optional<Event> uart_read_event(int fd) {
    auto fr = uart_read_frame(fd);
    if (!fr.has_value()) return std::nullopt;

    if (fr->msg_type != MSG_KEY_EVENT) {
        return std::nullopt;
    }

    msgpack::object_handle oh =
        msgpack::unpack((const char*)fr->payload.data(), fr->payload.size());
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

static void uart_send_frame(int uart_fd, uint8_t msg_type, const msgpack::sbuffer& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(2 + 1 + 1 + 4 + payload.size() + 4);

    frame.push_back(MAGIC0);
    frame.push_back(MAGIC1);
    frame.push_back(PROTO_VERSION);
    frame.push_back(msg_type);

    append_u32_le(frame, static_cast<uint32_t>(payload.size()));

    frame.insert(frame.end(),
                 (const uint8_t*)payload.data(),
                 (const uint8_t*)payload.data() + payload.size());

    uint32_t crc = crc32_ieee(frame.data(), frame.size());
    append_u32_le(frame, crc);

    write_all(uart_fd, frame.data(), frame.size());
}

static void uart_send_startup(int uart_fd, uint32_t session_id, uint32_t flags) {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);

    pk.pack_map(2);
    pk.pack_int(1); pk.pack_uint32(session_id);
    pk.pack_int(2); pk.pack_uint32(flags);

    uart_send_frame(uart_fd, MSG_STARTUP, sbuf);
}

static void uart_send_shutdown(int uart_fd, uint32_t session_id, const char* reason) {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);

    pk.pack_map(2);
    pk.pack_int(1); pk.pack_uint32(session_id);
    pk.pack_int(2);
    pk.pack_str((uint32_t)strlen(reason));
    pk.pack_str_body(reason, (uint32_t)strlen(reason));

    uart_send_frame(uart_fd, MSG_SHUTDOWN, sbuf);
}

static bool parse_startup_ack(const std::vector<uint8_t>& payload, uint32_t expected_session) {
    msgpack::object_handle oh;
    try {
        oh = msgpack::unpack((const char*)payload.data(), payload.size());
    } catch (...) {
        return false;
    }

    msgpack::object obj = oh.get();
    if (obj.type != msgpack::type::MAP)
        return false;

    for (uint32_t i = 0; i < obj.via.map.size; i++) {
        const auto& kv = obj.via.map.ptr[i];

        int64_t key = 0;
        if (kv.key.type == msgpack::type::POSITIVE_INTEGER)
            key = kv.key.via.u64;
        else
            continue;

        if (key == 1 &&
            kv.val.type == msgpack::type::POSITIVE_INTEGER &&
            kv.val.via.u64 == expected_session) {
            return true;
        }
    }
    return false;
}

static bool uart_startup_handshake(int uart_fd, uint32_t session_id) {
    const uint32_t flags = 0;
    const int resend_ms = 250;

    fprintf(stderr, "INFO: UART startup handshake, session_id=%u\n",
        session_id);

    uart_send_startup(uart_fd, session_id, flags);

    while (!g_stop) {
        struct pollfd pfd{};
        pfd.fd = uart_fd;
        pfd.events = POLLIN;

        int pr = poll(&pfd, 1, resend_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll(uart)");
            return false;
        }

        if (pr == 0) {
            // Timeout
            uart_send_startup(uart_fd, session_id, flags);
            continue;
        }

        auto fr = uart_read_frame(uart_fd);
        if (!fr.has_value()) {
            continue;
        }

        if (fr->msg_type == MSG_STARTUP_ACK) {
            if (parse_startup_ack(fr->payload, session_id)) {
                fprintf(stderr, "INFO: startup ACK received\n");
                return true;
            }
        }

        // Ignore other events
    }

    return false;
}

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

int main(int argc, char** argv) {
    const char* uart_path = nullptr;
    const char* uinput_path = nullptr;
    bool test = false;

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

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

    int uart_fd = open(uart_path, O_RDWR | O_NOCTTY);
    if (uart_fd < 0) die_perror("open(uart)");

    int uinput_fd = open(uinput_path, O_WRONLY | O_NOCTTY);
    if (uinput_fd < 0) die_perror("open(uinput)");

    UInput ui{uinput_fd, test};

    // Startup handshake
    uint32_t session_id = random_u32();
    if (!uart_startup_handshake(uart_fd, session_id)) {
        uart_send_shutdown(uart_fd, session_id, "startup-failed");
        return 1;
    }

    if (!test) {
        setup_uinput_real(uinput_fd);
    }

    while (!g_stop) {
        auto ev = uart_read_event(uart_fd);
        if (!ev.has_value()) {
            break;
        }

        std::visit([&](auto&& e) {
            emit_event(ui, EV_KEY, e.c, e.is_down ? 1 : 0);
            emit_syn(ui);
        }, *ev);
    }

    uart_send_shutdown(uart_fd, session_id, "daemon-exit");

    return 0;
}
