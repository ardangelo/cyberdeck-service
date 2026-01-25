#pragma once

#include <variant>

#define MAGIC0 0xA5
#define MAGIC1 0x5A
#define PROTO_VERSION 1

#define MSG_STARTUP 0x01
#define MSG_STARTUP_ACK 0x03
#define MSG_SHUTDOWN 0x03
#define MSG_KEY_EVENT 0x10

//! Formatted key event
#define FORMATTED_KEY_BB_CALL 0x02
#define FORMATTED_KEY_BB_BERRY 0x05
#define FORMATTED_KEY_BB_BACK 0x18
#define FORMATTED_KEY_BB_POWER 0x03
typedef struct __attribute__((__packed__)) RawKeyboardEvent {
    uint8_t c;
    bool is_down;
} RawKeyboardEvent;

using Event = std::variant<RawKeyboardEvent>;

inline static uint32_t crc32_ieee(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

inline static void append_u32_le(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}