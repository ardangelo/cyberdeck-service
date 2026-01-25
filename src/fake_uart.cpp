#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>

#include <msgpack.hpp>

#include "event.hpp"

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int) { g_stop = 1; }

static void write_all(int fd, const uint8_t* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("write");
            exit(1);
        }
        off += static_cast<size_t>(n);
    }
}

static bool read_full(int fd, void* buf, size_t len) {
    uint8_t* p = reinterpret_cast<uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);
        if (n == 0) return false; // EOF
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static std::vector<uint8_t> pack_frame(uint8_t msg_type, const msgpack::sbuffer& sbuf) {
    uint32_t payload_len = static_cast<uint32_t>(sbuf.size());

    std::vector<uint8_t> frame;
    frame.reserve(2 + 1 + 1 + 4 + payload_len + 4);

    frame.push_back(MAGIC0);
    frame.push_back(MAGIC1);
    frame.push_back(PROTO_VERSION);
    frame.push_back(msg_type);

    append_u32_le(frame, payload_len);
    frame.insert(frame.end(),
                 reinterpret_cast<const uint8_t*>(sbuf.data()),
                 reinterpret_cast<const uint8_t*>(sbuf.data()) + sbuf.size());

    uint32_t crc = crc32_ieee(frame.data(), frame.size());
    append_u32_le(frame, crc);

    return frame;
}

static std::vector<uint8_t> pack_raw_key_event_frame(const RawKeyboardEvent& ev) {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);

    pk.pack_map(1);
    pk.pack_int(2);
    pk.pack_bin(sizeof(RawKeyboardEvent));
    pk.pack_bin_body(reinterpret_cast<const char*>(&ev), sizeof(RawKeyboardEvent));

    return pack_frame(MSG_KEY_EVENT, sbuf);
}

static std::vector<uint8_t> pack_startup_ack_frame(uint32_t session_id) {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);

    // ACK payload: { 1: session_id }
    pk.pack_map(1);
    pk.pack_int(1);
    pk.pack_uint32(session_id);

    return pack_frame(MSG_STARTUP_ACK, sbuf);
}

static void send_raw_key(int out_fd, uint8_t c, bool is_down) {
    RawKeyboardEvent ev{};
    ev.c = c;
    ev.is_down = is_down;

    auto frame = pack_raw_key_event_frame(ev);
    write_all(out_fd, frame.data(), frame.size());
}

static void send_startup_ack(int out_fd, uint32_t session_id) {
    auto frame = pack_startup_ack_frame(session_id);
    write_all(out_fd, frame.data(), frame.size());
}

struct RxFrame {
    uint8_t msg_type{};
    std::vector<uint8_t> payload; // msgpack bytes
};

static bool find_magic(int fd) {
    uint8_t b = 0;
    for (;;) {
        ssize_t n = read(fd, &b, 1);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (b != MAGIC0) continue;

        uint8_t b2 = 0;
        if (!read_full(fd, &b2, 1)) return false;
        if (b2 == MAGIC1) return true;
        // otherwise keep scanning
    }
}

static bool read_frame_from_uart(int fd, RxFrame& out) {
    if (!find_magic(fd)) return false;

    uint8_t hdr[6]; // version, msg_type, payload_len(u32)
    if (!read_full(fd, hdr, sizeof(hdr))) return false;

    const uint8_t version = hdr[0];
    const uint8_t msg_type = hdr[1];
    const uint32_t payload_len = read_u32_le(hdr + 2);

    std::vector<uint8_t> payload(payload_len);
    if (payload_len && !read_full(fd, payload.data(), payload.size())) return false;

    uint8_t crc_bytes[4];
    if (!read_full(fd, crc_bytes, sizeof(crc_bytes))) return false;

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
        return false;
    }
    if (got_crc != calc_crc) {
        fprintf(stderr, "WARN: CRC mismatch (calc=0x%08x got=0x%08x)\n", calc_crc, got_crc);
        return false;
    }

    out.msg_type = msg_type;
    out.payload = std::move(payload);
    return true;
}

static bool parse_session_id_key1(const std::vector<uint8_t>& payload, uint32_t& session_id_out) {
    msgpack::object_handle oh;
    try {
        oh = msgpack::unpack(reinterpret_cast<const char*>(payload.data()), payload.size());
    } catch (...) {
        return false;
    }

    msgpack::object obj = oh.get();
    if (obj.type != msgpack::type::MAP) return false;

    for (uint32_t i = 0; i < obj.via.map.size; i++) {
        const auto& kv = obj.via.map.ptr[i];

        int64_t k = 0;
        if (kv.key.type == msgpack::type::POSITIVE_INTEGER) k = (int64_t)kv.key.via.u64;
        else if (kv.key.type == msgpack::type::NEGATIVE_INTEGER) k = (int64_t)kv.key.via.i64;
        else continue;

        if (k == 1 && kv.val.type == msgpack::type::POSITIVE_INTEGER) {
            session_id_out = (uint32_t)kv.val.via.u64;
            return true;
        }
    }
    return false;
}

struct Pty {
    int master_fd{-1};
    int slave_fd{-1};
    std::string slave_path;
};

static void set_raw_mode_fd(int fd) {
    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        perror("tcgetattr");
        exit(1);
    }
    cfmakeraw(&tio);

    // 8N1, no flow control
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        perror("tcsetattr");
        exit(1);
    }
}

static Pty create_pty() {
    Pty p{};

    p.master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (p.master_fd < 0) {
        perror("posix_openpt");
        exit(1);
    }
    if (grantpt(p.master_fd) != 0) {
        perror("grantpt");
        exit(1);
    }
    if (unlockpt(p.master_fd) != 0) {
        perror("unlockpt");
        exit(1);
    }

    char* name = ptsname(p.master_fd);
    if (!name) {
        perror("ptsname");
        exit(1);
    }
    p.slave_path = name;

    p.slave_fd = open(p.slave_path.c_str(), O_RDWR | O_NOCTTY);
    if (p.slave_fd < 0) {
        perror("open slave");
        exit(1);
    }

    set_raw_mode_fd(p.master_fd);
    set_raw_mode_fd(p.slave_fd);

    return p;
}

struct StdinNonCanonGuard {
    bool active{false};
    termios saved{};

    StdinNonCanonGuard() {
        if (!isatty(STDIN_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &saved) != 0) return;

        termios t = saved;

        // Disable canonical line mode.
        t.c_lflag &= (tcflag_t)~ICANON;

        // Keep Ctrl-C and echo.
        t.c_lflag |= ISIG;
        t.c_lflag |= ECHO;

        // Disable software flow control.
        t.c_iflag &= (tcflag_t)~(IXON | IXOFF);

        // Byte-at-a-time reads.
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) active = true;

        // Fix stdout newline rendering: \n -> \r\n
        if (isatty(STDOUT_FILENO)) {
            termios o{};
            if (tcgetattr(STDOUT_FILENO, &o) == 0) {
                o.c_oflag |= OPOST;
                o.c_oflag |= ONLCR;
                (void)tcsetattr(STDOUT_FILENO, TCSANOW, &o);
            }
        }
    }

    ~StdinNonCanonGuard() {
        if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
};

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // Handle Ctrl-C
    struct sigaction sa{};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        perror("sigaction(SIGINT)");
        return 1;
    }

    Pty p = create_pty();

    // Make interactive input byte-by-byte
    StdinNonCanonGuard stdin_guard;

    // Print the slave PTY path
    fprintf(stdout, "%s\r\n", p.slave_path.c_str());
    fflush(stdout);

    // Main loop: poll stdin and UART master for handshake frames.
    // - stdin bytes generate KEY_EVENT frames
    // - MSG_STARTUP triggers MSG_STARTUP_ACK
    // - MSG_SHUTDOWN triggers exit
    while (!g_stop) {
        struct pollfd pfds[2]{};
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[1].fd = p.master_fd;
        pfds[1].events = POLLIN;

        int pr = poll(pfds, 2, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }

        // Handle inbound frames from daemon (handshake/shutdown)
        if (pfds[1].revents & POLLIN) {
            RxFrame fr{};
            if (read_frame_from_uart(p.master_fd, fr)) {
                if (fr.msg_type == MSG_STARTUP) {
                    uint32_t sid = 0;
                    if (parse_session_id_key1(fr.payload, sid)) {
                        fprintf(stderr, "INFO: RX MSG_STARTUP sid=%u -> TX MSG_STARTUP_ACK\n", sid);
                        send_startup_ack(p.master_fd, sid);
                    } else {
                        fprintf(stderr, "WARN: RX MSG_STARTUP but could not parse session_id\n");
                    }
                } else if (fr.msg_type == MSG_SHUTDOWN) {
                    uint32_t sid = 0;
                    (void)parse_session_id_key1(fr.payload, sid);
                    fprintf(stderr, "INFO: RX MSG_SHUTDOWN sid=%u; exiting\n", sid);
                    break;
                } else {
                    // Ignore other message types in fake peer.
                }
            }
        }

        // Handle stdin -> key events
        if (pfds[0].revents & POLLIN) {
            uint8_t b = 0;
            ssize_t n = read(STDIN_FILENO, &b, 1);
            if (n == 0) {
                // stdin EOF (pipe/file)
                break;
            }
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("read(stdin)");
                return 1;
            }

            // In non-canonical mode, Ctrl-D is delivered as byte 0x04; treat as "done".
            if (isatty(STDIN_FILENO) && b == 0x04) {
                break;
            }

            send_raw_key(p.master_fd, b, true);
            send_raw_key(p.master_fd, b, false);
        }

        // If daemon hung up on PTY, poll may show HUP/ERR. Exit gracefully.
        if (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            fprintf(stderr, "INFO: UART master revents=0x%x; exiting\n", pfds[1].revents);
            break;
        }
    }

    return 0;
}
