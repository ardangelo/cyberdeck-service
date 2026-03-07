#include "jni/uart_jni_bridge.hpp"

extern "C" {
#include "nanopb/pb_decode.h"
#include "nanopb/pb_encode.h"
#include "pebble/k230_protocol.pb.h"
}

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif
#ifndef DRM_COMMAND_BASE
#define DRM_COMMAND_BASE 0x40
#endif
#ifndef DRM_IO
#define DRM_IO(nr) _IO(DRM_IOCTL_BASE, nr)
#endif

static volatile sig_atomic_t g_stop = 0;

struct Opts {
    const char* uart_path = nullptr;
    const char* uinput_path = nullptr;
    const char* jar_path = "/tmp/libpebble3-jvm-fat.jar";
    const char* display_path = nullptr;
    bool test_mode = false;
};

static void print_usage(const char* argv0) {
    fprintf(stderr, "Usage: %s --uart=<path> --uinput=<path> [--jar=<path>] [--display=<path>] [--test]\n",
        argv0);
}

static Opts parse_argv(int argc, char** argv) {
    Opts opts;

    static struct option long_options[] = {
        {"uart",    required_argument, nullptr, 'u'},
        {"uinput",  required_argument, nullptr, 'i'},
        {"jar",     required_argument, nullptr, 'j'},
        {"display", required_argument, nullptr, 'd'},
        {"test",    no_argument,       nullptr, 't'},
        {"help",    no_argument,       nullptr, 'h'},
        {nullptr,   0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "u:i:j:d:th", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'u':
                opts.uart_path = optarg;
                break;
            case 'i':
                opts.uinput_path = optarg;
                break;
            case 'j':
                opts.jar_path = optarg;
                break;
            case 'd':
                opts.display_path = optarg;
                break;
            case 't':
                opts.test_mode = true;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }

    if (!opts.uart_path || !opts.uinput_path) {
        print_usage(argv[0]);
        exit(1);
    }

    return opts;
}

enum class IoctlSetup { Run, Skip };

class UInputDevice {
public:
    UInputDevice(const char* path, IoctlSetup setup) {
        fprintf(stderr, "uinput: opening %s\n", path);
        fd_ = open(path, O_WRONLY | O_NONBLOCK);
        if (fd_ < 0) {
            fprintf(stderr, "uinput: open failed: %s\n", strerror(errno));
            return;
        }
        fprintf(stderr, "uinput: opened fd=%d\n", fd_);

        if (setup == IoctlSetup::Skip) {
            fprintf(stderr, "uinput: skipping ioctl setup (test mode)\n");
            return;
        }

        if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0) {
            fprintf(stderr, "uinput: UI_SET_EVBIT EV_KEY failed: %s\n", strerror(errno));
            return;
        }

        for (int i = 0; i <= KEY_MAX; ++i) {
            ioctl(fd_, UI_SET_KEYBIT, i);
        }

        if (ioctl(fd_, UI_SET_EVBIT, EV_REL) < 0) {
            fprintf(stderr, "uinput: UI_SET_EVBIT EV_REL failed: %s\n", strerror(errno));
            return;
        }
        ioctl(fd_, UI_SET_RELBIT, REL_X);
        ioctl(fd_, UI_SET_RELBIT, REL_Y);

        struct uinput_setup usetup{};
        snprintf(usetup.name, sizeof(usetup.name), "K230 Virtual Input");
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = 0x1234;
        usetup.id.product = 0x5678;
        usetup.id.version = 1;

        if (ioctl(fd_, UI_DEV_SETUP, &usetup) < 0) {
            fprintf(stderr, "uinput: UI_DEV_SETUP failed: %s\n", strerror(errno));
            return;
        }

        if (ioctl(fd_, UI_DEV_CREATE) < 0) {
            fprintf(stderr, "uinput: UI_DEV_CREATE failed: %s\n", strerror(errno));
            return;
        }
        fprintf(stderr, "uinput: device created on fd %d\n", fd_);
        usleep(200000);
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
        }
    }

    int fd_ = -1;
};

static constexpr uint16_t K230_ENDPOINT = 0xCD00;

class DisplayDevice {
public:
    explicit DisplayDevice(const char* path) : path_(path) {}

    ~DisplayDevice() {
        if (fd_ >= 0) close(fd_);
    }

    DisplayDevice(const DisplayDevice&) = delete;
    DisplayDevice& operator=(const DisplayDevice&) = delete;

    void redraw() {
        if (!path_) {
            return;
        }
        fprintf(stderr, "RedrawDisplay\n");
        if (!ensure_open()) return;
        if (ioctl(fd_, kDrmIoctlSharpRedraw, 0) < 0) {
            fprintf(stderr, "display: redraw ioctl failed on %s: %s\n",
                path_, strerror(errno));
            close(fd_);
            fd_ = -1;
            return;
        }
        fprintf(stderr, "display: redraw ioctl sent on %s\n", path_);
    }

private:
    bool ensure_open() {
        if (fd_ >= 0) return true;
        fd_ = open(path_, O_WRONLY | O_CLOEXEC);
        if (fd_ < 0) {
            fprintf(stderr, "display: open %s failed: %s\n", path_, strerror(errno));
            return false;
        }
        fprintf(stderr, "display: opened %s (fd=%d)\n", path_, fd_);
        return true;
    }

    static constexpr unsigned int kDrmSharpRedraw = 0x00;
    static constexpr unsigned long kDrmIoctlSharpRedraw =
        DRM_IO(DRM_COMMAND_BASE + kDrmSharpRedraw);

    const char* path_;
    int fd_ = -1;
};

static void send_disconnect(cyberdeck::UartJniBridge& bridge) {
    k230_K230ToNrf msg = k230_K230ToNrf_init_zero;
    msg.which_msg = k230_K230ToNrf_disconnect_tag;

    uint8_t buf[k230_K230ToNrf_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, k230_K230ToNrf_fields, &msg)) {
        fprintf(stderr, "Failed to encode Disconnect message\n");
        return;
    }

    std::vector<uint8_t> data(buf, buf + stream.bytes_written);
    if (bridge.send_message_blocking(K230_ENDPOINT, data)) {
        fprintf(stderr, "Disconnect message written to UART\n");
    } else {
        fprintf(stderr, "Failed to send Disconnect (timeout or error)\n");
    }
}

// Signal mask for SIGINT/SIGTERM — used with sigtimedwait()
static sigset_t g_sigmask;

static bool check_signal() {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms
    int sig = sigtimedwait(&g_sigmask, nullptr, &ts);
    if (sig > 0) {
        g_stop = 1;
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    Opts opts = parse_argv(argc, argv);

    // Block SIGINT/SIGTERM on the main thread BEFORE creating the JVM.
    // JVM threads inherit this mask, so signals never reach them.
    sigemptyset(&g_sigmask);
    sigaddset(&g_sigmask, SIGINT);
    sigaddset(&g_sigmask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &g_sigmask, nullptr);

    IoctlSetup ioctl_setup = opts.test_mode ? IoctlSetup::Skip : IoctlSetup::Run;
    UInputDevice uinput(opts.uinput_path, ioctl_setup);
    DisplayDevice display(opts.display_path);

    fprintf(stderr, "Creating JNI bridge (JAR: %s)...\n", opts.jar_path);
    cyberdeck::UartJniBridge bridge(opts.jar_path);

    volatile bool connected = false;

    bridge.set_endpoint_callback([&uinput, &display](uint16_t endpoint, const std::vector<uint8_t>& data) {
        if (endpoint != K230_ENDPOINT) {
            fprintf(stderr, "Ignoring endpoint 0x%04x (%zu bytes)\n", endpoint, data.size());
            return;
        }

        if (data.empty()) {
            fprintf(stderr, "K230: empty payload\n");
            return;
        }

        k230_NrfToK230 msg = k230_NrfToK230_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(data.data(), data.size());
        if (!pb_decode(&stream, k230_NrfToK230_fields, &msg)) {
            fprintf(stderr, "K230: pb_decode failed\n");
            return;
        }

        switch (msg.which_msg) {
            case k230_NrfToK230_keyboard_tag: {
                auto& kb = msg.msg.keyboard;
                fprintf(stderr, "Keyboard %u %s\n", kb.keycode,
                    kb.is_down ? "down" : "up");
                uinput.emit_key(static_cast<uint16_t>(kb.keycode),
                    kb.is_down ? 1 : 0);
                uinput.emit_sync();
                break;
            }

            case k230_NrfToK230_touchpad_tag: {
                auto& tp = msg.msg.touchpad;
                fprintf(stderr, "Touchpad %d %d\n", tp.delta_x, tp.delta_y);
                if (tp.delta_x != 0) {
                    uinput.emit_rel(REL_X, tp.delta_x);
                }
                if (tp.delta_y != 0) {
                    uinput.emit_rel(REL_Y, tp.delta_y);
                }
                uinput.emit_sync();
                break;
            }

            case k230_NrfToK230_redraw_display_tag:
                display.redraw();
                break;

            default:
                fprintf(stderr, "K230: unknown msg tag=%u\n",
                    static_cast<unsigned>(msg.which_msg));
                break;
        }
    });

    bridge.set_connected_callback([]() {
        fprintf(stderr, "UART connected, waiting for PPoG handshake...\n");
    });

    bridge.set_session_opened_callback([&connected]() {
        connected = true;
        fprintf(stderr, "PPoG session opened, ready\n");
    });

    bridge.set_disconnected_callback([]() {
        fprintf(stderr, "PPoG session closed\n");
    });

    fprintf(stderr, "Initializing bridge on %s...\n", opts.uart_path);
    if (!bridge.init(opts.uart_path)) {
        fprintf(stderr, "Failed to initialize bridge\n");
        return 1;
    }

    if (!bridge.connect()) {
        fprintf(stderr, "Failed to connect\n");
        return 1;
    }

    // Poll for signals with sigtimedwait (100ms timeout) — signals are blocked
    // on all threads, so JVM internals are never disrupted by SIGINT/SIGTERM.
    while (!connected && !g_stop) {
        check_signal();
    }

    while (!g_stop) {
        check_signal();
    }

    fprintf(stderr, "Shutting down...\n");
    send_disconnect(bridge);
    bridge.disconnect();

    return 0;
}
