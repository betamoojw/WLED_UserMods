#pragma once
#include <cstdint>
#include <cstring>

#define PROGMEM
#define PSTR(s) (s)

inline uint16_t pgm_read_word(const void* p) {
  uint16_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

inline uint32_t pgm_read_dword(const void* p) {
  uint32_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}
