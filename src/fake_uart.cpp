#include "uart.hpp"

extern "C" {
#include "nanopb/pb_decode.h"
#include "pebble/k230_protocol.pb.h"
}

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

struct Opts {
    int heartbeat_ms = 3000;
};

static void print_usage(const char* argv0) {
    fprintf(stderr, "Usage: %s [--heartbeat=<ms>] [--help]\n", argv0);
}

static Opts parse_argv(int argc, char** argv) {
    Opts opts;

    static struct option long_options[] = {
        {"heartbeat", required_argument, nullptr, 'b'},
        {"help",      no_argument,       nullptr, 'h'},
        {nullptr,     0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:h", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'b':
                opts.heartbeat_ms = atoi(optarg);
                if (opts.heartbeat_ms <= 0) {
                    opts.heartbeat_ms = 3000;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                throw UartError("invalid arguments");
        }
    }

    return opts;
}

static volatile sig_atomic_t g_stop = 0;

static void signal_handler(int) {
    g_stop = 1;
}

struct Pty {
    int master_fd = -1;
    int slave_fd = -1;
    char slave_path[256] = {};

    Pty() {
        master_fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_fd < 0) {
            throw SyscallError("posix_openpt", errno);
        }

        if (grantpt(master_fd) < 0) {
            close(master_fd);
            throw SyscallError("grantpt", errno);
        }

        if (unlockpt(master_fd) < 0) {
            close(master_fd);
            throw SyscallError("unlockpt", errno);
        }

        const char* name = ptsname(master_fd);
        if (!name) {
            close(master_fd);
            throw SyscallError("ptsname", errno);
        }
        snprintf(slave_path, sizeof(slave_path), "%s", name);

        slave_fd = open(slave_path, O_RDWR | O_NOCTTY);
        if (slave_fd < 0) {
            close(master_fd);
            throw SyscallError("open slave", errno);
        }

        configure_raw(master_fd);
        configure_raw(slave_fd);

        // Set master to non-blocking for poll-based receive
        int flags = fcntl(master_fd, F_GETFL);
        if (flags >= 0) {
            fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    ~Pty() {
        if (slave_fd >= 0) {
            close(slave_fd);
        }
        if (master_fd >= 0) {
            close(master_fd);
        }
    }

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

private:
    static void configure_raw(int fd) {
        struct termios tty{};
        if (tcgetattr(fd, &tty) < 0) {
            throw SyscallError("tcgetattr", errno);
        }

        cfmakeraw(&tty);
        tty.c_cflag |= CLOCAL | CREAD;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
        tty.c_cflag |= CS8;

        if (tcsetattr(fd, TCSANOW, &tty) < 0) {
            throw SyscallError("tcsetattr", errno);
        }
    }
};

class StdinNonCanonGuard {
public:
    StdinNonCanonGuard() {
        is_tty_ = isatty(STDIN_FILENO);
        if (!is_tty_) {
            return;
        }

        if (tcgetattr(STDIN_FILENO, &orig_) < 0) {
            throw SyscallError("tcgetattr stdin", errno);
        }

        struct termios raw = orig_;
        raw.c_lflag &= ~ICANON;
        raw.c_lflag |= ISIG;
        raw.c_lflag |= ECHO;
        raw.c_iflag &= ~(IXON | IXOFF);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
            throw SyscallError("tcsetattr stdin", errno);
        }

        struct termios stdout_tty{};
        if (tcgetattr(STDOUT_FILENO, &stdout_tty) == 0) {
            stdout_tty.c_oflag |= OPOST | ONLCR;
            tcsetattr(STDOUT_FILENO, TCSANOW, &stdout_tty);
        }
    }

    ~StdinNonCanonGuard() {
        if (is_tty_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &orig_);
        }
    }

    StdinNonCanonGuard(const StdinNonCanonGuard&) = delete;
    StdinNonCanonGuard& operator=(const StdinNonCanonGuard&) = delete;

private:
    struct termios orig_{};
    bool is_tty_ = false;
};

static void send_heartbeat(Uart& uart) {
    k230_NrfToK230 msg = k230_NrfToK230_init_zero;
    msg.which_msg = k230_NrfToK230_heartbeat_tag;
    uart.send(msg);
    fprintf(stderr, "[fake_uart] Sent heartbeat\n");
}

static void send_keyboard(Uart& uart, uint32_t keycode, bool is_down) {
    k230_NrfToK230 msg = k230_NrfToK230_init_zero;
    msg.which_msg = k230_NrfToK230_keyboard_tag;
    msg.msg.keyboard.keycode = keycode;
    msg.msg.keyboard.is_down = is_down;
    fprintf(stderr, "[fake_uart] Sending keyboard %u %s\n", keycode,
        is_down ? "down" : "up");
    uart.send(msg);
}

static void send_touchpad(Uart& uart, int32_t dx, int32_t dy) {
    k230_NrfToK230 msg = k230_NrfToK230_init_zero;
    msg.which_msg = k230_NrfToK230_touchpad_tag;
    msg.msg.touchpad.delta_x = dx;
    msg.msg.touchpad.delta_y = dy;
    msg.msg.touchpad.surface_qual = 100;
    fprintf(stderr, "[fake_uart] Sending touchpad %d %d\n", dx, dy);
    uart.send(msg);
}

int main(int argc, char** argv) {
    install_terminate_handler();

    Opts opts = parse_argv(argc, argv);

    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Pty pty;
    Uart uart(pty.master_fd);

    printf("%s\n", pty.slave_path);
    fflush(stdout);

    fprintf(stderr, "[fake_uart] PTY created: %s\n", pty.slave_path);
    fprintf(stderr, "[fake_uart] Heartbeat interval: %d ms\n", opts.heartbeat_ms);
    fprintf(stderr, "[fake_uart] Waiting for daemon to connect...\n");

    StdinNonCanonGuard stdin_guard;

    bool connected = false;
    auto heartbeat_interval = std::chrono::milliseconds(opts.heartbeat_ms);
    auto last_heartbeat = std::chrono::steady_clock::now() - heartbeat_interval;

    enum class EscState { None, Esc, Bracket };
    EscState esc_state = EscState::None;

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = pty.master_fd;
    fds[1].events = POLLIN;

    while (!g_stop) {
        auto now = std::chrono::steady_clock::now();
        if ((!connected) && (now - last_heartbeat >= heartbeat_interval)) {
            send_heartbeat(uart);
            last_heartbeat = now;
        }

        int timeout_ms = 100;
        if (!connected) {
            auto remaining = heartbeat_interval - (now - last_heartbeat);
            timeout_ms = std::max(0,
                static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        remaining).count()));
        }

        int ret = poll(fds, 2, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw SyscallError("poll", errno);
        }

        if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            fprintf(stderr, "[fake_uart] Daemon disconnected\n");
            break;
        }

        if (fds[1].revents & POLLIN) {
            auto payload = uart.receive();
            if (!payload.empty()) {
                k230_K230ToNrf msg = k230_K230ToNrf_init_zero;
                pb_istream_t stream = pb_istream_from_buffer(
                    payload.data(), payload.size());
                if (pb_decode(&stream, k230_K230ToNrf_fields, &msg)) {
                    switch (msg.which_msg) {
                        case k230_K230ToNrf_ack_tag:
                            if (!connected) {
                                connected = true;
                                fprintf(stderr, "[fake_uart] ACK received, connected!\n");
                            }
                            break;

                        case k230_K230ToNrf_shutdown_tag:
                            fprintf(stderr, "[fake_uart] Shutdown received, exiting\n");
                            g_stop = 1;
                            break;

                        default:
                            fprintf(stderr, "[fake_uart] Unknown response %u\n",
                                static_cast<unsigned>(msg.which_msg));
                            break;
                    }
                }
            }
        }

        if (fds[0].revents & POLLIN) {
            uint8_t byte;
            ssize_t n = read(STDIN_FILENO, &byte, 1);
            if (n <= 0) {
                continue;
            }

            if (byte == 0x04) {
                fprintf(stderr, "[fake_uart] Ctrl-D pressed, exiting\n");
                break;
            }

            switch (esc_state) {
                case EscState::None:
                    if (byte == 0x1B) {
                        esc_state = EscState::Esc;
                    } else {
                        send_keyboard(uart, byte, true);
                        send_keyboard(uart, byte, false);
                    }
                    break;

                case EscState::Esc:
                    if (byte == '[') {
                        esc_state = EscState::Bracket;
                    } else {
                        send_keyboard(uart, 0x1B, true);
                        send_keyboard(uart, 0x1B, false);
                        if (byte != 0x1B) {
                            send_keyboard(uart, byte, true);
                            send_keyboard(uart, byte, false);
                            esc_state = EscState::None;
                        }
                    }
                    break;

                case EscState::Bracket:
                    esc_state = EscState::None;
                    switch (byte) {
                        case 'A':
                            send_touchpad(uart, 0, -10);
                            break;
                        case 'B':
                            send_touchpad(uart, 0, 10);
                            break;
                        case 'C':
                            send_touchpad(uart, 10, 0);
                            break;
                        case 'D':
                            send_touchpad(uart, -10, 0);
                            break;
                        default:
                            break;
                    }
                    break;
            }
        }
    }

    return 0;
}
