#include "fakes/SD.h"
#include "../src/audio_file_source_sd_single_block.h"

#include <cassert>

FakeSD SD;

int main() {
  uint8_t data[1600];
  {
    AudioFileSourceSdSingleBlock source("/track.mp3");
    assert(source.isOpen());
    assert(SD.state.bufferSize == 512);
    assert(source.read(data, sizeof(data)) == 1300);
    assert((SD.state.reads == std::vector<uint32_t>{512, 512, 512}));
  }

  SD.state = FakeSdState{};
  {
    AudioFileSourceSdSingleBlock source("/track.mp3", false);
    assert(SD.state.bufferSize == 0);
    assert(source.read(data, sizeof(data)) == 1300);
    assert((SD.state.reads == std::vector<uint32_t>{1600}));
  }
}
