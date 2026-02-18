/*
 * PPOUART Test Tool
 *
 * Tests the PPOUART (Pebble Protocol over UART) implementation by performing:
 * 1. Reset handshake
 * 2. Basic data exchange
 * 3. Ack verification
 *
 * Usage: ppouart_test <tty_device>
 * Example: ppouart_test /dev/ttyS2
 */

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <vector>
#include <functional>

// COBS encoding/decoding (simplified implementation)
// See: https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing

std::vector<uint8_t> cobs_encode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> output;
    output.reserve(input.size() + (input.size() / 254) + 1);

    size_t code_index = 0;
    size_t code = 1;

    output.push_back(0);  // Placeholder for code byte

    for (uint8_t byte : input) {
        if (byte == 0) {
            output[code_index] = code;
            code = 1;
            code_index = output.size();
            output.push_back(0);
        } else {
            output.push_back(byte);
            code++;
            if (code == 0xFF) {
                output[code_index] = code;
                code = 1;
                code_index = output.size();
                output.push_back(0);
            }
        }
    }

    output[code_index] = code;

    return output;
}

std::vector<uint8_t> cobs_decode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> output;
    output.reserve(input.size());

    size_t i = 0;
    while (i < input.size()) {
        uint8_t code = input[i++];

        for (size_t j = 1; j < code && i < input.size(); j++) {
            output.push_back(input[i++]);
        }

        if (code < 0xFF && i < input.size()) {
            output.push_back(0);
        }
    }

    return output;
}

// PPoG packet structure
enum PPoGPacketType {
    PPOG_DATA = 0,
    PPOG_ACK = 1,
    PPOG_RESET_REQUEST = 2,
    PPOG_RESET_COMPLETE = 3
};

struct __attribute__((packed)) PPoGPacket {
    union {
        uint8_t header;
        struct {
            uint8_t type : 3;
            uint8_t sn : 5;
        };
    };
    uint8_t payload[256];
};

class UARTDevice {
public:
    UARTDevice(const char* device_path) : fd_(-1) {
        fd_ = open(device_path, O_RDWR | O_NOCTTY | O_SYNC);
        if (fd_ < 0) {
            perror("Failed to open UART device");
            return;
        }

        // Configure UART (115200 baud, 8N1)
        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) {
            perror("tcgetattr failed");
            close(fd_);
            fd_ = -1;
            return;
        }

        cfsetospeed(&tty, B115200);
        cfsetispeed(&tty, B115200);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;  // 8-bit chars
        tty.c_iflag &= ~IGNBRK;                      // disable break processing
        tty.c_lflag = 0;                             // no signaling chars, no echo, no canonical
        tty.c_oflag = 0;                             // no remapping, no delays
        tty.c_cc[VMIN] = 0;                          // non-blocking read
        tty.c_cc[VTIME] = 5;                         // 0.5 second read timeout

        tty.c_iflag &= ~(IXON | IXOFF | IXANY);      // shut off xon/xoff ctrl
        tty.c_cflag |= (CLOCAL | CREAD);             // ignore modem controls, enable reading
        tty.c_cflag &= ~(PARENB | PARODD);           // no parity
        tty.c_cflag &= ~CSTOPB;                      // 1 stop bit
        tty.c_cflag &= ~CRTSCTS;                     // no hardware flow control

        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            perror("tcsetattr failed");
            close(fd_);
            fd_ = -1;
            return;
        }
    }

    ~UARTDevice() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    bool is_open() const { return fd_ >= 0; }

    bool send_packet(const PPoGPacket& packet, size_t payload_size) {
        std::vector<uint8_t> raw_packet;
        raw_packet.push_back(packet.header);
        raw_packet.insert(raw_packet.end(), packet.payload, packet.payload + payload_size);

        std::vector<uint8_t> encoded = cobs_encode(raw_packet);

        // Send: 0x00 + COBS + 0x00
        uint8_t delimiter = 0x00;
        if (write(fd_, &delimiter, 1) != 1) {
            perror("Failed to write delimiter");
            return false;
        }

        if (write(fd_, encoded.data(), encoded.size()) != (ssize_t)encoded.size()) {
            perror("Failed to write encoded packet");
            return false;
        }

        if (write(fd_, &delimiter, 1) != 1) {
            perror("Failed to write delimiter");
            return false;
        }

        return true;
    }

    bool receive_packet(PPoGPacket& packet, size_t& payload_size, int timeout_ms = 1000) {
        std::vector<uint8_t> cobs_buffer;
        uint8_t byte;
        bool in_frame = false;

        int remaining_ms = timeout_ms;
        const int poll_interval_ms = 50;

        while (remaining_ms > 0) {
            ssize_t n = read(fd_, &byte, 1);
            if (n == 1) {
                if (byte == 0x00) {
                    if (in_frame && !cobs_buffer.empty()) {
                        // End of frame
                        std::vector<uint8_t> decoded = cobs_decode(cobs_buffer);
                        if (decoded.size() >= 1) {
                            packet.header = decoded[0];
                            payload_size = decoded.size() - 1;
                            if (payload_size > 0) {
                                memcpy(packet.payload, &decoded[1], payload_size);
                            }
                            return true;
                        }
                        cobs_buffer.clear();
                        in_frame = false;
                    } else {
                        // Start of frame
                        cobs_buffer.clear();
                        in_frame = true;
                    }
                } else if (in_frame) {
                    cobs_buffer.push_back(byte);
                }
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(poll_interval_ms * 1000);
                    remaining_ms -= poll_interval_ms;
                    continue;
                } else {
                    perror("Read error");
                    return false;
                }
            } else {
                // Timeout on individual read
                usleep(poll_interval_ms * 1000);
                remaining_ms -= poll_interval_ms;
            }
        }

        return false;  // Timeout
    }

private:
    int fd_;
};

// Helper to check if a packet is a test packet
bool is_test_packet(const PPoGPacket& packet, size_t payload_size) {
    const uint8_t prefix[] = {0xBE, 0xEF, 0xCA, 0xFE};
    if (payload_size < sizeof(prefix)) {
        return false;
    }
    return memcmp(packet.payload, prefix, sizeof(prefix)) == 0;
}

// Helper to expect a specific packet type and optionally Validate payload
// Returns true if packet received and validated, false on timeout or error
bool expect_packet(UARTDevice& uart, 
                   PPoGPacketType type, 
                   int expected_sn, 
                   int timeout_ms,
                   const char* step_name,
                   std::function<bool(const PPoGPacket&, size_t)> extra_validator = nullptr) {
    
    int remaining_ms = timeout_ms;
    const int poll_interval_ms = 50;

    while (remaining_ms > 0) {
        PPoGPacket packet;
        size_t payload_size;

        if (uart.receive_packet(packet, payload_size, poll_interval_ms)) {
            // Check type
            if (packet.type != type) {
                printf("  [INFO] Skipping unexpected packet type %d\n", packet.type);
                continue;
            }

            // Check SN if not -1
            if (expected_sn != -1 && packet.sn != expected_sn) {
                printf("  [INFO] Skipping packet with unexpected SN %d (wanted %d)\n", packet.sn, expected_sn);
                continue;
            }

            // Check extra validator
            if (extra_validator && !extra_validator(packet, payload_size)) {
                printf("  [INFO] Skipping packet that failed validation\n");
                continue;
            }

            // Success
            return true;
        }

        remaining_ms -= poll_interval_ms;
    }

    printf("  [FAIL] %s: Timeout waiting for packet\n", step_name);
    return false;
}

bool test_reset_handshake(UARTDevice& uart) {
    printf("[TEST] Reset handshake\n");

    // Send ResetRequest (version 1)
    PPoGPacket reset_req = {};
    reset_req.type = PPOG_RESET_REQUEST;
    reset_req.sn = 0;
    reset_req.payload[0] = 1;  // Version 1

    printf("  Sending ResetRequest...\n");
    if (!uart.send_packet(reset_req, 1)) {
        printf("  [FAIL] Failed to send ResetRequest\n");
        return false;
    }
    printf("  [PASS] ResetRequest sent\n");

    // Expect ResetComplete from firmware
    printf("  Waiting for ResetComplete from firmware...\n");
    
    // We capture the window sizes to print them
    uint8_t fw_rx = 0, fw_tx = 0;
    auto validator = [&](const PPoGPacket& p, size_t size) {
        if (size >= 2) {
            fw_rx = p.payload[0];
            fw_tx = p.payload[1];
            return true;
        }
        return false;
    };

    if (!expect_packet(uart, PPOG_RESET_COMPLETE, -1, 5000, "Receive ResetComplete", validator)) {
        return false;
    }

    printf("  [PASS] ResetComplete received from firmware\n");
    printf("  Firmware windows: RX=%d TX=%d\n", fw_rx, fw_tx);

    // Send ResetComplete back to firmware
    PPoGPacket reset_complete2 = {};
    reset_complete2.type = PPOG_RESET_COMPLETE;
    reset_complete2.sn = 0;
    reset_complete2.payload[0] = 4;  // Our RX window
    reset_complete2.payload[1] = 4;  // Our TX window

    printf("  Sending ResetComplete to firmware...\n");
    if (!uart.send_packet(reset_complete2, 2)) {
        printf("  [FAIL] Failed to send ResetComplete\n");
        return false;
    }
    printf("  [PASS] ResetComplete sent\n");

    printf("[PASS] Reset handshake completed successfully\n\n");
    return true;
}

void print_hex_dump(const uint8_t* data, size_t len, size_t max_bytes = 64) {
    printf("    Hex: ");
    size_t display_len = (len > max_bytes) ? max_bytes : len;
    for (size_t i = 0; i < display_len; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0 && i + 1 < display_len) {
            printf("\n         ");
        }
    }
    if (len > max_bytes) {
        printf("... (%zu more bytes)", len - max_bytes);
    }
    printf("\n");
}

void print_ascii(const uint8_t* data, size_t len) {
    printf("    ASCII: \"");
    for (size_t i = 0; i < len && i < 64; i++) {
        if (data[i] >= 32 && data[i] < 127) {
            printf("%c", data[i]);
        } else {
            printf(".");
        }
    }
    if (len > 64) {
        printf("... (%zu more bytes)", len - 64);
    }
    printf("\"\n");
}

// Continuous monitoring mode - receives and acks all packets
void run_monitor(UARTDevice& uart) {
    printf("\n[MONITOR] Entering continuous monitoring mode\n");
    printf("  - Receiving and acking ALL packets from firmware\n");
    printf("  - Press Ctrl+C to exit\n");
    printf("  - Run 'k230 ppouart test' on firmware to send test data\n\n");

    uint8_t next_rx_sn = 0;
    uint32_t data_packets = 0;
    uint32_t ack_packets = 0;
    uint32_t other_packets = 0;

    while (true) {
        PPoGPacket packet;
        size_t payload_size;

        // Try to receive with 100ms timeout (non-blocking poll)
        if (!uart.receive_packet(packet, payload_size, 100)) {
            continue;  // No packet, loop again
        }

        // Handle by type
        switch (packet.type) {
            case PPOG_DATA: {
                data_packets++;
                bool is_test = is_test_packet(packet, payload_size);

                printf("[RX DATA] SN=%d, Size=%zu%s\n",
                       packet.sn, payload_size,
                       is_test ? " [TEST PACKET]" : "");

                if (payload_size > 0) {
                    print_hex_dump(packet.payload, payload_size);
                    print_ascii(packet.payload, payload_size);
                }

                // Send Ack immediately
                PPoGPacket ack = {};
                ack.type = PPOG_ACK;
                ack.sn = packet.sn;

                if (uart.send_packet(ack, 0)) {
                    printf("[TX ACK ] SN=%d\n\n", ack.sn);
                } else {
                    printf("[ERROR] Failed to send Ack for SN=%d\n\n", ack.sn);
                }

                // Update expected SN if in order
                // Note: Test packets may have arbitrary SNs, so don't warn for those
                if (packet.sn == next_rx_sn) {
                    next_rx_sn = (next_rx_sn + 1) % 32;
                } else if (!is_test) {
                    printf("[WARN] Out of order: got SN=%d, expected SN=%d\n\n",
                           packet.sn, next_rx_sn);
                }
                // For test packets with out-of-order SN, just ignore the warning
                break;
            }

            case PPOG_ACK: {
                ack_packets++;
                printf("[RX ACK ] SN=%d\n\n", packet.sn);
                break;
            }

            case PPOG_RESET_REQUEST: {
                other_packets++;
                printf("[RX RESET_REQ] Unexpected ResetRequest during monitoring\n\n");
                break;
            }

            case PPOG_RESET_COMPLETE: {
                other_packets++;
                printf("[RX RESET_CPL] Unexpected ResetComplete during monitoring\n\n");
                break;
            }

            default: {
                other_packets++;
                printf("[RX ???] Unknown type=%d, SN=%d\n\n", packet.type, packet.sn);
                break;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <tty_device>\n", argv[0]);
        fprintf(stderr, "Example: %s /dev/ttyS2\n", argv[0]);
        fprintf(stderr, "\nThis tool:\n");
        fprintf(stderr, "  1. Performs PPOUART reset handshake\n");
        fprintf(stderr, "  2. Enters continuous monitoring mode\n");
        fprintf(stderr, "  3. Receives and acks ALL packets from firmware\n");
        fprintf(stderr, "  4. Prints packet contents for debugging\n");
        fprintf(stderr, "\nUse with firmware:\n");
        fprintf(stderr, "  k230 on              # Power on K230, start PPOUART\n");
        fprintf(stderr, "  ./ppouart_test /dev/ttyS1  # Run this tool\n");
        fprintf(stderr, "  k230 ppouart test    # Send test data from firmware\n");
        return 1;
    }

    const char* device_path = argv[1];

    printf("=== PPOUART Test Tool ===\n");
    printf("Device: %s\n\n", device_path);

    UARTDevice uart(device_path);
    if (!uart.is_open()) {
        fprintf(stderr, "Failed to open UART device: %s\n", device_path);
        return 1;
    }

    printf("UART device opened successfully\n\n");

    // Perform reset handshake
    if (!test_reset_handshake(uart)) {
        printf("\n[FAIL] Reset handshake failed. Cannot continue.\n");
        return 1;
    }

    // Enter continuous monitoring mode (runs forever until Ctrl+C)
    run_monitor(uart);

    return 0;
}
