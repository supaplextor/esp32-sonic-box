#include "../src/id3_header.h"

#include <cassert>

int main() {
  uint32_t offset = 123;
  const uint8_t mp3[] = {0xff, 0xfb, 0x90, 0x00};
  assert(id3AudioOffset(mp3, sizeof(mp3), sizeof(mp3), offset) == Id3HeaderResult::NoTag);
  assert(offset == 0);

  const uint8_t id3v23[] = {'I', 'D', '3', 3, 0, 0, 0, 0, 1, 0};
  assert(id3AudioOffset(id3v23, sizeof(id3v23), 500, offset) == Id3HeaderResult::Valid);
  assert(offset == 138);

  const uint8_t id3v24Footer[] = {'I', 'D', '3', 4, 0, 0x10, 0, 0, 1, 0};
  assert(id3AudioOffset(id3v24Footer, sizeof(id3v24Footer), 500, offset) ==
         Id3HeaderResult::Valid);
  assert(offset == 148);
  assert(id3AudioOffset(id3v24Footer, sizeof(id3v24Footer), 148, offset) ==
         Id3HeaderResult::Invalid);
  assert(id3AudioOffset(id3v24Footer, 5, 500, offset) == Id3HeaderResult::Invalid);

  const uint8_t invalidSize[] = {'I', 'D', '3', 4, 0, 0, 0, 0x80, 0, 0};
  assert(id3AudioOffset(invalidSize, sizeof(invalidSize), 500, offset) ==
         Id3HeaderResult::Invalid);
}
