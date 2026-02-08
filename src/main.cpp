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

// Global state
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) {
    g_stop = 1;
}

[[noreturn]] static void die_perror(const char* what) {
    perror(what);
    exit(1);
}

static void write_all(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            die_perror("write");
        }
        off += static_cast<size_t>(n);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// COBS + CRC8 Frame Receiver
// ─────────────────────────────────────────────────────────────────────────────

class FrameReceiver {
public:
    explicit FrameReceiver(int fd) : uart_fd_(fd) {}

    // Returns true if a valid frame was received into buffer.
    // frame_len is set to decoded payload length (excluding CRC).
    bool receive(uint8_t* buffer, size_t buffer_size, size_t& frame_len) {
        while (!g_stop) {
            uint8_t byte;
            ssize_t n = read(uart_fd_, &byte, 1);

            if (n == 0) return false;  // EOF
            if (n < 0) {
                if (errno == EINTR) {
                    if (g_stop) return false;
                    continue;
                }
                die_perror("read(uart)");
            }

            if (byte == FRAME_DELIMITER) {
                if (frame_in_progress_) {
                    // Frame complete - finalize decoding
                    size_t len = cobs_streaming_decode_finish(&cobs_ctx_);
                    if (len != SIZE_MAX && len >= 2) {
                        // Verify CRC (last byte)
                        uint8_t rx_crc = rx_buffer_[len - 1];
                        uint8_t calc_crc = crc8_calculate_bytes(rx_buffer_, len - 1, false);
                        if (rx_crc == calc_crc) {
                            size_t payload_len = len - 1;
                            if (payload_len <= buffer_size) {
                                memcpy(buffer, rx_buffer_, payload_len);
                                frame_len = payload_len;
                                frame_in_progress_ = false;
                                return true;
                            }
                        }
                    }
                }
                // Start new frame
                cobs_streaming_decode_start(&cobs_ctx_, rx_buffer_, sizeof(rx_buffer_));
                frame_in_progress_ = true;
            } else if (frame_in_progress_) {
                if (!cobs_streaming_decode(&cobs_ctx_, static_cast<char>(byte))) {
                    frame_in_progress_ = false;  // Decode error
                }
            }
        }
        return false;
    }

private:
    int uart_fd_;
    uint8_t rx_buffer_[RX_BUFFER_SIZE]{};
    CobsDecodeContext cobs_ctx_{};
    bool frame_in_progress_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Frame Transmitter
// ─────────────────────────────────────────────────────────────────────────────

class FrameTransmitter {
public:
    explicit FrameTransmitter(int fd) : uart_fd_(fd) {}

    void send(const k230_K230ToNrf& msg) {
        // Encode protobuf
        uint8_t pb_buffer[k230_K230ToNrf_size];
        pb_ostream_t stream = pb_ostream_from_buffer(pb_buffer, sizeof(pb_buffer));
        if (!pb_encode(&stream, k230_K230ToNrf_fields, &msg)) {
            fprintf(stderr, "WARN: pb_encode failed\n");
            return;
        }
        size_t pb_len = stream.bytes_written;

        // Append CRC8
        uint8_t with_crc[k230_K230ToNrf_size + 1];
        memcpy(with_crc, pb_buffer, pb_len);
        with_crc[pb_len] = crc8_calculate_bytes(pb_buffer, pb_len, false);

        // COBS encode
        uint8_t cobs_buffer[MAX_SIZE_AFTER_COBS_ENCODING(k230_K230ToNrf_size + 1)];
        size_t cobs_len = cobs_encode(cobs_buffer, with_crc, pb_len + 1);

        // Send delimiter + frame
        uint8_t delimiter = FRAME_DELIMITER;
        write_all(uart_fd_, &delimiter, 1);
        write_all(uart_fd_, cobs_buffer, cobs_len);
    }

    void send_ack(uint32_t sequence) {
        k230_K230ToNrf msg = k230_K230ToNrf_init_zero;
        msg.which_msg = k230_K230ToNrf_ack_tag;
        msg.msg.ack.sequence = sequence;
        send(msg);
    }

    void send_shutdown() {
        k230_K230ToNrf msg = k230_K230ToNrf_init_zero;
        msg.which_msg = k230_K230ToNrf_shutdown_tag;
        send(msg);
    }

private:
    int uart_fd_;
};

// ─────────────────────────────────────────────────────────────────────────────
// UInput Virtual Device
// ─────────────────────────────────────────────────────────────────────────────

class UInputDevice {
public:
    explicit UInputDevice(int fd, bool test_mode = false)
        : fd_(fd), test_mode_(test_mode) {}

    void setup() {
        if (test_mode_) return;

        if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0) die_perror("UI_SET_EVBIT EV_KEY");
        if (ioctl(fd_, UI_SET_EVBIT, EV_REL) < 0) die_perror("UI_SET_EVBIT EV_REL");

        // Enable all keys
        for (int k = 0; k < KEY_MAX; ++k) {
            ioctl(fd_, UI_SET_KEYBIT, k);
        }

        // Enable mouse axes
        if (ioctl(fd_, UI_SET_RELBIT, REL_X) < 0) die_perror("UI_SET_RELBIT REL_X");
        if (ioctl(fd_, UI_SET_RELBIT, REL_Y) < 0) die_perror("UI_SET_RELBIT REL_Y");

        struct uinput_setup setup{};
        snprintf(setup.name, sizeof(setup.name), "K230 Virtual Input");
        setup.id.bustype = BUS_USB;
        setup.id.vendor = 0x1234;
        setup.id.product = 0x5678;
        setup.id.version = 1;

        if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0) die_perror("UI_DEV_SETUP");
        if (ioctl(fd_, UI_DEV_CREATE) < 0) die_perror("UI_DEV_CREATE");
    }

    void inject_key(uint32_t keycode, bool is_down) {
        emit(EV_KEY, static_cast<uint16_t>(keycode), is_down ? 1 : 0);
        emit_syn();
    }

    void inject_mouse(int32_t dx, int32_t dy) {
        if (dx != 0) emit(EV_REL, REL_X, dx);
        if (dy != 0) emit(EV_REL, REL_Y, dy);
        if (dx != 0 || dy != 0) emit_syn();
    }

private:
    void emit(uint16_t type, uint16_t code, int32_t value) {
        struct input_event ev{};
        ev.type = type;
        ev.code = code;
        ev.value = value;
        write_all(fd_, &ev, sizeof(ev));
    }

    void emit_syn() {
        emit(EV_SYN, SYN_REPORT, 0);
    }

    int fd_;
    bool test_mode_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Protocol Handler
// ─────────────────────────────────────────────────────────────────────────────

class ProtocolHandler {
public:
    ProtocolHandler(FrameReceiver& rx, FrameTransmitter& tx, UInputDevice& uinput)
        : receiver_(rx), transmitter_(tx), uinput_(uinput) {}

    void run() {
        fprintf(stderr, "INFO: Waiting for heartbeat...\n");

        while (!g_stop) {
            uint8_t buffer[k230_NrfToK230_size];
            size_t frame_len = 0;

            if (!receiver_.receive(buffer, sizeof(buffer), frame_len)) {
                break;
            }

            // Decode nanopb message
            k230_NrfToK230 msg = k230_NrfToK230_init_zero;
            pb_istream_t stream = pb_istream_from_buffer(buffer, frame_len);
            if (!pb_decode(&stream, k230_NrfToK230_fields, &msg)) {
                fprintf(stderr, "WARN: pb_decode failed\n");
                continue;
            }

            handle_message(msg);
        }
    }

private:
    void handle_message(const k230_NrfToK230& msg) {
        switch (msg.which_msg) {
            case k230_NrfToK230_heartbeat_tag:
                if (!connected_) {
                    fprintf(stderr, "INFO: Heartbeat received, sending ACK\n");
                    connected_ = true;
                }
                transmitter_.send_ack(0);
                break;

            case k230_NrfToK230_keyboard_tag:
                if (connected_) {
                    uinput_.inject_key(msg.msg.keyboard.keycode, msg.msg.keyboard.is_down);
                }
                break;

            case k230_NrfToK230_touchpad_tag:
                if (connected_) {
                    uinput_.inject_mouse(msg.msg.touchpad.delta_x, msg.msg.touchpad.delta_y);
                }
                break;

            default:
                fprintf(stderr, "WARN: Unknown message type %u\n", msg.which_msg);
                break;
        }
    }

    FrameReceiver& receiver_;
    FrameTransmitter& transmitter_;
    UInputDevice& uinput_;
    bool connected_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const char* uart_path = nullptr;
    const char* uinput_path = nullptr;
    bool test_mode = false;

    // Signal handling
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--uart=", 7) == 0) {
            uart_path = argv[i] + 7;
        } else if (strncmp(argv[i], "--uinput=", 9) == 0) {
            uinput_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--test") == 0) {
            test_mode = true;
        }
    }

    if (!uart_path || !uinput_path) {
        fprintf(stderr, "Usage: %s --uart=<path> --uinput=<path> [--test]\n", argv[0]);
        return 2;
    }

    // Open devices
    int uart_fd = open(uart_path, O_RDWR | O_NOCTTY);
    if (uart_fd < 0) die_perror("open(uart)");

    int uinput_fd = open(uinput_path, O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) die_perror("open(uinput)");

    // Initialize components
    FrameReceiver receiver(uart_fd);
    FrameTransmitter transmitter(uart_fd);
    UInputDevice uinput(uinput_fd, test_mode);

    if (!test_mode) {
        uinput.setup();
    }

    // Run protocol
    ProtocolHandler handler(receiver, transmitter, uinput);
    handler.run();

    // Cleanup
    transmitter.send_shutdown();
    fprintf(stderr, "INFO: Shutdown sent, exiting\n");

    close(uart_fd);
    close(uinput_fd);
    return 0;
}
