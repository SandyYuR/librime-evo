// 手排版本以 2026-01-01 为起点，用 36 进制缩短存储；0 表示未手排。
#ifndef RIME_DICT_MANUAL_STAMP_CODEC_H_
#define RIME_DICT_MANUAL_STAMP_CODEC_H_

#include <stdint.h>
#include <limits>
#include <string>

namespace rime {
namespace manual_stamp_codec {
constexpr uint64_t kEpochUnixSeconds = 1767225600ULL;

inline std::string Format(uint64_t value) {
  if (!value)
    return "0";
  char reversed[13];
  size_t size = 0;
  constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  do {
    reversed[size++] = kDigits[value % 36];
    value /= 36;
  } while (value);
  std::string result;
  result.reserve(size);
  while (size)
    result += reversed[--size];
  return result;
}

inline bool Parse(const std::string& raw, uint64_t* out) {
  if (!out || raw.empty() || raw.size() > 13 ||
      (raw.size() > 1 && raw[0] == '0'))
    return false;
  uint64_t value = 0;
  for (char ch : raw) {
    const unsigned digit = (ch >= '0' && ch <= '9')   ? ch - '0'
                           : (ch >= 'a' && ch <= 'z') ? ch - 'a' + 10
                                                      : 36;
    if (digit >= 36 ||
        value > (std::numeric_limits<uint64_t>::max() - digit) / 36)
      return false;
    value = value * 36 + digit;
  }
  *out = value;
  return true;
}

inline uint64_t FromUnixSeconds(uint64_t seconds) {
  if (seconds < kEpochUnixSeconds)
    return 1;
  return seconds - kEpochUnixSeconds + 1;
}
}  // namespace manual_stamp_codec
}  // namespace rime
#endif  // RIME_DICT_MANUAL_STAMP_CODEC_H_
