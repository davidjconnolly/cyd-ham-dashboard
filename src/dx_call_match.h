#pragma once

// Matching a watched pattern against a spotted callsign.
//
// A callsign is compared whole AND by its slash-delimited components, so a
// plain pattern like 3Y0J matches 3Y0J, 3Y0J/MM and FT4/3Y0J alike. A trailing
// '*' is a prefix hunt (VP6*).
//
// Comparing only the components used to be the whole of it, which meant a
// pattern that itself contained a slash could never match anything: watching
// YS/WE9G split the incoming call into "YS" and "WE9G" and compared each
// against the full "YS/WE9G", so it silently never fired. Entering a portable
// call exactly as the cluster spots it is the obvious thing for someone to do,
// and it was the one input guaranteed not to work.
//
// Deliberately free of Arduino types so the host test compiles this same code
// rather than a copy of it. Both arguments are expected already trimmed and
// upper-cased by the caller.

#include <stddef.h>
#include <string.h>

// Returns true when `call` is matched by `pattern`.
inline bool dxCallMatchesPattern(const char* pattern, const char* call) {
  if (pattern == nullptr || call == nullptr) {
    return false;
  }
  const size_t patternLength = strlen(pattern);
  const size_t callLength = strlen(call);
  if (patternLength == 0 || callLength == 0) {
    return false;
  }

  if (pattern[patternLength - 1] == '*') {
    const size_t stem = patternLength - 1;
    return stem > 0 && callLength >= stem && strncmp(call, pattern, stem) == 0;
  }

  // The whole call first: the only thing a pattern containing a slash can match.
  if (patternLength == callLength && strcmp(call, pattern) == 0) {
    return true;
  }

  // Then each slash-delimited component, so a plain pattern still matches a
  // portable or prefixed callsign.
  size_t start = 0;
  while (start <= callLength) {
    const char* slash = strchr(call + start, '/');
    const size_t end = slash != nullptr ? static_cast<size_t>(slash - call) : callLength;
    if (end - start == patternLength && strncmp(call + start, pattern, patternLength) == 0) {
      return true;
    }
    if (slash == nullptr) {
      break;
    }
    start = end + 1;
  }
  return false;
}
