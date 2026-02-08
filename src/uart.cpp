#define restrict __restrict__

extern "C" {
#include "nanopb/pb_encode.h"
#include "nanopb/pb_decode.h"
#include "pebble/console/cobs.h"
#include "pebble/util/crc8.h"
#include "pebble/k230_protocol.pb.h"
}

#include "uart.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include <unistd.h>

SyscallError::SyscallError(const char* what, int err)
    : UartError(what), saved_errno(err) {}

static void terminate_handler() {
    try {
        if (auto eptr = std::current_exception()) {
            std::rethrow_exception(eptr);
        }
    } catch (const SyscallError& e) {
        fprintf(stderr, "Fatal: %s: %s\n", e.what(), strerror(e.saved_errno));
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "Fatal: unknown exception\n");
    }
    std::abort();
}

void install_terminate_handler() {
    std::set_terminate(terminate_handler);
}

Uart::Uart(int fd) : fd_(fd), in_frame_(false) {
    cobs_streaming_decode_start(&cobs_ctx_, rx_buffer_, sizeof(rx_buffer_));
}

std::span<const uint8_t> Uart::receive() {
    while (true) {
        uint8_t byte;
        ssize_t n = read(fd_, &byte, 1);

        if (n < 0) {
            if ((errno == EINTR) || (errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                return {};
            }
            throw SyscallError("read", errno);
        }

        if (n == 0) {
            throw UartError("UART EOF");
        }

        if (byte == 0x00) {
            if (in_frame_) {
                size_t decoded_len = cobs_streaming_decode_finish(&cobs_ctx_);
                // Reset for next frame
                cobs_streaming_decode_start(&cobs_ctx_, rx_buffer_, sizeof(rx_buffer_));
                in_frame_ = true;

                if ((decoded_len != SIZE_MAX) && (decoded_len >= 2)) {
                    uint8_t expected_crc = rx_buffer_[decoded_len - 1];
                    uint8_t actual_crc = crc8_calculate_bytes(
                        rx_buffer_, decoded_len - 1, false);
                    if (actual_crc == expected_crc) {
                        return std::span<const uint8_t>(rx_buffer_, decoded_len - 1);
                    }
                }
            } else {
                cobs_streaming_decode_start(&cobs_ctx_, rx_buffer_, sizeof(rx_buffer_));
                in_frame_ = true;
            }
        } else if (in_frame_) {
            if (!cobs_streaming_decode(&cobs_ctx_, static_cast<char>(byte))) {
                in_frame_ = false;
            }
        }
    }
}

void Uart::send_raw(const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = write(fd_, data + offset, len - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw SyscallError("write", errno);
        }
        offset += static_cast<size_t>(n);
    }
}

template<typename MsgType>
static void encode_and_send(Uart& uart, const MsgType& msg,
    const pb_msgdesc_t* fields) {
    uint8_t pb_buffer[128];
    pb_ostream_t stream = pb_ostream_from_buffer(pb_buffer, sizeof(pb_buffer));
    if (!pb_encode(&stream, fields, &msg)) {
        throw UartError("pb_encode failed");
    }
    size_t pb_len = stream.bytes_written;

    uint8_t crc = crc8_calculate_bytes(pb_buffer, pb_len, false);
    pb_buffer[pb_len] = crc;
    size_t payload_len = pb_len + 1;

    uint8_t cobs_buffer[MAX_SIZE_AFTER_COBS_ENCODING(128)];
    size_t cobs_len = cobs_encode(cobs_buffer, pb_buffer, payload_len);

    // Write delimiter + COBS data + trailing delimiter
    // Trailing delimiter ensures receiver can complete the frame immediately
    uint8_t delim = 0x00;
    uart.send_raw(&delim, 1);
    uart.send_raw(cobs_buffer, cobs_len);
    uart.send_raw(&delim, 1);
}

void Uart::send(const k230_K230ToNrf& msg) {
    encode_and_send(*this, msg, k230_K230ToNrf_fields);
}

void Uart::send(const k230_NrfToK230& msg) {
    encode_and_send(*this, msg, k230_NrfToK230_fields);
}