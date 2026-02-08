#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <string>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>

// C++ compatibility for C headers using 'restrict'
#define restrict __restrict__

extern "C" {
#include "nanopb/pb_encode.h"
#include "nanopb/pb_decode.h"
#include "pebble/console/cobs.h"
#include "pebble/util/crc8.h"
#include "pebble/k230_protocol.pb.h"
}

// Protocol constants
static constexpr size_t RX_BUFFER_SIZE = 128;
static constexpr uint8_t FRAME_DELIMITER = 0x00;

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int) { g_stop = 1; }

[[noreturn]] static void die_perror(const char* what) {
    perror(what);
    std::exit(1);
}

static void write_all(int fd, const uint8_t* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            die_perror("write");
        }
        off += static_cast<size_t>(n);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame Transmitter (nRF → K230)
// ─────────────────────────────────────────────────────────────────────────────

class FrameTransmitter {
public:
    explicit FrameTransmitter(int fd) : fd_(fd) {}

    void send(const k230_NrfToK230& msg) {
        // Encode protobuf
        uint8_t pb_buffer[k230_NrfToK230_size];
        pb_ostream_t stream = pb_ostream_from_buffer(pb_buffer, sizeof(pb_buffer));
        if (!pb_encode(&stream, k230_NrfToK230_fields, &msg)) {
            std::fprintf(stderr, "WARN: pb_encode failed\n");
            return;
        }
        size_t pb_len = stream.bytes_written;

        // Append CRC8
        uint8_t with_crc[k230_NrfToK230_size + 1];
        std::memcpy(with_crc, pb_buffer, pb_len);
        with_crc[pb_len] = crc8_calculate_bytes(pb_buffer, pb_len, false);

        // COBS encode
        uint8_t cobs_buffer[MAX_SIZE_AFTER_COBS_ENCODING(k230_NrfToK230_size + 1)];
        size_t cobs_len = cobs_encode(cobs_buffer, with_crc, pb_len + 1);

        // Send delimiter + frame
        uint8_t delimiter = FRAME_DELIMITER;
        write_all(fd_, &delimiter, 1);
        write_all(fd_, cobs_buffer, cobs_len);
    }

    void send_heartbeat() {
        k230_NrfToK230 msg = k230_NrfToK230_init_zero;
        msg.which_msg = k230_NrfToK230_heartbeat_tag;
        send(msg);
    }

    void send_keyboard(uint32_t keycode, bool is_down) {
        k230_NrfToK230 msg = k230_NrfToK230_init_zero;
        msg.which_msg = k230_NrfToK230_keyboard_tag;
        msg.msg.keyboard.keycode = keycode;
        msg.msg.keyboard.is_down = is_down;
        send(msg);
    }

    void send_touchpad(int32_t dx, int32_t dy, uint32_t surface_qual = 100) {
        k230_NrfToK230 msg = k230_NrfToK230_init_zero;
        msg.which_msg = k230_NrfToK230_touchpad_tag;
        msg.msg.touchpad.delta_x = dx;
        msg.msg.touchpad.delta_y = dy;
        msg.msg.touchpad.surface_qual = surface_qual;
        send(msg);
    }

private:
    int fd_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Frame Receiver (K230 → nRF)
// ─────────────────────────────────────────────────────────────────────────────

class FrameReceiver {
public:
    explicit FrameReceiver(int fd) : fd_(fd) {}

    // Non-blocking receive attempt
    bool try_receive(k230_K230ToNrf& out_msg) {
        uint8_t byte;
        while (true) {
            ssize_t n = read(fd_, &byte, 1);
            if (n == 0) return false;
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    return false;
                }
                return false;
            }

            if (byte == FRAME_DELIMITER) {
                if (frame_in_progress_) {
                    size_t len = cobs_streaming_decode_finish(&cobs_ctx_);
                    if (len != SIZE_MAX && len >= 2) {
                        uint8_t rx_crc = rx_buffer_[len - 1];
                        uint8_t calc_crc = crc8_calculate_bytes(rx_buffer_, len - 1, false);
                        if (rx_crc == calc_crc) {
                            pb_istream_t stream = pb_istream_from_buffer(rx_buffer_, len - 1);
                            out_msg = k230_K230ToNrf_init_zero;
                            if (pb_decode(&stream, k230_K230ToNrf_fields, &out_msg)) {
                                frame_in_progress_ = false;
                                return true;
                            }
                        }
                    }
                }
                cobs_streaming_decode_start(&cobs_ctx_, rx_buffer_, sizeof(rx_buffer_));
                frame_in_progress_ = true;
            } else if (frame_in_progress_) {
                if (!cobs_streaming_decode(&cobs_ctx_, static_cast<char>(byte))) {
                    frame_in_progress_ = false;
                }
            }
        }
    }

private:
    int fd_;
    uint8_t rx_buffer_[RX_BUFFER_SIZE]{};
    CobsDecodeContext cobs_ctx_{};
    bool frame_in_progress_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// PTY Helpers
// ─────────────────────────────────────────────────────────────────────────────

struct Pty {
    int master_fd = -1;
    int slave_fd = -1;
    std::string slave_path;
};

static void set_raw_mode(int fd) {
    termios tio{};
    if (tcgetattr(fd, &tio) != 0) die_perror("tcgetattr");
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) die_perror("tcsetattr");
}

static Pty create_pty() {
    Pty p;
    p.master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (p.master_fd < 0) die_perror("posix_openpt");
    if (grantpt(p.master_fd) != 0) die_perror("grantpt");
    if (unlockpt(p.master_fd) != 0) die_perror("unlockpt");

    char* name = ptsname(p.master_fd);
    if (!name) die_perror("ptsname");
    p.slave_path = name;

    p.slave_fd = open(p.slave_path.c_str(), O_RDWR | O_NOCTTY);
    if (p.slave_fd < 0) die_perror("open slave");

    set_raw_mode(p.master_fd);
    set_raw_mode(p.slave_fd);
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stdin Non-Canonical Mode RAII
// ─────────────────────────────────────────────────────────────────────────────

class StdinNonCanonGuard {
public:
    StdinNonCanonGuard() {
        if (!isatty(STDIN_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) return;

        termios t = saved_;
        t.c_lflag &= static_cast<tcflag_t>(~ICANON);
        t.c_lflag |= ISIG;
        t.c_lflag |= ECHO;
        t.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF));
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
            active_ = true;
        }

        // Fix newline rendering
        if (isatty(STDOUT_FILENO)) {
            termios o{};
            if (tcgetattr(STDOUT_FILENO, &o) == 0) {
                o.c_oflag |= OPOST | ONLCR;
                tcsetattr(STDOUT_FILENO, TCSANOW, &o);
            }
        }
    }

    ~StdinNonCanonGuard() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
        }
    }

private:
    bool active_ = false;
    termios saved_{};
};

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // Signal handling
    struct sigaction sa{};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    Pty pty = create_pty();
    StdinNonCanonGuard stdin_guard;

    // Print slave PTY path for daemon to connect
    std::printf("%s\n", pty.slave_path.c_str());
    std::fflush(stdout);

    FrameTransmitter tx(pty.master_fd);
    FrameReceiver rx(pty.master_fd);

    bool connected = false;
    int heartbeat_counter = 0;
    constexpr int HEARTBEAT_INTERVAL_MS = 3000;
    constexpr int POLL_TIMEOUT_MS = 100;

    std::fprintf(stderr, "INFO: Fake UART started. Type to send key events.\n");
    std::fprintf(stderr, "INFO: Arrow keys send touchpad events. Ctrl-D to exit.\n");

    while (!g_stop) {
        pollfd pfds[2]{};
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[1].fd = pty.master_fd;
        pfds[1].events = POLLIN;

        int pr = poll(pfds, 2, POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            die_perror("poll");
        }

        // Send periodic heartbeat
        heartbeat_counter += POLL_TIMEOUT_MS;
        if (!connected && heartbeat_counter >= HEARTBEAT_INTERVAL_MS) {
            std::fprintf(stderr, "INFO: Sending heartbeat...\n");
            tx.send_heartbeat();
            heartbeat_counter = 0;
        }

        // Handle incoming frames from daemon
        if (pfds[1].revents & POLLIN) {
            k230_K230ToNrf msg;
            while (rx.try_receive(msg)) {
                if (msg.which_msg == k230_K230ToNrf_ack_tag) {
                    if (!connected) {
                        std::fprintf(stderr, "INFO: ACK received, connected!\n");
                        connected = true;
                    }
                } else if (msg.which_msg == k230_K230ToNrf_shutdown_tag) {
                    std::fprintf(stderr, "INFO: Shutdown received, exiting.\n");
                    return 0;
                }
            }
        }

        // Handle stdin input
        if (pfds[0].revents & POLLIN) {
            uint8_t buf[8];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n == 0) break;  // EOF
            if (n < 0) {
                if (errno == EINTR) continue;
                die_perror("read(stdin)");
            }

            // Check for Ctrl-D
            if (isatty(STDIN_FILENO) && n == 1 && buf[0] == 0x04) {
                break;
            }

            // Check for escape sequences (arrow keys)
            if (n >= 3 && buf[0] == 0x1B && buf[1] == '[') {
                int32_t dx = 0, dy = 0;
                switch (buf[2]) {
                    case 'A': dy = -10; break;  // Up
                    case 'B': dy = 10; break;   // Down
                    case 'C': dx = 10; break;   // Right
                    case 'D': dx = -10; break;  // Left
                }
                if (dx != 0 || dy != 0) {
                    std::fprintf(stderr, "TX: touchpad(%d, %d)\n", dx, dy);
                    tx.send_touchpad(dx, dy);
                    continue;
                }
            }

            // Send as key events
            for (ssize_t i = 0; i < n; ++i) {
                uint32_t keycode = buf[i];
                std::fprintf(stderr, "TX: key(%u down+up)\n", keycode);
                tx.send_keyboard(keycode, true);
                tx.send_keyboard(keycode, false);
            }
        }

        // Check for PTY hangup
        if (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            std::fprintf(stderr, "INFO: PTY hangup, exiting.\n");
            break;
        }
    }

    return 0;
}
