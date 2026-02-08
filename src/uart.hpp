#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#define restrict __restrict__

extern "C" {
#include "pebble/console/cobs.h"
#include "pebble/k230_protocol.pb.h"
}

inline constexpr size_t kRxBufferSize = 128;

struct UartError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct SyscallError : UartError {
    int saved_errno;
    SyscallError(const char* what, int err);
};

class Uart {
public:
    explicit Uart(int fd);

    // Receive a frame, blocking until complete. Returns payload span excluding CRC.
    // Throws on fatal error. Returns empty span on EINTR or EAGAIN.
    std::span<const uint8_t> receive();

    void send_raw(const uint8_t* data, size_t len);

    void send(const k230_K230ToNrf& msg);
    void send(const k230_NrfToK230& msg);

    int fd() const {
        return fd_;
    }

private:
    int fd_;
    uint8_t rx_buffer_[kRxBufferSize];
    CobsDecodeContext cobs_ctx_;
    bool in_frame_ = false;
};

void install_terminate_handler();