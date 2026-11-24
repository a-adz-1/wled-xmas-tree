// tree_geometry.h - maps every LED of a WLED segment to a position on a cone-shaped tree.
//
// This file has no WLED dependencies so it can be unit tested on a PC.
//
// Every LED gets:
//   h    - height on the tree, 0 = base, 255 = top
//   a    - angle around the tree, 0..255 = one full turn
//   kind - KIND_DARK (not part of the tree, e.g. the bend at a turn), KIND_TREE or KIND_STAR
//   tag  - for tree LEDs: the string ("run") number it belongs to (0..255)
//          for star LEDs: position within the star, scaled to 0..255
//
// Effects are written in terms of (h, a) so they look the same on any tree layout.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdlib.h>

namespace xmastree {

enum Layout : uint8_t {
  LAYOUT_STRINGS = 0,   // straight strings from base to top: zig-zag single strip, multi-channel, or all-up
  LAYOUT_SPIRAL  = 1,   // one strip wound around a cone
  LAYOUT_CUSTOM  = 2    // positions loaded from a CSV file (lights on a real tree)
};

enum StarPosition : uint8_t {
  STAR_NONE  = 0,
  STAR_END   = 1,       // star LEDs come after the tree LEDs (strip finishes at the top)
  STAR_START = 2        // star LEDs come first, then the tree
};

enum Kind : uint8_t {
  KIND_DARK = 0,
  KIND_TREE = 1,
  KIND_STAR = 2
};

struct TreePixel {
  uint8_t h;
  uint8_t a;
  uint8_t kind;
  uint8_t tag;
};

struct TreeConfig {
  uint8_t  layout       = LAYOUT_STRINGS;
  uint16_t offset       = 0;      // unused LEDs at the start of the segment, before the tree (or star) begins

  // LAYOUT_STRINGS
  uint16_t runs         = 8;      // number of vertical strings
  uint16_t ledsPerRun   = 50;     // LEDs on each string
  bool     serpentine   = true;   // true: strings alternate up/down (zig-zag). false: every string runs the same way
  bool     firstRunUp   = true;   // true: the first string starts at the base and goes up
  uint8_t  turnGap      = 0;      // dark LEDs used up by each turn between strings

  // LAYOUT_SPIRAL
  uint16_t spiralLeds   = 300;    // LEDs in the spiral
  uint16_t spiralTurns10 = 80;    // turns around the tree x10 (80 = 8 turns)
  bool     spiralFromTop = false; // true: the strip starts at the top and winds down

  // all layouts
  bool     clockwise    = false;  // direction strings/spiral advance around the tree, seen from above
  uint16_t starLeds     = 0;
  uint8_t  starPos      = STAR_END;
};

// Number of LEDs the strings/spiral part of the tree uses (including turn gaps, excluding star/offset).
inline uint32_t treeBodyLength(const TreeConfig& c) {
  switch (c.layout) {
    case LAYOUT_STRINGS:
      if (c.runs == 0 || c.ledsPerRun == 0) return 0;
      return (uint32_t)c.runs * c.ledsPerRun + (uint32_t)(c.runs - 1) * c.turnGap;
    case LAYOUT_SPIRAL:
      return c.spiralLeds;
    default:
      return 0; // custom: defined by the file
  }
}

// Total LEDs the generated map covers (offset + star + body). Not used for LAYOUT_CUSTOM.
inline uint32_t totalLength(const TreeConfig& c) {
  uint32_t star = (c.starPos == STAR_NONE) ? 0 : c.starLeds;
  return c.offset + star + treeBodyLength(c);
}

inline uint8_t toByte(float v) {          // 0..1 -> 0..255 (clamped)
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 255;
  return (uint8_t)lroundf(v * 255.0f);
}

inline uint8_t angleToByte(float turns) { // any number of turns -> 0..255 (wrapped)
  float f = turns - floorf(turns);
  int v = (int)lroundf(f * 256.0f);
  return (uint8_t)(v & 0xFF);
}

// Fill the body (strings or spiral) starting at out[0]. Returns LEDs written.
inline uint32_t buildBody(const TreeConfig& c, TreePixel* out, uint32_t maxLen) {
  uint32_t n = 0;
  const float dir = c.clockwise ? -1.0f : 1.0f;

  if (c.layout == LAYOUT_STRINGS) {
    if (c.runs == 0 || c.ledsPerRun == 0) return 0;
    for (uint16_t r = 0; r < c.runs; r++) {
      bool up = c.firstRunUp;
      if (c.serpentine && (r & 1)) up = !up;
      // spread strings evenly around the tree; centre each string in its slice
      uint8_t a = angleToByte(dir * ((float)r + 0.5f) / (float)c.runs);
      for (uint16_t j = 0; j < c.ledsPerRun; j++) {
        if (n >= maxLen) return n;
        float pos = (c.ledsPerRun > 1) ? (float)j / (float)(c.ledsPerRun - 1) : 0.5f;
        float h = up ? pos : 1.0f - pos;
        out[n++] = { toByte(h), a, KIND_TREE, (uint8_t)(r & 0xFF) };
      }
      if (r + 1 < c.runs) {
        for (uint8_t g = 0; g < c.turnGap; g++) {
          if (n >= maxLen) return n;
          // gap LEDs sit at the bend: top or bottom of the tree, between the two strings
          bool atTop = up; // after an upward string the bend is at the top
          uint8_t ga = angleToByte(dir * ((float)r + 1.0f) / (float)c.runs);
          out[n++] = { (uint8_t)(atTop ? 255 : 0), ga, KIND_DARK, (uint8_t)(r & 0xFF) };
        }
      }
    }
    return n;
  }

  if (c.layout == LAYOUT_SPIRAL) {
    uint16_t len = c.spiralLeds;
    float turns = (float)c.spiralTurns10 / 10.0f;
    for (uint16_t k = 0; k < len; k++) {
      if (n >= maxLen) return n;
      float t = (len > 1) ? (float)k / (float)(len - 1) : 0.0f;
      // A strip wound with even vertical spacing on a cone: each turn is shorter near the top.
      // From the base, strip length up to height h is proportional to (2h - h^2)  ->  h = 1 - sqrt(1 - t)
      // From the top, strip length down to height h is proportional to (1 - h)^2 ->  h = 1 - sqrt(t)
      float h = c.spiralFromTop ? 1.0f - sqrtf(t) : 1.0f - sqrtf(1.0f - t);
      uint8_t a = angleToByte(dir * turns * h);
      // tag = which turn we are on (useful for "per ring" effects)
      uint8_t tag = (uint8_t)((int)(turns * h) & 0xFF);
      out[n++] = { toByte(h), a, KIND_TREE, tag };
    }
    return n;
  }
  return 0;
}

inline void buildStar(TreePixel* out, uint32_t count) {
  for (uint32_t s = 0; s < count; s++) {
    uint8_t pos = (count > 1) ? (uint8_t)((s * 255u) / (count - 1)) : 0;
    out[s] = { 255, (uint8_t)((s * 256u) / (count ? count : 1)), KIND_STAR, pos };
  }
}

// Build the full map for LAYOUT_STRINGS / LAYOUT_SPIRAL.
// `out` must hold at least maxLen entries. Returns the number of entries written.
inline uint32_t buildMap(const TreeConfig& c, TreePixel* out, uint32_t maxLen) {
  uint32_t n = 0;
  for (uint16_t i = 0; i < c.offset && n < maxLen; i++) out[n++] = { 0, 0, KIND_DARK, 0 };

  uint32_t star = (c.starPos == STAR_NONE) ? 0 : c.starLeds;
  if (c.starPos == STAR_START && star) {
    uint32_t s = (star < maxLen - n) ? star : maxLen - n;
    buildStar(out + n, s);
    n += s;
  }
  n += buildBody(c, out + n, maxLen - n);
  if (c.starPos == STAR_END && star && n < maxLen) {
    uint32_t s = (star < maxLen - n) ? star : maxLen - n;
    buildStar(out + n, s);
    n += s;
  }
  return n;
}

// ---------------------------------------------------------------------------------------------
// Custom layout: a CSV with one line per LED, in strip order.
//   x,y,z          3D position (any units, z = up). Angle and height are derived from it.
//   x,y,z,kind     kind: 0 = not on the tree, 1 = tree, 2 = star
// Lines starting with '#' and blank lines are ignored. A line "-" marks an unused LED.
// Usage: run pass 1 (observe) on every line, then finish(), then pass 2 (place) on every line.
// ---------------------------------------------------------------------------------------------
struct CsvMapper {
  float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f, minZ = 1e30f, maxZ = -1e30f;
  float cx = 0, cy = 0;
  uint32_t points = 0;

  // returns false for comments/blank lines (which do not count as an LED)
  static bool parse(const char* line, float& x, float& y, float& z, int& kind, bool& unused) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0 || *line == '\r' || *line == '\n' || *line == '#') return false;
    unused = false; kind = KIND_TREE;
    if (*line == '-' && (line[1] == 0 || line[1] == '\r' || line[1] == '\n' || line[1] == ',')) { unused = true; return true; }
    float v[4]; int got = 0;
    const char* p = line;
    while (got < 4) {
      char* end;
      float f = strtof(p, &end);
      if (end == p) break;
      v[got++] = f;
      p = end;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == ',' || *p == ';') p++; else break;
    }
    if (got < 3) { unused = true; return true; }   // malformed line still occupies an LED slot
    x = v[0]; y = v[1]; z = v[2];
    if (got == 4) kind = (int)v[3];
    if (kind == KIND_DARK) unused = true;
    return true;
  }

  void observe(const char* line) {
    float x, y, z; int kind; bool unused;
    if (!parse(line, x, y, z, kind, unused) || unused) return;
    minX = fminf(minX, x); maxX = fmaxf(maxX, x);
    minY = fminf(minY, y); maxY = fmaxf(maxY, y);
    minZ = fminf(minZ, z); maxZ = fmaxf(maxZ, z);
    points++;
  }

  void finish() { cx = (minX + maxX) * 0.5f; cy = (minY + maxY) * 0.5f; }

  // returns true if the line represents an LED (and fills px)
  bool place(const char* line, TreePixel& px) const {
    float x = 0, y = 0, z = 0; int kind; bool unused;
    if (!parse(line, x, y, z, kind, unused)) return false;
    if (unused || points == 0) { px = { 0, 0, KIND_DARK, 0 }; return true; }
    float hz = (maxZ > minZ) ? (z - minZ) / (maxZ - minZ) : 0.5f;
    float ang = atan2f(y - cy, x - cx) / (2.0f * (float)M_PI);   // -0.5..0.5 turns
    px.h = toByte(hz);
    px.a = angleToByte(ang);
    px.kind = (kind == KIND_STAR) ? KIND_STAR : KIND_TREE;
    px.tag = 0;
    return true;
  }
};

} // namespace xmastree
