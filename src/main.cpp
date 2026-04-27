#include "jni/uart_jni_bridge.hpp"

extern "C" {
#include "nanopb/pb_decode.h"
#include "nanopb/pb_encode.h"
#include "pebble/k230_protocol.pb.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
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
static constexpr uint16_t K230_ENDPOINT = 0xCD00;
static constexpr uint16_t K230_RESERVED_KEYCODE_BASE = 200;
static constexpr const char *kGeneratedKeymapFile = "cyberdeck.kmap";
static constexpr const char *kConsoleDevice = "/dev/tty1";

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

    void emit_key(uint16_t code, int32_t value) {
        emit(EV_KEY, code, value);
    }

    void emit_rel(uint16_t code, int32_t value) {
        emit(EV_REL, code, value);
    }

    void emit_sync() {
        emit(EV_SYN, SYN_REPORT, 0);
    }

    void emit_keypress(uint16_t code) {
        emit_key(code, 1);
        emit_sync();
        emit_key(code, 0);
        emit_sync();
    }

    void emit_chord(const std::vector<uint16_t>& modifiers, uint16_t keycode) {
        for (uint16_t modifier : modifiers) {
            emit_key(modifier, 1);
        }
        emit_key(keycode, 1);
        emit_sync();

        emit_key(keycode, 0);
        for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) {
            emit_key(*it, 0);
        }
        emit_sync();
    }

private:
    void emit(uint16_t type, uint16_t code, int32_t value) {
        if (fd_ < 0) {
            return;
        }

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

class DisplayDevice {
public:
    explicit DisplayDevice(const char* path) : path_(path) {}

    ~DisplayDevice() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    DisplayDevice(const DisplayDevice&) = delete;
    DisplayDevice& operator=(const DisplayDevice&) = delete;

    void redraw() {
        if (!path_) {
            return;
        }
        fprintf(stderr, "RedrawDisplay\n");
        if (!ensure_open()) {
            return;
        }
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
        if (fd_ >= 0) {
            return true;
        }
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

struct CustomKeymapEntry {
    uint8_t base_keycode = 0;
    k230_FormattedLayer layer = k230_FormattedLayer_FORMATTED_LAYER_NONE;
    uint8_t reserved_keycode = 0;
    std::array<uint8_t, 4> utf8{};
};

struct ReplayKeypress {
    uint16_t keycode = 0;
    bool shift = false;
};

static std::optional<uint32_t> utf8_to_codepoint(const std::array<uint8_t, 4>& utf8) {
    if (utf8[0] == 0) {
        return std::nullopt;
    }

    const uint8_t first = utf8[0];
    if ((first & 0x80u) == 0) {
        if (utf8[1] != 0 || utf8[2] != 0 || utf8[3] != 0) {
            return std::nullopt;
        }
        return first;
    }

    if ((first & 0xe0u) == 0xc0u) {
        if ((utf8[1] & 0xc0u) != 0x80u || utf8[2] != 0 || utf8[3] != 0) {
            return std::nullopt;
        }
        return ((first & 0x1fu) << 6) | (utf8[1] & 0x3fu);
    }

    if ((first & 0xf0u) == 0xe0u) {
        if ((utf8[1] & 0xc0u) != 0x80u || (utf8[2] & 0xc0u) != 0x80u || utf8[3] != 0) {
            return std::nullopt;
        }
        return ((first & 0x0fu) << 12) |
               ((utf8[1] & 0x3fu) << 6) |
               (utf8[2] & 0x3fu);
    }

    if ((first & 0xf8u) == 0xf0u) {
        if ((utf8[1] & 0xc0u) != 0x80u ||
            (utf8[2] & 0xc0u) != 0x80u ||
            (utf8[3] & 0xc0u) != 0x80u) {
            return std::nullopt;
        }
        return ((first & 0x07u) << 18) |
               ((utf8[1] & 0x3fu) << 12) |
               ((utf8[2] & 0x3fu) << 6) |
               (utf8[3] & 0x3fu);
    }

    return std::nullopt;
}

static std::filesystem::path get_state_directory() {
    if (const char* state_dir = std::getenv("STATE_DIRECTORY")) {
        return state_dir;
    }
    return "/var/lib/cyberdeck-service";
}

static bool run_command(const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
        return false;
    }

    return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

// Built-in keymap seeds — (base, layer, utf8) tuples. Source-of-truth mirror
// of the printable PHYS_ALT/SYM entries (and KEY_BB_DOLLAR cross-layer) in
// /store/git/cyberdeck-firmware/src/fw/services/common/keyboard_modifiers.h.
// Keep in sync with the firmware's modifiers_phys_alt / modifiers_altgr.
//
// Reserved console keycodes (used to address the loaded console keymap) are
// assigned at runtime by deduping on UTF-8 — many seeds share an output char,
// and the legacy console keymap only has 256 slots, so we can't afford one
// keycode per seed.
//
// KEY_BB_DOLLAR == KEY_MUTE (113) per firmware keyboard_codes.h.
struct KeymapSeed {
    uint16_t base_keycode;
    k230_FormattedLayer layer;
    std::array<uint8_t, 4> utf8;
};

static const std::array<KeymapSeed, 62> kKeymapSeeds = {{
    // NONE layer
    { KEY_MUTE, k230_FormattedLayer_FORMATTED_LAYER_NONE, {'$', 0, 0, 0} },

    // SHIFT layer
    { KEY_MUTE, k230_FormattedLayer_FORMATTED_LAYER_SHIFT, {'$', 0, 0, 0} },

    // PHYS_ALT layer
    { KEY_0,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'0', 0, 0, 0} },
    { KEY_BACKSPACE, k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {0x7f, 0, 0, 0} },
    { KEY_Q,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'#', 0, 0, 0} },
    { KEY_W,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'1', 0, 0, 0} },
    { KEY_E,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'2', 0, 0, 0} },
    { KEY_R,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'3', 0, 0, 0} },
    { KEY_T,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'(', 0, 0, 0} },
    { KEY_Y,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {')', 0, 0, 0} },
    { KEY_U,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'_', 0, 0, 0} },
    { KEY_I,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'-', 0, 0, 0} },
    { KEY_O,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'+', 0, 0, 0} },
    { KEY_P,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'@', 0, 0, 0} },
    { KEY_A,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'*', 0, 0, 0} },
    { KEY_S,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'4', 0, 0, 0} },
    { KEY_D,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'5', 0, 0, 0} },
    { KEY_F,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'6', 0, 0, 0} },
    { KEY_G,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'/', 0, 0, 0} },
    { KEY_H,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {':', 0, 0, 0} },
    { KEY_J,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {';', 0, 0, 0} },
    { KEY_K,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'\'', 0, 0, 0} },
    { KEY_L,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'"', 0, 0, 0} },
    { KEY_Z,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'7', 0, 0, 0} },
    { KEY_X,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'8', 0, 0, 0} },
    { KEY_C,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'9', 0, 0, 0} },
    { KEY_V,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'?', 0, 0, 0} },
    { KEY_B,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'!', 0, 0, 0} },
    { KEY_N,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {',', 0, 0, 0} },
    { KEY_M,         k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'.', 0, 0, 0} },
    { KEY_SPACE,     k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {' ', 0, 0, 0} },
    { KEY_MUTE,      k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT, {'$', 0, 0, 0} },

    // SYM layer (modifiers_altgr)
    { KEY_0,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'0', 0, 0, 0} },
    { KEY_BACKSPACE, k230_FormattedLayer_FORMATTED_LAYER_SYM, {0x7f, 0, 0, 0} },
    { KEY_Q,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'~', 0, 0, 0} },
    { KEY_W,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'`', 0, 0, 0} },
    { KEY_E,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'{', 0, 0, 0} },
    { KEY_R,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'}', 0, 0, 0} },
    { KEY_T,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'[', 0, 0, 0} },
    { KEY_Y,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {']', 0, 0, 0} },
    { KEY_U,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'<', 0, 0, 0} },
    { KEY_I,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'>', 0, 0, 0} },
    { KEY_O,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'^', 0, 0, 0} },
    { KEY_P,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'%', 0, 0, 0} },
    { KEY_A,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'=', 0, 0, 0} },
    { KEY_S,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc3, 0xb7, 0, 0} },     // ÷
    { KEY_D,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc2, 0xb1, 0, 0} },     // ±
    { KEY_F,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc2, 0xb7, 0, 0} },     // ·
    { KEY_G,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'\\', 0, 0, 0} },
    { KEY_H,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'|', 0, 0, 0} },
    { KEY_J,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {'&', 0, 0, 0} },
    { KEY_K,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xe2, 0x80, 0x9c, 0} }, // “
    { KEY_L,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xe2, 0x80, 0x9d, 0} }, // ”
    { KEY_Z,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc2, 0xa5, 0, 0} },     // ¥
    { KEY_X,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc2, 0x82, 0xac, 0} }, // € (matches firmware byte sequence)
    { KEY_C,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc2, 0xa3, 0, 0} },     // £
    { KEY_V,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc2, 0xbf, 0, 0} },     // ¿
    { KEY_B,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc2, 0xa1, 0, 0} },     // ¡
    { KEY_N,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc2, 0xab, 0, 0} },     // «
    { KEY_M,         k230_FormattedLayer_FORMATTED_LAYER_SYM, {0xc2, 0xbb, 0, 0} },     // »
    { KEY_SPACE,     k230_FormattedLayer_FORMATTED_LAYER_SYM, {' ', 0, 0, 0} },
    { KEY_MUTE,      k230_FormattedLayer_FORMATTED_LAYER_SYM, {'$', 0, 0, 0} },
}};

class ConsoleKeymapManager {
public:
    ConsoleKeymapManager()
        : state_dir_(get_state_directory()),
          keymap_path_(state_dir_ / kGeneratedKeymapFile) {
        build_entries_from_seeds();
        const bool rendered = write_keymap_file();
        keymap_applied_ = rendered && apply_keymap_file(keymap_path_);
    }

    std::optional<CustomKeymapEntry> find_entry(uint16_t base_keycode,
                                                k230_FormattedLayer layer) const {
        for (const auto& entry : installed_entries_) {
            if ((entry.base_keycode == base_keycode) && (entry.layer == layer)) {
                return entry;
            }
        }
        return std::nullopt;
    }

    bool is_keymap_applied() const {
        return keymap_applied_;
    }

private:
    void build_entries_from_seeds() {
        installed_entries_.reserve(kKeymapSeeds.size());
        uint16_t next_reserved = K230_RESERVED_KEYCODE_BASE;

        for (const auto& seed : kKeymapSeeds) {
            uint8_t reserved = 0;
            bool found = false;
            for (const auto& existing : installed_entries_) {
                if (existing.utf8 == seed.utf8) {
                    reserved = existing.reserved_keycode;
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (next_reserved > 0xff) {
                    fprintf(stderr,
                        "keymap: ran out of reserved keycode space at base=%u layer=%u\n",
                        seed.base_keycode, static_cast<unsigned>(seed.layer));
                    continue;
                }
                reserved = static_cast<uint8_t>(next_reserved++);
            }

            installed_entries_.push_back(CustomKeymapEntry{
                .base_keycode = static_cast<uint8_t>(seed.base_keycode),
                .layer = seed.layer,
                .reserved_keycode = reserved,
                .utf8 = seed.utf8,
            });
        }
    }

    std::string render_keymap_overlay() const {
        std::ostringstream out;
        // No `charset` directive: loadkeys's charset list is for 8-bit
        // codepages only; UTF-8 entries use the U+XXXX notation directly
        // and require the console to already be in K_UNICODE mode.
        out << "! Generated by cyberdeck-service from built-in keymap seeds.\n\n";

        std::vector<uint8_t> emitted;
        emitted.reserve(installed_entries_.size());
        for (const auto& entry : installed_entries_) {
            if (std::find(emitted.begin(), emitted.end(), entry.reserved_keycode)
                != emitted.end()) {
                continue;
            }
            const std::optional<uint32_t> codepoint = utf8_to_codepoint(entry.utf8);
            if (!codepoint.has_value()) {
                continue;
            }
            out << "keycode " << static_cast<unsigned>(entry.reserved_keycode)
                << " = U+"
                << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
                << *codepoint
                << std::setfill(' ') << std::dec << "\n";
            emitted.push_back(entry.reserved_keycode);
        }

        return out.str();
    }

    bool write_keymap_file() {
        std::error_code ec;
        std::filesystem::create_directories(state_dir_, ec);
        if (ec) {
            fprintf(stderr, "keymap: failed to create state dir %s: %s\n",
                state_dir_.c_str(), ec.message().c_str());
            return false;
        }

        std::ofstream output(keymap_path_, std::ios::trunc);
        if (!output) {
            fprintf(stderr, "keymap: failed to open %s for write\n",
                keymap_path_.c_str());
            return false;
        }

        output << render_keymap_overlay();
        output.close();
        if (!output) {
            fprintf(stderr, "keymap: failed to write %s\n", keymap_path_.c_str());
            return false;
        }

        fprintf(stderr, "keymap: rendered %zu entries to %s\n",
            installed_entries_.size(), keymap_path_.c_str());
        return true;
    }

    bool apply_keymap_file(const std::filesystem::path& path) const {
        fprintf(stderr, "keymap: applying %s to %s\n", path.c_str(), kConsoleDevice);
        return run_command({
            "loadkeys",
            "-q",
            "-C",
            kConsoleDevice,
            path.string(),
        });
    }

    std::filesystem::path state_dir_;
    std::filesystem::path keymap_path_;
    std::vector<CustomKeymapEntry> installed_entries_;
    bool keymap_applied_ = false;
};

static std::optional<ReplayKeypress> ascii_to_replay_keypress(uint8_t ch) {
    if (ch >= 'a' && ch <= 'z') {
        return ReplayKeypress{
            .keycode = static_cast<uint16_t>(KEY_A + (ch - 'a')),
            .shift = false,
        };
    }

    if (ch >= 'A' && ch <= 'Z') {
        return ReplayKeypress{
            .keycode = static_cast<uint16_t>(KEY_A + (ch - 'A')),
            .shift = true,
        };
    }

    if (ch >= '1' && ch <= '9') {
        return ReplayKeypress{
            .keycode = static_cast<uint16_t>(KEY_1 + (ch - '1')),
            .shift = false,
        };
    }

    if (ch == '0') {
        return ReplayKeypress{ .keycode = KEY_0, .shift = false };
    }

    switch (ch) {
        case ' ':
            return ReplayKeypress{ .keycode = KEY_SPACE, .shift = false };
        case '\t':
            return ReplayKeypress{ .keycode = KEY_TAB, .shift = false };
        case '\n':
            return ReplayKeypress{ .keycode = KEY_ENTER, .shift = false };
        case '`':
            return ReplayKeypress{ .keycode = KEY_GRAVE, .shift = false };
        case '~':
            return ReplayKeypress{ .keycode = KEY_GRAVE, .shift = true };
        case '-':
            return ReplayKeypress{ .keycode = KEY_MINUS, .shift = false };
        case '_':
            return ReplayKeypress{ .keycode = KEY_MINUS, .shift = true };
        case '=':
            return ReplayKeypress{ .keycode = KEY_EQUAL, .shift = false };
        case '+':
            return ReplayKeypress{ .keycode = KEY_EQUAL, .shift = true };
        case '[':
            return ReplayKeypress{ .keycode = KEY_LEFTBRACE, .shift = false };
        case '{':
            return ReplayKeypress{ .keycode = KEY_LEFTBRACE, .shift = true };
        case ']':
            return ReplayKeypress{ .keycode = KEY_RIGHTBRACE, .shift = false };
        case '}':
            return ReplayKeypress{ .keycode = KEY_RIGHTBRACE, .shift = true };
        case '\\':
            return ReplayKeypress{ .keycode = KEY_BACKSLASH, .shift = false };
        case '|':
            return ReplayKeypress{ .keycode = KEY_BACKSLASH, .shift = true };
        case ';':
            return ReplayKeypress{ .keycode = KEY_SEMICOLON, .shift = false };
        case ':':
            return ReplayKeypress{ .keycode = KEY_SEMICOLON, .shift = true };
        case '\'':
            return ReplayKeypress{ .keycode = KEY_APOSTROPHE, .shift = false };
        case '"':
            return ReplayKeypress{ .keycode = KEY_APOSTROPHE, .shift = true };
        case ',':
            return ReplayKeypress{ .keycode = KEY_COMMA, .shift = false };
        case '<':
            return ReplayKeypress{ .keycode = KEY_COMMA, .shift = true };
        case '.':
            return ReplayKeypress{ .keycode = KEY_DOT, .shift = false };
        case '>':
            return ReplayKeypress{ .keycode = KEY_DOT, .shift = true };
        case '/':
            return ReplayKeypress{ .keycode = KEY_SLASH, .shift = false };
        case '?':
            return ReplayKeypress{ .keycode = KEY_SLASH, .shift = true };
        case '!':
            return ReplayKeypress{ .keycode = KEY_1, .shift = true };
        case '@':
            return ReplayKeypress{ .keycode = KEY_2, .shift = true };
        case '#':
            return ReplayKeypress{ .keycode = KEY_3, .shift = true };
        case '$':
            return ReplayKeypress{ .keycode = KEY_4, .shift = true };
        case '%':
            return ReplayKeypress{ .keycode = KEY_5, .shift = true };
        case '^':
            return ReplayKeypress{ .keycode = KEY_6, .shift = true };
        case '&':
            return ReplayKeypress{ .keycode = KEY_7, .shift = true };
        case '*':
            return ReplayKeypress{ .keycode = KEY_8, .shift = true };
        case '(':
            return ReplayKeypress{ .keycode = KEY_9, .shift = true };
        case ')':
            return ReplayKeypress{ .keycode = KEY_0, .shift = true };
        default:
            return std::nullopt;
    }
}

static bool emit_utf8_entry_as_standard_key(UInputDevice& uinput, const CustomKeymapEntry& entry) {
    const auto codepoint = utf8_to_codepoint(entry.utf8);
    if (!codepoint.has_value() || (*codepoint > 0x7f)) {
        return false;
    }

    const auto replay = ascii_to_replay_keypress(static_cast<uint8_t>(*codepoint));
    if (!replay.has_value()) {
        return false;
    }

    std::vector<uint16_t> modifiers;
    if (replay->shift) {
        modifiers.push_back(KEY_LEFTSHIFT);
    }
    uinput.emit_chord(modifiers, replay->keycode);
    return true;
}

static bool send_k230_to_nrf(cyberdeck::UartJniBridge& bridge, const k230_K230ToNrf& msg) {
    uint8_t buf[k230_K230ToNrf_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, k230_K230ToNrf_fields, &msg)) {
        fprintf(stderr, "Failed to encode K230ToNrf message\n");
        return false;
    }

    std::vector<uint8_t> data(buf, buf + stream.bytes_written);
    if (!bridge.send_message_blocking(K230_ENDPOINT, data)) {
        fprintf(stderr, "Failed to send K230ToNrf message\n");
        return false;
    }

    return true;
}

static void send_disconnect(cyberdeck::UartJniBridge& bridge) {
    k230_K230ToNrf msg = k230_K230ToNrf_init_zero;
    msg.which_msg = k230_K230ToNrf_disconnect_tag;
    if (send_k230_to_nrf(bridge, msg)) {
        fprintf(stderr, "Disconnect message written to UART\n");
    } else {
        fprintf(stderr, "Failed to send Disconnect (timeout or error)\n");
    }
}

static bool emit_formatted_special_key(UInputDevice& uinput, const k230_FormattedKeypress& keypress) {
    if (keypress.control || keypress.alt) {
        return false;
    }

    if ((keypress.layer != k230_FormattedLayer_FORMATTED_LAYER_PHYS_ALT) &&
        (keypress.layer != k230_FormattedLayer_FORMATTED_LAYER_SYM)) {
        return false;
    }

    switch (keypress.base_keycode) {
        case KEY_ENTER:
            uinput.emit_keypress(KEY_TAB);
            return true;
        case KEY_BACKSPACE:
            uinput.emit_keypress(KEY_DELETE);
            return true;
        case KEY_SPACE:
        case KEY_0:
            uinput.emit_keypress(static_cast<uint16_t>(keypress.base_keycode));
            return true;
        default:
            return false;
    }
}

static bool emit_formatted_keypress(UInputDevice& uinput, const ConsoleKeymapManager& keymaps,
                                    const k230_FormattedKeypress& keypress) {
    if (const auto entry = keymaps.find_entry(
            static_cast<uint16_t>(keypress.base_keycode), keypress.layer);
        entry.has_value()) {
        if (keypress.control || keypress.alt) {
            fprintf(stderr,
                "Formatted keypress unsupported with control/alt custom mapping: base=%u layer=%u\n",
                keypress.base_keycode, static_cast<unsigned>(keypress.layer));
            return false;
        }

        if (emit_utf8_entry_as_standard_key(uinput, *entry)) {
            return true;
        }

        if (keymaps.is_keymap_applied()) {
            uinput.emit_keypress(entry->reserved_keycode);
            return true;
        }

        fprintf(stderr,
            "Formatted keypress requires console keymap: base=%u layer=%u reserved=%u\n",
            keypress.base_keycode, static_cast<unsigned>(keypress.layer),
            entry->reserved_keycode);
        return false;
    }

    if (emit_formatted_special_key(uinput, keypress)) {
        return true;
    }

    if ((keypress.layer != k230_FormattedLayer_FORMATTED_LAYER_NONE) &&
        (keypress.layer != k230_FormattedLayer_FORMATTED_LAYER_SHIFT)) {
        fprintf(stderr, "Formatted keypress unsupported without keymap entry: base=%u layer=%u\n",
            keypress.base_keycode, static_cast<unsigned>(keypress.layer));
        return false;
    }

    std::vector<uint16_t> modifiers;
    if (keypress.control) {
        modifiers.push_back(KEY_LEFTCTRL);
    }
    if (keypress.alt) {
        modifiers.push_back(KEY_LEFTALT);
    }
    if (keypress.layer == k230_FormattedLayer_FORMATTED_LAYER_SHIFT) {
        modifiers.push_back(KEY_LEFTSHIFT);
    }

    uinput.emit_chord(modifiers, static_cast<uint16_t>(keypress.base_keycode));
    return true;
}

// Signal mask for SIGINT/SIGTERM — used with sigtimedwait()
static sigset_t g_sigmask;

static bool check_signal() {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
    int sig = sigtimedwait(&g_sigmask, nullptr, &ts);
    if (sig > 0) {
        g_stop = 1;
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    Opts opts = parse_argv(argc, argv);

    sigemptyset(&g_sigmask);
    sigaddset(&g_sigmask, SIGINT);
    sigaddset(&g_sigmask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &g_sigmask, nullptr);

    IoctlSetup ioctl_setup = opts.test_mode ? IoctlSetup::Skip : IoctlSetup::Run;
    UInputDevice uinput(opts.uinput_path, ioctl_setup);
    DisplayDevice display(opts.display_path);
    ConsoleKeymapManager keymaps;

    fprintf(stderr, "Creating JNI bridge (JAR: %s)...\n", opts.jar_path);
    cyberdeck::UartJniBridge bridge(opts.jar_path);

    std::atomic_bool connected{false};

    bridge.set_endpoint_callback([&uinput, &display, &keymaps](
        uint16_t endpoint, const std::vector<uint8_t>& data) {
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
                const auto& kb = msg.msg.keyboard;
                fprintf(stderr, "Keyboard %u %s\n", kb.keycode,
                    kb.is_down ? "down" : "up");
                uinput.emit_key(static_cast<uint16_t>(kb.keycode),
                    kb.is_down ? 1 : 0);
                uinput.emit_sync();
                break;
            }

            case k230_NrfToK230_formatted_keypress_tag: {
                const auto& keypress = msg.msg.formatted_keypress;
                fprintf(stderr, "Formatted base=%u layer=%u ctrl=%u alt=%u\n",
                    keypress.base_keycode, static_cast<unsigned>(keypress.layer),
                    keypress.control, keypress.alt);
                if (!emit_formatted_keypress(uinput, keymaps, keypress)) {
                    fprintf(stderr, "Formatted keypress dropped\n");
                }
                break;
            }

            case k230_NrfToK230_touchpad_tag: {
                const auto& tp = msg.msg.touchpad;
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
        connected.store(true);
        fprintf(stderr, "PPoG session opened, ready\n");
    });

    bridge.set_disconnected_callback([&connected]() {
        connected.store(false);
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

    while (!connected.load() && !g_stop) {
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
