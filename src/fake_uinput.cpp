// fake_uinput.cpp
// FIFO-based sink that emulates the *write()* side of /dev/uinput for tests.
// It cannot emulate uinput ioctls. Your daemon must skip ioctls in test mode
// and write struct input_event records to this FIFO.
//
// Behavior change:
// - Stays running until killed (SIGINT / Ctrl-C).
// - On SIGINT: unlinks FIFO and exits.
// - If writer closes (EOF), it waits for the next writer instead of exiting.
//
// Usage:
//   ./fake_uinput [/path/to/fifo]

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/input.h>

static const char* g_fifo_path = nullptr;
static volatile sig_atomic_t g_stop = 0;

static void die_perror(const char* what) {
  perror(what);
  exit(1);
}

static void on_sigint(int) {
  g_stop = 1;
}

static const char* ev_type_name(uint16_t t) {
  switch (t) {
    case EV_SYN: return "EV_SYN";
    case EV_KEY: return "EV_KEY";
    case EV_REL: return "EV_REL";
    case EV_ABS: return "EV_ABS";
    case EV_MSC: return "EV_MSC";
    case EV_SW:  return "EV_SW";
    case EV_LED: return "EV_LED";
    case EV_SND: return "EV_SND";
    case EV_REP: return "EV_REP";
    case EV_FF:  return "EV_FF";
    case EV_PWR: return "EV_PWR";
    default:     return "EV_???";
  }
}

static const char* syn_code_name(uint16_t c) {
  switch (c) {
    case SYN_REPORT: return "SYN_REPORT";
    case SYN_CONFIG: return "SYN_CONFIG";
    case SYN_MT_REPORT: return "SYN_MT_REPORT";
    case SYN_DROPPED: return "SYN_DROPPED";
    default: return "SYN_???";
  }
}

static const char* key_value_name(int32_t v) {
  switch (v) {
    case 0: return "UP";
    case 1: return "DOWN";
    case 2: return "REPEAT";
    default: return "VAL_?";
  }
}

static ssize_t read_full(int fd, void* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = read(fd, (char*)buf + off, len - off);
    if (n == 0) return 0; // EOF
    if (n < 0) {
      if (errno == EINTR) {
        if (g_stop) return 0;
        continue;
      }
      return -1;
    }
    off += (size_t)n;
  }
  return (ssize_t)off;
}

static void cleanup_fifo() {
  if (g_fifo_path) {
    // Best-effort unlink; ignore errors.
    unlink(g_fifo_path);
  }
}

int main(int argc, char** argv) {
  const char* path = (argc >= 2) ? argv[1] : "/tmp/uinput-sink";
  g_fifo_path = path;

  // Install SIGINT handler
  struct sigaction sa{};
  sa.sa_handler = on_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, nullptr) != 0) die_perror("sigaction(SIGINT)");

  // Ensure FIFO is removed on normal exit path too
  atexit(cleanup_fifo);

  // Remove any existing node if it is a FIFO; otherwise refuse to clobber.
  struct stat st{};
  if (lstat(path, &st) == 0) {
    if (S_ISFIFO(st.st_mode)) {
      if (unlink(path) != 0) die_perror("unlink(existing fifo)");
    } else {
      fprintf(stderr, "Refusing to replace non-FIFO at %s\n", path);
      return 2;
    }
  } else if (errno != ENOENT) {
    die_perror("lstat");
  }

  if (mkfifo(path, 0600) != 0) die_perror("mkfifo");

  fprintf(stdout, "uinput sink FIFO created: %s\n", path);
  fprintf(stdout, "Configure daemon to open this path instead of /dev/uinput (test mode).\n");
  fprintf(stdout, "Expecting stream of struct input_event (%zu bytes each).\n\n",
          (size_t)sizeof(struct input_event));
  fflush(stdout);

  while (!g_stop) {
    // Open FIFO for reading. This blocks until a writer opens the FIFO.
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
      if (errno == EINTR && g_stop) break;
      die_perror("open(fifo for read)");
    }

    fprintf(stdout, "Writer connected. Reading events...\n");
    fflush(stdout);

    while (!g_stop) {
      struct input_event ev{};
      ssize_t n = read_full(fd, &ev, sizeof(ev));
      if (n == 0) {
        if (g_stop) break;
        fprintf(stdout, "EOF: writer closed. Waiting for next writer...\n");
        fflush(stdout);
        break; // break inner loop, reopen FIFO to wait for next writer
      }
      if (n < 0) {
        if (errno == EINTR && g_stop) break;
        die_perror("read");
      }

      long sec  = ev.time.tv_sec;
      long usec = ev.time.tv_usec;

      if (ev.type == EV_KEY) {
        fprintf(stdout,
                "[%ld.%06ld] EV_KEY code=%u value=%" PRId32 " (%s)\n",
                sec, usec, ev.code, ev.value, key_value_name(ev.value));
      } else if (ev.type == EV_SYN) {
        fprintf(stdout,
                "[%ld.%06ld] EV_SYN code=%u (%s) value=%" PRId32 "\n",
                sec, usec, ev.code, syn_code_name(ev.code), ev.value);
      } else {
        fprintf(stdout,
                "[%ld.%06ld] %s type=%u code=%u value=%" PRId32 "\n",
                sec, usec, ev_type_name(ev.type), ev.type, ev.code, ev.value);
      }
      fflush(stdout);
    }

    close(fd);
  }

  // On SIGINT (or normal exit), FIFO will be unlinked by atexit handler.
  return 0;
}
