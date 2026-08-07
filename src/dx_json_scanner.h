#pragma once

#include <stddef.h>
#include <stdint.h>

// Splits a JSON array into its top-level objects, one character at a time.
//
// Every bit of scan state lives in this struct, so a scan can be suspended
// between any two bytes of a stream and resumed on a later loop iteration
// without the caller holding anything back. That is what lets the DX JSON
// reader obey the per-iteration read budget the rest of this firmware's
// network code obeys, instead of draining a feed inside one loop() call.
//
// Deliberately free of Arduino types — no String, no Stream, nothing from
// the core. The firmware and the host test in tools/ compile the same header,
// so the resume behaviour can be exercised against captured feeds on a
// machine rather than only on a board.
//
// kCapacity is the largest object body kept. A longer one still has its
// boundaries tracked correctly; it is reported as overflowed and the caller
// drops it, which is what the old inline scanner did too.
template <size_t kCapacity>
class DxJsonObjectScanner {
 public:
  enum Event : uint8_t {
    kNone,         // still mid-object, or between objects
    kObjectReady,  // object() holds a complete top-level object
    kArrayEnd      // the array's closing bracket; the feed is done
  };

  DxJsonObjectScanner() { reset(); }

  void reset() {
    length_ = 0;
    buffer_[0] = '\0';
    foundArray_ = false;
    inObject_ = false;
    inString_ = false;
    escaped_ = false;
    overflow_ = false;
    depth_ = 0;
  }

  Event feed(char c) {
    if (!foundArray_) {
      if (c == '[') {
        foundArray_ = true;
      }
      return kNone;
    }

    if (!inObject_) {
      if (c == '{') {
        inObject_ = true;
        overflow_ = false;
        depth_ = 1;
        // Provably false already whenever the previous object closed properly,
        // but reset alongside the other per-object state rather than relying on
        // that: an object that starts from a known state cannot inherit a
        // desync from the one before it.
        inString_ = false;
        escaped_ = false;
        length_ = 0;
        append('{');
      } else if (c == ']') {
        return kArrayEnd;
      }
      return kNone;
    }

    append(c);

    if (inString_) {
      if (escaped_) {
        escaped_ = false;
      } else if (c == '\\') {
        escaped_ = true;
      } else if (c == '"') {
        inString_ = false;
      }
      return kNone;
    }

    if (c == '"') {
      inString_ = true;
    } else if (c == '{') {
      ++depth_;
    } else if (c == '}') {
      --depth_;
      if (depth_ == 0) {
        inObject_ = false;
        return kObjectReady;
      }
    }
    return kNone;
  }

  // Valid until the next feed() that starts a new object.
  const char* object() const { return buffer_; }
  size_t objectLength() const { return length_; }
  // True when the object was longer than kCapacity, so object() holds only its
  // start and must not be parsed.
  bool objectOverflowed() const { return overflow_; }

 private:
  void append(char c) {
    if (length_ < kCapacity) {
      buffer_[length_++] = c;
      buffer_[length_] = '\0';
    } else {
      overflow_ = true;
    }
  }

  char buffer_[kCapacity + 1];
  size_t length_;
  bool foundArray_;
  bool inObject_;
  bool inString_;
  bool escaped_;
  bool overflow_;
  int depth_;
};
