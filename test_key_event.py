#!/usr/bin/env python3
"""
Test harness for cyberdeck_daemon.

Starts fake_uinput, fake_uart, and cyberdeck_daemon, then verifies
that keyboard events flow through the system correctly.
"""

import os
import select
import signal
import subprocess
import sys
import time
from dataclasses import dataclass


@dataclass
class Config:
    fake_uinput: str = "./fake_uinput"
    fake_uart: str = "./fake_uart"
    daemon: str = "./cyberdeck_daemon"
    test_string: str = "abc"
    heartbeat_ms: int = 500
    timeout_startup: float = 5.0
    timeout_handshake: float = 10.0
    timeout_events: float = 10.0


def log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", file=sys.stderr, flush=True)


class StreamReader:
    """Non-blocking line reader for subprocess pipes."""
    
    def __init__(self, stream, name: str):
        self.stream = stream
        self.name = name
        self.buf = b""
        os.set_blocking(stream.fileno(), False)
    
    def fileno(self):
        return self.stream.fileno()
    
    def read_lines(self) -> list[str]:
        """Read available data and return complete lines."""
        lines = []
        try:
            chunk = self.stream.read(4096)
            if chunk:
                self.buf += chunk
        except BlockingIOError:
            pass
        
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            lines.append(line.decode("utf-8", errors="replace").rstrip("\r"))
        
        return lines


def main() -> int:
    cfg = Config(
        fake_uinput=os.environ.get("FAKE_UINPUT", "./fake_uinput"),
        fake_uart=os.environ.get("FAKE_UART", "./fake_uart"),
        daemon=os.environ.get("DAEMON", "./cyberdeck_daemon"),
        test_string=os.environ.get("TEST_STRING", "abc"),
    )
    
    procs: list[subprocess.Popen] = []
    
    try:
        # Start fake_uinput
        log(f"Starting: {cfg.fake_uinput}")
        fake_uinput = subprocess.Popen(
            [cfg.fake_uinput],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        procs.append(fake_uinput)
        uinput_stdout = StreamReader(fake_uinput.stdout, "uinput_stdout")
        uinput_stderr = StreamReader(fake_uinput.stderr, "uinput_stderr")
        
        # Wait for FIFO path
        fifo_path = None
        deadline = time.time() + cfg.timeout_startup
        while (time.time() < deadline) and (fifo_path is None):
            ready, _, _ = select.select([uinput_stdout.stream], [], [], 0.5)
            for line in uinput_stdout.read_lines():
                log(f"uinput stdout: {line}")
                if "FIFO created:" in line:
                    fifo_path = line.split("FIFO created:", 1)[1].strip()
        
        if not fifo_path:
            log("ERROR: Failed to get FIFO path from fake_uinput")
            return 1
        log(f"FIFO: {fifo_path}")
        
        # Start fake_uart with fast heartbeat
        log(f"Starting: {cfg.fake_uart} --heartbeat={cfg.heartbeat_ms}")
        fake_uart = subprocess.Popen(
            [cfg.fake_uart, f"--heartbeat={cfg.heartbeat_ms}"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        procs.append(fake_uart)
        uart_stdout = StreamReader(fake_uart.stdout, "uart_stdout")
        uart_stderr = StreamReader(fake_uart.stderr, "uart_stderr")
        
        # Read PTY path, first line on stdout
        pty_path = None
        deadline = time.time() + cfg.timeout_startup
        while (time.time() < deadline) and (pty_path is None):
            ready, _, _ = select.select([uart_stdout.stream], [], [], 0.5)
            for line in uart_stdout.read_lines():
                pty_path = line.strip()
                break
        
        if not pty_path:
            log("ERROR: Failed to get PTY path from fake_uart")
            return 1
        log(f"PTY: {pty_path}")
        
        # Start daemon in test mode
        log(f"Starting: {cfg.daemon}")
        daemon = subprocess.Popen(
            [cfg.daemon, f"--uart={pty_path}", f"--uinput={fifo_path}", "--test"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        procs.append(daemon)
        daemon_stdout = StreamReader(daemon.stdout, "daemon_stdout")
        daemon_stderr = StreamReader(daemon.stderr, "daemon_stderr")
        
        # Wait for handshake, polling all streams
        log("Waiting for handshake...")
        connected = False
        deadline = time.time() + cfg.timeout_handshake
        all_readers = [uart_stderr, daemon_stderr, uinput_stdout]
        
        while (time.time() < deadline) and (not connected):
            ready, _, _ = select.select(
                [r.stream for r in all_readers], [], [], 0.1)
            
            for reader in all_readers:
                for line in reader.read_lines():
                    log(f"{reader.name}: {line}")
                    if (reader.name == "uart_stderr") and ("connected!" in line):
                        connected = True
            
            if daemon.poll() is not None:
                log(f"ERROR: daemon exited with code {daemon.returncode}")
                return 1
        
        if not connected:
            log("ERROR: Handshake failed")
            return 1
        
        log("Handshake complete!")
        
        # Send test characters
        log(f"Sending test string: {cfg.test_string!r}")
        fake_uart.stdin.write(cfg.test_string.encode())
        fake_uart.stdin.flush()
        
        # Collect events until we have expected count or timeout
        expected_keys = len(cfg.test_string) * 2
        log(f"Waiting for {expected_keys} key events...")
        events = []
        deadline = time.time() + cfg.timeout_events
        
        while (time.time() < deadline) and (len(events) < expected_keys):
            ready, _, _ = select.select(
                [r.stream for r in all_readers], [], [], 0.1)
            
            for reader in all_readers:
                for line in reader.read_lines():
                    log(f"{reader.name}: {line}")
                    if (reader.name == "uinput_stdout") and ("EV_KEY" in line):
                        events.append(line)
        
        if len(events) >= expected_keys:
            log(f"SUCCESS: Got {len(events)} key events, expected {expected_keys}")
            return 0
        else:
            log(f"FAILURE: Got {len(events)} key events, expected {expected_keys}")
            return 1
        
    finally:
        for proc in reversed(procs):
            if proc.poll() is None:
                try:
                    proc.send_signal(signal.SIGINT)
                    proc.wait(timeout=1)
                except Exception:
                    proc.kill()


if __name__ == "__main__":
    sys.exit(main())
