// Host test for src/dx_call_match.h — the same header the firmware compiles.
//
// The bug this exists to prevent: comparing only the slash-delimited components
// of a callsign meant a watch pattern containing a slash could never match
// anything at all, silently. Entering a portable call exactly as the cluster
// spots it (YS/WE9G) was the one input guaranteed to fail.

#include "dx_call_match.h"

#include <iostream>
#include <string>

namespace {
int g_failures = 0;

void expect(const char* pattern, const char* call, bool want) {
  const bool got = dxCallMatchesPattern(pattern, call);
  const bool ok = got == want;
  std::cout << (ok ? "  PASS  " : "  FAIL  ") << "pattern " << pattern << " vs call " << call
            << " -> " << (got ? "match" : "no match") << (ok ? "" : "  (WRONG)") << "\n";
  if (!ok) {
    ++g_failures;
  }
}
}  // namespace

int main() {
  std::cout << "Plain patterns, in every shape a call gets spotted\n";
  expect("3Y0J", "3Y0J", true);
  expect("3Y0J", "3Y0J/MM", true);
  expect("3Y0J", "FT4/3Y0J", true);
  expect("3Y0J", "FT4/3Y0J/P", true);
  expect("3Y0J", "3Y0JX", false);
  expect("3Y0J", "X3Y0J", false);
  expect("3Y0J", "3Y0", false);

  std::cout << "\nPatterns that themselves contain a slash (the regression)\n";
  expect("YS/WE9G", "YS/WE9G", true);
  expect("YS/WE9G", "YS", false);
  expect("YS/WE9G", "WE9G", false);
  expect("VP2E/K5WE", "VP2E/K5WE", true);
  // A slash pattern is an exact call, so it must not match a further-suffixed one.
  expect("YS/WE9G", "YS/WE9G/P", false);

  std::cout << "\nPrefix hunting\n";
  expect("VP6*", "VP6D", true);
  expect("VP6*", "VP6", true);
  expect("VP6*", "VP7D", false);
  expect("V7*", "V73XY", true);
  expect("V7*", "KV73", false);
  expect("*", "ANY", false);  // a bare star has no stem and must not match everything

  std::cout << "\nDegenerate input\n";
  expect("", "3Y0J", false);
  expect("3Y0J", "", false);
  expect("/", "A/B", false);

  std::cout << "\n";
  if (g_failures) {
    std::cout << g_failures << " FAILURE(S)\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
