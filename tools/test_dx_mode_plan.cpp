// Host test for src/dx_mode_plan.h — the same header the firmware compiles.
//
// Mode is never carried as data by any DX cluster source, so the firmware
// infers it from the frequency. The only honest way to check an inference is
// against spots where somebody stated the answer: this replays a captured feed
// and, for every spot whose comment names its mode, requires the band plan to
// agree. Spots the plan declines to classify are counted but not held against
// it — declining is the correct answer for the parts of a band where Region 1
// and Region 2 disagree.

#include "dx_mode_plan.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
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

void expectMode(double mhz, const char* want) {
  const char* got = dxmode::forFrequency(mhz);
  const std::string gotText = got ? got : "unknown";
  const std::string wantText = want ? want : "unknown";
  check(std::to_string(mhz) + " MHz -> " + wantText, gotText == wantText, gotText);
}

// The modes a spotter's comment can name, longest-first so FT8/FT4 win over a
// bare substring. Mirrors the order the firmware scans its mode options in.
const char* kModes[] = {"FT8", "FT4", "CW", "SSB", "USB", "LSB", "RTTY", "SSTV", "PSK"};

std::string upper(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }
  return s;
}

const char* statedMode(const std::string& comment) {
  const std::string up = upper(comment);
  for (const char* mode : kModes) {
    if (up.find(mode) != std::string::npos) {
      return mode;
    }
  }
  return nullptr;
}

// FT4 and FT8 share a family, and USB/LSB are both SSB: the band plan is not
// expected to tell those apart, only to get the family right.
std::string family(const std::string& mode) {
  if (mode == "USB" || mode == "LSB") return "SSB";
  if (mode == "FT4") return "FT8";
  return mode;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "tools/testdata";
  const std::string path = dir + "/dxsummit_spots.tsv";

  std::cout << "Known calling frequencies\n";
  expectMode(14.074, "FT8");
  expectMode(14.0765, "FT8");   // within the 5 kHz tolerance
  expectMode(144.174, "FT8");   // 2 m, absent from the old table
  expectMode(70.154, "FT8");    // 4 m, absent from the old table
  expectMode(50.313, "FT8");
  expectMode(24.915, "FT8");
  expectMode(10.136, "FT8");
  expectMode(21.140, "FT4");
  expectMode(28.180, "FT4");

  std::cout << "\nBands where FT8 and FT4 are closer together than the tolerance\n";
  // 17 m and 12 m put them 4 kHz apart, inside the 5 kHz window, so the nearest
  // frequency has to win rather than whichever is listed first.
  expectMode(18.100, "FT8");
  expectMode(18.104, "FT4");
  expectMode(24.915, "FT8");
  expectMode(24.919, "FT4");
  expectMode(24.9166, "FT8");  // between the two, nearer FT8
  expectMode(24.923, "FT4");   // outside FT8's window entirely
  expectMode(14.084, "FT4");

  std::cout << "\nFrequencies the old table got wrong\n";
  // 21.080/28.080 were listed as FT4 but carry no FT4 activity; they sit in the
  // digital segment, which this plan deliberately declines to name.
  expectMode(21.080, nullptr);
  expectMode(28.080, nullptr);

  std::cout << "\nCW and phone segments\n";
  expectMode(14.050, "CW");
  expectMode(14.250, "SSB");
  expectMode(7.010, "CW");
  expectMode(21.300, "SSB");
  expectMode(10.120, "CW");   // 30 m: CW, and no phone allocation anywhere

  std::cout << "\nSlices where Region 1 and Region 2 disagree stay unclassified\n";
  expectMode(7.100, nullptr);   // phone in R1, data in the US
  expectMode(7.080, nullptr);
  std::cout << "\nAnd the digital segment is never guessed at wholesale\n";
  expectMode(14.090, nullptr);  // RTTY/PSK/JS8 live here too

  std::cout << "\nAgainst a captured feed: " << path << "\n";
  std::ifstream in(path);
  if (!in) {
    std::cerr << "cannot open " << path << "\n";
    return 2;
  }

  int agree = 0, disagree = 0, declined = 0, total = 0, unknownAfter = 0;
  // Scored separately from the family metric below, which by design forgives
  // FT8/FT4 confusion — and therefore hid the case where every 18.104 and
  // 24.919 spot was reported as FT8.
  int digitalExact = 0, digitalWrong = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    ++total;
    const double mhz = std::atof(line.substr(0, tab).c_str()) / 1000.0;
    const std::string comment = line.substr(tab + 1);
    const char* stated = statedMode(comment);
    const char* planned = dxmode::forFrequency(mhz);

    if (stated == nullptr && planned == nullptr) {
      ++unknownAfter;
    }
    if (stated == nullptr) {
      continue;
    }
    const std::string statedText = stated;
    if (planned != nullptr && (statedText == "FT8" || statedText == "FT4") &&
        (std::string(planned) == "FT8" || std::string(planned) == "FT4")) {
      if (std::string(planned) == statedText) {
        ++digitalExact;
      } else {
        ++digitalWrong;
        std::cout << "        FT8/FT4 mixup: " << mhz << " MHz plan=" << planned
                  << " comment=" << stated << "\n";
      }
    }

    if (planned == nullptr) {
      ++declined;
    } else if (family(planned) == family(stated)) {
      ++agree;
    } else {
      ++disagree;
      std::cout << "        disagreement: " << mhz << " MHz plan=" << planned
                << " comment=" << stated << "  \"" << comment.substr(0, 40) << "\"\n";
    }
  }

  const int committed = agree + disagree;
  const double accuracy = committed ? (100.0 * agree / committed) : 0.0;
  std::cout << "  " << total << " spots, " << (agree + disagree + declined)
            << " with a mode stated by the spotter\n";
  std::cout << "  band plan committed on " << committed << ", declined " << declined << "\n";
  check("band plan agrees with the spotter at least 97% of the time", accuracy >= 97.0,
        std::to_string(accuracy) + "%");
  // Not 100%, and it cannot be: on 30 m, 17 m and 12 m the FT8 and FT4 calling
  // frequencies are 4 kHz apart while spotters round by a couple, so a spot
  // landing between them is genuinely undecidable from frequency alone. Both
  // residual cases in the captured feed have the mode in the comment, which the
  // firmware always prefers, so the device gets them right regardless. The bar
  // is here to catch a systematic failure — every 18.104 and 24.919 spot being
  // called FT8, which is what first-match ordering used to do.
  const int digitalTotal = digitalExact + digitalWrong;
  const double digitalAccuracy = digitalTotal ? (100.0 * digitalExact / digitalTotal) : 100.0;
  check("FT8 and FT4 are told apart at least 95% of the time", digitalAccuracy >= 95.0,
        std::to_string(digitalExact) + "/" + std::to_string(digitalTotal) + " = " +
            std::to_string(digitalAccuracy) + "%");
  // Guards the actual user-visible complaint: almost every spot showing Unknown.
  check("fewer than 15% of all spots are left with no mode at all",
        total > 0 && (100.0 * unknownAfter / total) < 15.0,
        std::to_string(unknownAfter) + " of " + std::to_string(total));

  std::cout << "\n";
  if (g_failures) {
    std::cout << g_failures << " FAILURE(S)\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
