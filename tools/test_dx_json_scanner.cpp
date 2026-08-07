// Host test for src/dx_json_scanner.h — the same header the firmware compiles.
//
// The scanner exists so the DX JSON feed can be read a bounded slice per
// loop() iteration instead of being drained inside one call. The thing that
// has to be true for that to be safe is that suspending the scan between any
// two bytes changes nothing about what comes out. That is exactly what a host
// can prove against a captured feed, and it is why this fix did not need a
// board to land.
//
// Build and run:
//   c++ -std=c++17 -I src -o /tmp/test_dx_json_scanner tools/test_dx_json_scanner.cpp
//   /tmp/test_dx_json_scanner tools/testdata/iz3mez_spots.json

#include "dx_json_scanner.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(const std::string& what, bool ok, const std::string& detail = "") {
  std::cout << (ok ? "  PASS  " : "  FAIL  ") << what;
  if (!detail.empty()) {
    std::cout << "  [" << detail << "]";
  }
  std::cout << "\n";
  if (!ok) {
    ++g_failures;
  }
}

struct ScanResult {
  std::vector<std::string> objects;
  std::vector<bool> overflowed;
  bool sawArrayEnd = false;
  size_t maxBytesInOnePump = 0;
  size_t pumps = 0;
};

// Drives the scanner the way dx_spots.cpp's pumpDxJsonScan does: at most
// `budget` characters per call, resuming wherever the previous call stopped.
template <size_t kCapacity>
ScanResult scanInSlices(const std::string& feed, size_t budget) {
  DxJsonObjectScanner<kCapacity> scanner;
  ScanResult result;
  size_t position = 0;

  while (position < feed.size() && !result.sawArrayEnd) {
    size_t consumed = 0;
    ++result.pumps;
    while (consumed < budget && position < feed.size()) {
      const auto event = scanner.feed(feed[position++]);
      ++consumed;
      if (event == scanner.kArrayEnd) {
        result.sawArrayEnd = true;
        break;
      }
      if (event == scanner.kObjectReady) {
        result.objects.emplace_back(scanner.object(), scanner.objectLength());
        result.overflowed.push_back(scanner.objectOverflowed());
      }
    }
    if (consumed > result.maxBytesInOnePump) {
      result.maxBytesInOnePump = consumed;
    }
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <feed.json> [--dump]\n";
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << argv[1] << "\n";
    return 2;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string feed = ss.str();
  const bool dump = argc > 2 && std::string(argv[2]) == "--dump";

  // The capacity the firmware uses (kMaxDxObjectChars).
  constexpr size_t kCapacity = 1536;

  const ScanResult whole = scanInSlices<kCapacity>(feed, feed.size());

  if (dump) {
    // NUL-separated, not newline-separated: real feeds are pretty-printed, so
    // the objects themselves contain newlines.
    for (const auto& object : whole.objects) {
      std::cout << object << '\0';
    }
    return 0;
  }

  std::cout << "feed: " << argv[1] << ", " << feed.size() << " bytes\n\n";

  std::cout << "Reading the whole feed in one go\n";
  check("the array's end is reached", whole.sawArrayEnd);
  check("objects are found", !whole.objects.empty(),
        std::to_string(whole.objects.size()) + " objects");
  bool anyOverflow = false;
  for (bool o : whole.overflowed) {
    anyOverflow = anyOverflow || o;
  }
  check("no object overflows the firmware's 1536-byte buffer", !anyOverflow);

  // The property the whole restructure rests on: where the reads are cut makes
  // no difference to what the scan produces.
  std::cout << "\nSuspending and resuming at every slice size\n";
  const std::vector<size_t> budgets = {1,   2,   3,    5,    7,    13,   64,
                                       127, 512, 1024, 2048, 4096, 60000};
  for (size_t budget : budgets) {
    const ScanResult sliced = scanInSlices<kCapacity>(feed, budget);
    const bool identical = sliced.objects == whole.objects &&
                           sliced.overflowed == whole.overflowed &&
                           sliced.sawArrayEnd == whole.sawArrayEnd;
    check("slice size " + std::to_string(budget) + " gives identical output", identical,
          std::to_string(sliced.objects.size()) + " objects over " +
              std::to_string(sliced.pumps) + " pumps");
  }

  std::cout << "\nThe read budget is actually respected\n";
  for (size_t budget : {size_t(1), size_t(64), size_t(2048)}) {
    const ScanResult sliced = scanInSlices<kCapacity>(feed, budget);
    check("never reads more than " + std::to_string(budget) + " bytes in one pump",
          sliced.maxBytesInOnePump <= budget,
          "max " + std::to_string(sliced.maxBytesInOnePump));
  }

  // Object boundaries are tracked separately from the buffer, so a body too
  // long to keep must still leave the scan aligned for the next object.
  std::cout << "\nOversized objects still resync\n";
  const ScanResult tiny = scanInSlices<64>(feed, 7);
  bool allOverflowed = !tiny.overflowed.empty();
  for (bool o : tiny.overflowed) {
    allOverflowed = allOverflowed && o;
  }
  check("a 64-byte buffer overflows on every object of this feed", allOverflowed);
  check("and still finds exactly the same number of objects",
        tiny.objects.size() == whole.objects.size(),
        std::to_string(tiny.objects.size()) + " vs " + std::to_string(whole.objects.size()));
  check("and still reaches the end of the array", tiny.sawArrayEnd);

  // Braces and brackets inside strings must not move the depth, and an escaped
  // quote must not end one. Split one byte at a time so every state is
  // suspended at least once.
  std::cout << "\nStrings that look like structure\n";
  const std::string tricky =
      R"([{"a":"}{][","b":"say \"hi\"","c":"trailing backslash \\","d":{"n":[1,2]}},)"
      R"({"e":"plain"}])";
  const ScanResult trickyResult = scanInSlices<kCapacity>(tricky, 1);
  check("two objects found", trickyResult.objects.size() == 2,
        std::to_string(trickyResult.objects.size()));
  check("the first object keeps its braces and escapes intact",
        trickyResult.objects.size() == 2 &&
            trickyResult.objects[0] ==
                R"({"a":"}{][","b":"say \"hi\"","c":"trailing backslash \\","d":{"n":[1,2]}})",
        trickyResult.objects.empty() ? "" : trickyResult.objects[0]);
  check("the second object is clean", trickyResult.objects.size() == 2 &&
                                          trickyResult.objects[1] == R"({"e":"plain"})");
  check("the array end is seen after them", trickyResult.sawArrayEnd);

  std::cout << "\n";
  if (g_failures) {
    std::cout << g_failures << " FAILURE(S)\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
