// fake_uart.cpp
// PTY-backed fake UART producer for msgpack-framed protocol.
// TTY-only target: emits RawKeyboardEvent (keycode byte + is_down).
//
// Behavior (interactive-friendly):
// - If stdin is a TTY, switch to NON-CANONICAL mode (byte-by-byte) but keep:
//     * ISIG  (Ctrl-C generates SIGINT)
//     * ECHO  (keypresses are echoed)
//     * OPOST/ONLCR (stdout newlines become CRLF for sane cursor behavior)
// - Ctrl-D (0x04) is treated as "end input" and exits the loop.
// - Ctrl-C kills/exits cleanly (SIGINT handler sets a stop flag).
//
// Reads bytes from stdin, and for each byte emits: key down, key up.
// Exits on stdin EOF (pipe/file) or Ctrl-D (tty).

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

#include <msgpack.hpp>

#include "event.hpp"

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int) {
  g_stop = 1;
}

// ---------------- Protocol packing ----------------
static std::vector<uint8_t> pack_raw_key_event_frame(const RawKeyboardEvent& ev)
{
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);

  pk.pack_map(1);
  pk.pack_int(2);
  pk.pack_bin(sizeof(RawKeyboardEvent));
  pk.pack_bin_body(reinterpret_cast<const char*>(&ev), sizeof(RawKeyboardEvent));

  uint32_t payload_len = static_cast<uint32_t>(sbuf.size());

  std::vector<uint8_t> frame;
  frame.reserve(2 + 1 + 1 + 4 + payload_len + 4);

  frame.push_back(MAGIC0);
  frame.push_back(MAGIC1);
  frame.push_back(PROTO_VERSION);
  frame.push_back(MSG_KEY_EVENT);

  append_u32_le(frame, payload_len);
  frame.insert(frame.end(),
               reinterpret_cast<const uint8_t*>(sbuf.data()),
               reinterpret_cast<const uint8_t*>(sbuf.data()) + sbuf.size());

  uint32_t crc = crc32_ieee(frame.data(), frame.size());
  append_u32_le(frame, crc);

  return frame;
}

// ---------------- PTY ----------------
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

static void send_raw_key(int out_fd, uint8_t c, bool is_down) {
  RawKeyboardEvent ev{};
  ev.c = c;
  ev.is_down = is_down;

  auto frame = pack_raw_key_event_frame(ev);
  write_all(out_fd, frame.data(), frame.size());
}

// Put the controlling terminal into byte-by-byte mode but keep echo/signals/newline behavior.
struct StdinNonCanonGuard {
  bool active{false};
  termios saved{};

  StdinNonCanonGuard() {
    if (!isatty(STDIN_FILENO))
      return;

    if (tcgetattr(STDIN_FILENO, &saved) != 0)
      return;

    termios t = saved;

    // Input: disable canonical line mode, but keep ISIG and ECHO as-is.
    t.c_lflag &= (tcflag_t)~ICANON;

    // Ensure Ctrl-C generates SIGINT, and keys echo.
    t.c_lflag |= ISIG;
    t.c_lflag |= ECHO;

    // Disable software flow control so Ctrl-S/Ctrl-Q don't freeze input unexpectedly.
    t.c_iflag &= (tcflag_t)~(IXON | IXOFF);

    // Make reads return as soon as 1 byte is available.
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
      active = true;

    // Also fix stdout newline presentation on the same TTY: enable OPOST and ONLCR.
    // (This avoids the "newline without carriage return" cursor behavior you saw.)
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
    if (active) {
      tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
  }
};

int main(int argc, char** argv)
{
  (void)argc;
  (void)argv;

  // Handle Ctrl-C reliably (even if read() is interrupted).
  struct sigaction sa{};
  sa.sa_handler = on_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, nullptr) != 0) {
    perror("sigaction(SIGINT)");
    return 1;
  }

  Pty p = create_pty();

  // Make interactive input byte-by-byte without breaking Ctrl-C, Ctrl-D, echo, CRLF.
  StdinNonCanonGuard stdin_guard;

  // Print the slave PTY path (use \r\n so it looks sane even if terminal is unusual).
  fprintf(stdout, "%s\r\n", p.slave_path.c_str());
  fflush(stdout);

  for (;;) {
    if (g_stop) break;

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

    // In non-canonical mode, Ctrl-D is delivered as a byte (0x04). Treat as "done".
    if (isatty(STDIN_FILENO) && b == 0x04) {
      break;
    }

    send_raw_key(p.master_fd, b, true);
    send_raw_key(p.master_fd, b, false);
  }

  return 0;
}
