#include "../src/track_page.h"

#include <cassert>
#include <string>
#include <vector>

struct FakeSource {
  std::vector<std::string> paths;
  size_t position = 0;
  size_t reads = 0;
  size_t resets = 0;

  bool reset() {
    position = 0;
    ++resets;
    return true;
  }

  bool next(std::string& path) {
    ++reads;
    if (position == paths.size()) {
      return false;
    }
    path = paths[position++];
    return true;
  }
};

int main() {
  FakeSource source;
  TrackPage<FakeSource, std::string, 7> page(source);
  assert(!page.begin());
  assert(page.count() == 0);
  assert(!page.previous());
  assert(!page.next());

  for (int i = 0; i < 17; ++i) {
    source.paths.push_back("track" + std::to_string(i));
  }
  source.reads = 0;
  assert(page.begin());
  assert(source.reads == 7);
  assert(page.page() == 0 && page.count() == 7);
  assert(page[0] == "track0" && page[6] == "track6");
  assert(!page.previous());

  size_t resets = source.resets;
  assert(page.next());
  assert(source.resets == resets);
  assert(page.page() == 1 && page.count() == 7);
  assert(page[0] == "track7" && page[6] == "track13");
  assert(page.next());
  assert(page.page() == 2 && page.count() == 3);
  assert(page[0] == "track14" && page[2] == "track16");
  assert(!page.next());
  assert(page.page() == 2 && page.count() == 3 && page[2] == "track16");
  assert(!page.next());
  assert(page.previous());
  assert(page.page() == 1 && page.count() == 7 && page[0] == "track7");
  assert(page.next());
  assert(page.page() == 2 && page.count() == 3 && page[0] == "track14");
  assert(page.previous());
  assert(page.previous());
  assert(page.page() == 0 && page.count() == 7 && page[6] == "track6");
  assert(!page.previous());

  source.paths.resize(7);
  assert(page.begin());
  assert(!page.next());
  assert(page.page() == 0 && page.count() == 7 && page[6] == "track6");
}
