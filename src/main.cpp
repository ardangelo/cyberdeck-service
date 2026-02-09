#include "uart.hpp"

extern "C" {
#include "nanopb/pb_decode.h"
#include "pebble/k230_protocol.pb.h"
}

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void signal_handler(int) {
    g_stop = 1;
}

struct Opts {
    const char* uart_path = nullptr;
    const char* uinput_path = nullptr;
    bool test_mode = false;
    bool skip_heartbeat = false;
};

static void print_usage(const char* argv0) {
    fprintf(stderr, "Usage: %s --uart=<path> --uinput=<path> [--test] [--skip-heartbeat]\n",
        argv0);
}

static Opts parse_argv(int argc, char** argv) {
    Opts opts;

    static struct option long_options[] = {
        {"uart",           required_argument, nullptr, 'u'},
        {"uinput",         required_argument, nullptr, 'i'},
        {"test",           no_argument,       nullptr, 't'},
        {"skip-heartbeat", no_argument,       nullptr, 's'},
        {"help",           no_argument,       nullptr, 'h'},
        {nullptr,          0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "u:i:t:s:h", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'u':
                opts.uart_path = optarg;
                break;
            case 'i':
                opts.uinput_path = optarg;
                break;
            case 't':
                opts.test_mode = true;
                break;
            case 's':
                opts.skip_heartbeat = true;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                throw UartError("invalid arguments");
        }
    }

    if ((!opts.uart_path) || (!opts.uinput_path)) {
        print_usage(argv[0]);
        throw UartError("missing required arguments");
    }

    return opts;
}

static int open_uart(const char* path) {
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        throw SyscallError("open uart", errno);
    }

    struct termios tty{};
    if (tcgetattr(fd, &tty) < 0) {
        close(fd);
        throw SyscallError("tcgetattr", errno);
    }

    cfmakeraw(&tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tty.c_cflag |= CS8;

    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) < 0) {
        close(fd);
        throw SyscallError("tcsetattr", errno);
    }

    return fd;
}

enum class IoctlSetup { Run, Skip };

class UInputDevice {
public:
    UInputDevice(const char* path, IoctlSetup setup) {
        fprintf(stderr, "uinput: opening %s\n", path);
        fd_ = open(path, O_WRONLY | O_NONBLOCK);
        if (fd_ < 0) {
            throw SyscallError("open uinput", errno);
        }
        fprintf(stderr, "uinput: opened fd=%d\n", fd_);

        if (setup == IoctlSetup::Skip) {
            fprintf(stderr, "uinput: skipping ioctl setup (test mode)\n");
            return;
        }

        if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0) {
            throw SyscallError("UI_SET_EVBIT EV_KEY", errno);
        }
        fprintf(stderr, "uinput: UI_SET_EVBIT EV_KEY ok\n");

        int key_fail = 0;
        for (int i = 0; i <= KEY_MAX; ++i) {
            if (ioctl(fd_, UI_SET_KEYBIT, i) < 0) {
                ++key_fail;
            }
        }
        fprintf(stderr, "uinput: UI_SET_KEYBIT 0..%d (%d failures)\n",
            KEY_MAX, key_fail);

        if (ioctl(fd_, UI_SET_EVBIT, EV_REL) < 0) {
            throw SyscallError("UI_SET_EVBIT EV_REL", errno);
        }
        fprintf(stderr, "uinput: UI_SET_EVBIT EV_REL ok\n");
        if (ioctl(fd_, UI_SET_RELBIT, REL_X) < 0) {
            throw SyscallError("UI_SET_RELBIT REL_X", errno);
        }
        if (ioctl(fd_, UI_SET_RELBIT, REL_Y) < 0) {
            throw SyscallError("UI_SET_RELBIT REL_Y", errno);
        }
        fprintf(stderr, "uinput: UI_SET_RELBIT REL_X/REL_Y ok\n");

        struct uinput_setup usetup{};
        snprintf(usetup.name, sizeof(usetup.name), "K230 Virtual Input");
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = 0x1234;
        usetup.id.product = 0x5678;
        usetup.id.version = 1;

        if (ioctl(fd_, UI_DEV_SETUP, &usetup) < 0) {
            throw SyscallError("UI_DEV_SETUP", errno);
        }
        fprintf(stderr, "uinput: UI_DEV_SETUP ok (name=%s bus=0x%x vendor=0x%x product=0x%x)\n",
            usetup.name, usetup.id.bustype, usetup.id.vendor, usetup.id.product);

        if (ioctl(fd_, UI_DEV_CREATE) < 0) {
            throw SyscallError("UI_DEV_CREATE", errno);
        }
        fprintf(stderr, "uinput: UI_DEV_CREATE ok, waiting for kernel registration...\n");
        usleep(200000); // 200ms for kernel to register the device
        fprintf(stderr, "uinput: device created successfully on fd %d\n", fd_);
    }

    ~UInputDevice() {
        if (fd_ >= 0) {
            ioctl(fd_, UI_DEV_DESTROY);
            close(fd_);
        }
    }

    UInputDevice(const UInputDevice&) = delete;
    UInputDevice& operator=(const UInputDevice&) = delete;

    UInputDevice(UInputDevice&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    UInputDevice& operator=(UInputDevice&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ioctl(fd_, UI_DEV_DESTROY);
                close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    void emit_key(uint16_t code, int32_t value) {
        emit(EV_KEY, code, value);
    }

    void emit_rel(uint16_t code, int32_t value) {
        emit(EV_REL, code, value);
    }

    void emit_sync() {
        emit(EV_SYN, SYN_REPORT, 0);
    }

private:
    void emit(uint16_t type, uint16_t code, int32_t value) {
        struct input_event ev{};
        gettimeofday(&ev.time, nullptr);
        ev.type = type;
        ev.code = code;
        ev.value = value;
        ssize_t n = write(fd_, &ev, sizeof(ev));
        if (n < 0) {
            fprintf(stderr, "uinput: write failed: %s (fd=%d type=%u code=%u value=%d)\n",
                strerror(errno), fd_, type, code, value);
        } else if (n != sizeof(ev)) {
            fprintf(stderr, "uinput: short write: %zd/%zu\n", n, sizeof(ev));
        }
    }

    int fd_ = -1;
};

enum class State { Waiting, Connected };

static void send_ack(Uart& uart) {
    k230_K230ToNrf msg = k230_K230ToNrf_init_zero;
    msg.which_msg = k230_K230ToNrf_ack_tag;
    msg.msg.ack.sequence = 0;
    uart.send(msg);
}

static void send_shutdown(Uart& uart) {
    k230_K230ToNrf msg = k230_K230ToNrf_init_zero;
    msg.which_msg = k230_K230ToNrf_shutdown_tag;
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

    int uart_fd = open_uart(opts.uart_path);
    Uart uart(uart_fd);

    IoctlSetup ioctl_setup = opts.test_mode ? IoctlSetup::Skip : IoctlSetup::Run;
    UInputDevice uinput(opts.uinput_path, ioctl_setup);

    State state = opts.skip_heartbeat ? State::Connected : State::Waiting;
    if (opts.skip_heartbeat) {
        fprintf(stderr, "Skipping heartbeat, starting connected\n");
    } else {
        fprintf(stderr, "Waiting for heartbeat...\n");
    }

    while (!g_stop) {
        auto payload = uart.receive();
        if (payload.empty()) {
            continue;
        }

        // Debug: dump received payload
        fprintf(stderr, "RX [%zu bytes]:", payload.size());
        for (size_t i = 0; i < payload.size(); ++i) {
            fprintf(stderr, " %02x", payload[i]);
        }
        fprintf(stderr, "\n");

        k230_NrfToK230 msg = k230_NrfToK230_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload.data(), payload.size());
        if (!pb_decode(&stream, k230_NrfToK230_fields, &msg)) {
            fprintf(stderr, "Warning: pb_decode failed\n");
            continue;
        }

        fprintf(stderr, "Decoded which_msg=%u\n", static_cast<unsigned>(msg.which_msg));

        switch (msg.which_msg) {
            case k230_NrfToK230_heartbeat_tag:
                fprintf(stderr, "Heartbeat received, sending ACK\n");
                send_ack(uart);
                if (state == State::Waiting) {
                    state = State::Connected;
                    fprintf(stderr, "Connected\n");
                }
                break;

            case k230_NrfToK230_keyboard_tag:
                if (state == State::Connected) {
                    auto& kb = msg.msg.keyboard;
                    fprintf(stderr, "Keyboard %u %s\n", kb.keycode,
                        kb.is_down ? "down" : "up");
                    uinput.emit_key(static_cast<uint16_t>(kb.keycode),
                        kb.is_down ? 1 : 0);
                    uinput.emit_sync();
                }
                break;

            case k230_NrfToK230_touchpad_tag:
                if (state == State::Connected) {
                    auto& tp = msg.msg.touchpad;
                    fprintf(stderr, "Touchpad %d %d\n", tp.delta_x, tp.delta_y);
                    if (tp.delta_x != 0) {
                        uinput.emit_rel(REL_X, tp.delta_x);
                    }
                    if (tp.delta_y != 0) {
                        uinput.emit_rel(REL_Y, tp.delta_y);
                    }
                    uinput.emit_sync();
                }
                break;

            default:
                fprintf(stderr, "Warning: unknown message type %u\n",
                    static_cast<unsigned>(msg.which_msg));
                break;
        }
    }

    fprintf(stderr, "Shutdown sent, exiting\n");
    //send_shutdown(uart);
    close(uart_fd);

    return 0;
}
