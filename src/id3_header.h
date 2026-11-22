#pragma once

#include <stddef.h>
#include <stdint.h>

enum class Id3HeaderResult {
  NoTag,
  Valid,
  Invalid,
};

inline Id3HeaderResult id3AudioOffset(const uint8_t* header, size_t length,
                                      uint32_t fileSize, uint32_t& offset) {
  offset = 0;
  if (length < 3 || header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
    return Id3HeaderResult::NoTag;
  }
  if (length < 10 || header[3] < 2 || header[3] > 4 || header[4] == 0xff ||
      ((header[6] | header[7] | header[8] | header[9]) & 0x80) != 0) {
    return Id3HeaderResult::Invalid;
  }

  const uint32_t tagSize = (static_cast<uint32_t>(header[6]) << 21) |
                           (static_cast<uint32_t>(header[7]) << 14) |
                           (static_cast<uint32_t>(header[8]) << 7) |
                           header[9];
  offset = 10 + tagSize + ((header[3] == 4 && (header[5] & 0x10)) ? 10 : 0);
  return offset < fileSize ? Id3HeaderResult::Valid : Id3HeaderResult::Invalid;
}
