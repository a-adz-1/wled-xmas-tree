// WLED Christmas Tree usermod
// https://github.com/a-adz-1/wled-xmas-tree
//
// Describe how your LEDs are arranged on the tree once (Config -> Usermods -> Xmas Tree),
// and the "Tree ..." effects will look right on any layout: a single zig-zag strip,
// several outputs going up and down, a spiral, or lights on a real tree mapped from a CSV.
//
// Effects map each LED of the segment to (height, angle) on the tree, so use ONE segment that
// covers the whole tree (and star), with Reverse, Mirror, Grouping and Spacing left at defaults.

#include "wled.h"
#include "tree_geometry.h"
#include "xmas_tree_page.h"

using namespace xmastree;

#define XT_MAX_LEDS   4096
#define XT_PIC_W      32
#define XT_PIC_H      48
#define XT_PIC_BYTES  (XT_PIC_W * XT_PIC_H * 3)
#define XT_CSV_FILE   "/xmastree.csv"
#define XT_PIC_FILE   "/xmastree.pic"

// ---------------------------------------------------------------------------------------------
// Shared state used by the effect functions (effects are plain functions, so this lives at file scope)
// ---------------------------------------------------------------------------------------------
namespace {

enum StarMode : uint8_t {
  STARMODE_FOLLOW = 0,   // star takes part in the effect (each effect decides what the star does)
  STARMODE_SOLID,        // star shows the star colour
  STARMODE_TWINKLE,      // star colour with sparkles
  STARMODE_PULSE,        // star colour, breathing
  STARMODE_RAINBOW,      // spinning rainbow
  STARMODE_OFF
};

struct TreeState {
  TreeConfig cfg;
  TreePixel* map      = nullptr;
  uint16_t   mapLen   = 0;
  uint8_t    aspect10 = 20;           // tree height / base width x10
  uint8_t    starMode = STARMODE_TWINKLE;
  uint32_t   starColor = 0xFFB000;    // warm gold
  uint8_t*   pic      = nullptr;      // XT_PIC_W x XT_PIC_H RGB, row 0 = top of the tree
  bool       picLoaded = false;
  uint16_t   treeLeds = 0;            // LEDs of KIND_TREE (for info)
  uint16_t   runs     = 1;            // number of distinct strings (for angular tolerance)
  const char* lastError = nullptr;
};
TreeState T;

// --- small helpers -------------------------------------------------------------------------

inline uint32_t hash32(uint32_t x) {          // fast integer hash (lowbias32)
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// Position of segment pixel i on the tree. Pixels outside the map (or with no map) fall back to
// "a single string going up the segment", so effects still do something sensible.
inline TreePixel treePx(unsigned i) {
  if (T.map && i < T.mapLen) return T.map[i];
  unsigned len = SEGLEN;
  uint8_t h = len > 1 ? (uint8_t)((i * 255u) / (len - 1)) : 0;
  return { h, 0, KIND_TREE, 0 };
}

inline uint32_t scaleColor(uint32_t c, uint8_t v) { return color_fade(c, v, false); }

// time scaled by a speed factor, without 32-bit overflow after a few hours
inline uint32_t ticks(uint32_t factor, uint8_t shift) { return (uint32_t)(((uint64_t)strip.now * factor) >> shift); }

inline uint8_t triwave(uint8_t x) { return x < 128 ? x * 2 : (255 - x) * 2; }

// distance between two tree points, in "tree heights" * 256  (horizontal part scaled by the cone radius)
inline uint16_t treeDistance(uint8_t h1, uint8_t a1, uint8_t h2, uint8_t a2) {
  int dh = (int)h1 - (int)h2;
  int da = (int8_t)(uint8_t)(a1 - a2);                         // -128..127, wraps around the tree
  // horizontal arc at mean height: (da/256 turns) * 2*pi * radius, radius = (1-h) * base/2, base = 1/aspect
  float hm = 1.0f - ((h1 + h2) * 0.5f) / 255.0f;
  float arc = (float)da * 6.2832f * hm * 0.5f / ((float)T.aspect10 / 10.0f);
  float d = sqrtf((float)(dh * dh) + arc * arc);
  return d > 65535.0f ? 65535 : (uint16_t)d;
}

// angular half-width that makes something "on" a string: half the gap between strings
inline uint8_t stringTolerance() {
  if (T.cfg.layout == LAYOUT_STRINGS && T.runs > 1) return (uint8_t)max(2, 128 / (int)T.runs);
  return 6;
}

uint32_t starColorFor(const TreePixel& p, uint32_t effectColor) {
  uint32_t now = strip.now;
  switch (T.starMode) {
    case STARMODE_FOLLOW:  return effectColor;
    case STARMODE_SOLID:   return T.starColor;
    case STARMODE_TWINKLE: {
      uint32_t r = hash32(p.tag * 7919u + (now / 90));
      if ((r & 0xFF) < 28) return color_blend(T.starColor, 0xFFFFFF, 200);
      return scaleColor(T.starColor, 200 + (r >> 8 & 0x37));
    }
    case STARMODE_PULSE:   return scaleColor(T.starColor, 60 + scale8(sin8_t(now / 12), 195));
    case STARMODE_RAINBOW: {
      CRGBW c = CHSV32((uint16_t)((p.tag + now / 8) << 8), 255, 255);
      return c.color32;
    }
    default: return BLACK;
  }
}

// Write pixel i, routing star LEDs through the star setting and blanking unused LEDs.
inline void putPx(unsigned i, const TreePixel& p, uint32_t c) {
  if (p.kind == KIND_DARK) c = BLACK;
  else if (p.kind == KIND_STAR) c = starColorFor(p, c);
  SEGMENT.setPixelColor(i, c);
}

inline uint32_t pal(uint8_t idx, uint8_t bri = 255) { return SEGMENT.color_from_palette(idx, false, true, 0, bri); }

// colour helper: palette if one is selected, else blend between colour slots 1 and 2
inline uint32_t palOrColors(uint8_t idx, bool usePalette) {
  if (usePalette) return pal(idx);
  return color_blend(SEGCOLOR(0), SEGCOLOR(1), triwave(idx));
}

void fallbackStatic() { SEGMENT.fill(SEGCOLOR(0)); }

// ---------------------------------------------------------------------------------------------
//  EFFECTS
// ---------------------------------------------------------------------------------------------

// Spiral stripes wrapping around the tree
void spiralCore(uint32_t c1, uint32_t c2, bool usePalette) {
  const unsigned len = SEGLEN;
  const uint8_t stripes = 1 + (SEGMENT.intensity >> 5);              // 1..8
  const float twist = (float)SEGMENT.custom1 / 32.0f;                // 0..8 turns from base to top
  const uint8_t width = 16 + (SEGMENT.custom2 * 7 >> 3);             // stripe duty
  const uint16_t t = ticks(SEGMENT.speed + 1, 7);
  const bool down = SEGMENT.check1;
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    float turns = (float)p.a / 256.0f + twist * (float)p.h / 255.0f;
    uint8_t phase = (uint8_t)((int)(turns * 256.0f * stripes) + (down ? t : -(int)t));
    uint32_t c;
    if (usePalette) {
      c = pal(phase);
    } else {
      // soft-edged stripe of colour 1 on colour 2
      int edge = (int)phase - (int)width;
      uint8_t mix = edge <= -12 ? 255 : (edge >= 12 ? 0 : (uint8_t)(255 - (edge + 12) * 255 / 24));
      c = color_blend(c2, c1, mix);
    }
    putPx(i, p, c);
  }
}

void mode_tree_spiral() {
  spiralCore(SEGCOLOR(0), SEGCOLOR(1), !SEGMENT.check2);
}
static const char _data_FX_TREE_SPIRAL[] PROGMEM =
  "Tree Spiral@!,Stripes,Twist,Width,,Downward,Two colours;!,!;!;;sx=80,ix=64,c1=96,c2=128,pal=11";

void mode_tree_candycane() {
  uint32_t c1 = SEGMENT.check2 ? SEGCOLOR(0) : 0xE00000;
  uint32_t c2 = SEGMENT.check2 ? SEGCOLOR(1) : 0xFFFFFF;
  spiralCore(c1, c2, false);
}
static const char _data_FX_TREE_CANDYCANE[] PROGMEM =
  "Tree Candy Cane@!,Stripes,Twist,Red width,,Downward,Use my colours;!,!;;;sx=60,ix=96,c1=96,c2=110";

// Colour bands moving up (or down) the tree
void mode_tree_rise() {
  const unsigned len = SEGLEN;
  const uint8_t bands = 1 + (SEGMENT.intensity >> 4);
  const uint16_t t = ticks(SEGMENT.speed + 1, 8);
  const bool down = SEGMENT.check1;
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint8_t idx = (uint8_t)(((p.h * bands) >> 2) + (down ? t : -(int)t));
    putPx(i, p, pal(idx));
  }
}
static const char _data_FX_TREE_RISE[] PROGMEM = "Tree Rising Colours@!,Bands,,,,Downward;;!;;sx=100,ix=48,pal=11";

// Colour wheel rotating around the tree
void mode_tree_rotate() {
  const unsigned len = SEGLEN;
  const uint8_t slices = 1 + (SEGMENT.intensity >> 5);
  const uint16_t t = ticks(SEGMENT.speed + 1, 8);
  const uint8_t lean = SEGMENT.custom1;
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint8_t idx = (uint8_t)(p.a * slices + ((p.h * lean) >> 8) + t);
    putPx(i, p, pal(idx));
  }
}
static const char _data_FX_TREE_ROTATE[] PROGMEM = "Tree Rotate@!,Slices,Lean;;!;;sx=90,ix=32,c1=0,pal=11";

// Classic fairy-light twinkle: each LED keeps a colour and twinkles on its own rhythm
void mode_tree_twinkle() {
  const unsigned len = SEGLEN;
  const uint8_t density = SEGMENT.intensity;
  const bool warm = SEGMENT.check1;
  const uint32_t warmWhite = 0xFF9A40;
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint32_t r = hash32(i * 2654435761u + 12345);
    uint32_t base = warm ? warmWhite : pal(r & 0xFF);
    uint8_t rate = 2 + ((r >> 8) & 7);                                // per-LED speed
    uint8_t ph = (uint8_t)(ticks(SEGMENT.speed + 16, 12) * rate / 4 + (r >> 16));
    uint8_t wave = sin8_t(ph);
    uint8_t v;
    if (((r >> 24) & 0xFF) < density) v = 40 + scale8(wave, 215);      // twinkling LED
    else v = 150 + scale8(wave, 40);                                    // steady, gentle shimmer
    putPx(i, p, scaleColor(base, v));
  }
}
static const char _data_FX_TREE_TWINKLE[] PROGMEM = "Tree Twinkle@!,Twinkle amount,,,,Warm white;;!;;sx=80,ix=140,pal=48";

// Baubles: a base colour with ornaments that softly glow up and fade in palette colours
void mode_tree_ornaments() {
  const unsigned len = SEGLEN;
  const uint32_t now = strip.now;
  const uint32_t base = (SEGCOLOR(1) & 0xFFFFFF) ? SEGCOLOR(1) : 0x003A00;   // colour 2, or dark green
  const uint8_t density = 8 + (SEGMENT.intensity >> 1);
  const uint32_t period = 1500 + (255 - SEGMENT.speed) * 24;               // ms per bauble cycle
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint32_t r = hash32(i * 40503u + 7);
    uint32_t local = now + (r % period);                                    // stagger LEDs
    uint32_t cycle = local / period;
    uint8_t phase = (uint8_t)(((local % period) * 255) / period);
    uint32_t rc = hash32(i ^ (cycle * 0x9E3779B9u));
    uint32_t c = base;
    if ((rc & 0xFF) < density) {
      uint32_t bc = pal((rc >> 8) & 0xFF);
      c = color_blend(base, bc, triwave(phase));
    }
    putPx(i, p, c);
  }
}
static const char _data_FX_TREE_ORNAMENTS[] PROGMEM = "Tree Ornaments@!,Baubles;,!;!;;sx=100,ix=90,pal=48";

// Snow falling down the tree and piling up at the base, then melting away
struct Flake { uint8_t a; uint8_t speed; uint16_t h16; };
void mode_tree_snow() {
  const unsigned len = SEGLEN;
  const unsigned count = 4 + (SEGMENT.intensity >> 3);                  // 4..35 flakes
  const unsigned bytes = count * sizeof(Flake);
  if (!SEGENV.allocateData(bytes)) { fallbackStatic(); return; }
  Flake* f = reinterpret_cast<Flake*>(SEGENV.data);
  const uint8_t pileMax = SEGMENT.custom1 >> 1;                          // up to half the tree
  const bool doPile = pileMax > 0;
  const uint32_t flakeCol = SEGMENT.check1 ? SEGCOLOR(0) : 0xFFFFFF;
  const uint32_t bg = SEGCOLOR(1);
  const uint8_t size = 6 + (SEGMENT.custom2 >> 4);                        // flake radius (h units)

  auto respawn = [&](Flake& fl, bool anywhere) {
    // start on an actual LED's angle so the flake runs down a string
    unsigned pick = hw_random16(len ? len : 1);
    TreePixel p = treePx(pick);
    fl.a = p.kind == KIND_TREE ? p.a : hw_random8();
    fl.h16 = anywhere ? hw_random16() : 65535;
    fl.speed = 2 + hw_random8(6);
  };

  if (SEGENV.call == 0) {
    for (unsigned k = 0; k < count; k++) respawn(f[k], true);
    SEGENV.aux0 = 0;  // pile height * 256
    SEGENV.aux1 = 0;  // 0 = building, 1 = melting
    SEGENV.step = strip.now;
  }
  uint32_t dt = strip.now - SEGENV.step;
  if (dt > 100) dt = 100;
  SEGENV.step = strip.now;
  const uint32_t fall = (dt * (SEGMENT.speed + 20)) >> 3;
  const uint16_t pileH = SEGENV.aux0 >> 8;

  for (unsigned k = 0; k < count; k++) {
    uint32_t d = fall * f[k].speed;
    uint32_t floorH = doPile ? (uint32_t)pileH << 8 : 0;
    if (f[k].h16 <= floorH + d) {
      if (doPile && SEGENV.aux1 == 0) SEGENV.aux0 = min<uint32_t>(SEGENV.aux0 + 90, (uint32_t)pileMax << 8);
      respawn(f[k], false);
    } else f[k].h16 -= d;
  }
  if (doPile) {
    if (SEGENV.aux1 == 0 && (SEGENV.aux0 >> 8) >= pileMax) SEGENV.aux1 = 1;
    if (SEGENV.aux1 == 1) {
      SEGENV.aux0 = SEGENV.aux0 > dt * 3 ? SEGENV.aux0 - dt * 3 : 0;
      if (SEGENV.aux0 == 0) SEGENV.aux1 = 0;
    }
  }

  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint32_t c = bg;
    if (doPile && p.h < pileH) {
      uint8_t sp = (hash32(i + (strip.now >> 7)) & 0xFF) < 12 ? 255 : 170;   // glistening snow
      c = scaleColor(flakeCol, sp);
    }
    uint16_t best = 255;
    for (unsigned k = 0; k < count; k++) {
      if (abs((int)p.h - (int)(f[k].h16 >> 8)) >= size) continue;          // quick reject
      uint16_t dd = treeDistance(p.h, p.a, f[k].h16 >> 8, f[k].a);
      if (dd < best) best = dd;
    }
    if (best < size) c = color_blend(c, flakeCol, 255 - (best * 255 / size));
    putPx(i, p, c);
  }
}
static const char _data_FX_TREE_SNOW[] PROGMEM = "Tree Snowfall@!,Flakes,Snow pile,Flake size,,Colour 1 snow;!,!;;;sx=90,ix=100,c1=80,c2=64";

// Comets / shooting stars running up (or down) the strings
struct Meteor { uint32_t pos; uint8_t a; uint8_t hue; uint8_t speed; uint8_t pad; };   // pos = head height * 256
void mode_tree_meteors() {
  const unsigned len = SEGLEN;
  const unsigned count = 1 + (SEGMENT.intensity >> 4);                   // 1..16
  if (!SEGENV.allocateData(count * sizeof(Meteor))) { fallbackStatic(); return; }
  Meteor* m = reinterpret_cast<Meteor*>(SEGENV.data);
  const bool down = SEGMENT.check1;
  const int trail = 20 + (SEGMENT.custom1 >> 1);                          // trail length (height units)
  auto respawn = [&](Meteor& mt, bool anywhere) {
    TreePixel p = treePx(hw_random16(len ? len : 1));
    mt.a = p.kind == KIND_TREE ? p.a : hw_random8();
    mt.pos = anywhere ? (uint32_t)hw_random16() : 0;
    mt.hue = hw_random8();
    mt.speed = 3 + hw_random8(5);
  };
  if (SEGENV.call == 0) { for (unsigned k = 0; k < count; k++) respawn(m[k], true); SEGENV.step = strip.now; }
  uint32_t dt = strip.now - SEGENV.step; if (dt > 100) dt = 100; SEGENV.step = strip.now;
  for (unsigned k = 0; k < count; k++) {
    m[k].pos += (dt * (SEGMENT.speed + 10) * m[k].speed) >> 3;
    if ((m[k].pos >> 8) > (uint32_t)(255 + trail)) respawn(m[k], false);  // head and tail have left the top
  }
  const uint8_t tol = stringTolerance();
  const uint32_t bg = SEGCOLOR(1);
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    const int ph = down ? 255 - p.h : p.h;                                 // height along the direction of travel
    uint32_t c = bg;
    for (unsigned k = 0; k < count; k++) {
      int da = abs((int)(int8_t)(uint8_t)(p.a - m[k].a));
      if (da > tol) continue;
      int behind = (int)(m[k].pos >> 8) - ph;                                // how far this LED is behind the head
      if (behind < 0 || behind > trail) continue;
      uint8_t v = 255 - behind * 255 / trail;
      uint32_t mc = SEGMENT.check2 ? 0xFFFFFF : pal(m[k].hue);
      if (behind < 3) mc = color_blend(mc, 0xFFFFFF, 160);                  // bright head
      c = color_add(c, scaleColor(mc, v));
    }
    putPx(i, p, c);
  }
}
static const char _data_FX_TREE_METEORS[] PROGMEM = "Tree Shooting Stars@!,Count,Trail,,,Downward,White;,!;!;;sx=110,ix=80,c1=100,pal=11";

// Fireworks bursting on the surface of the tree
struct Burst { uint8_t h, a, hue, active; uint16_t age; };
void mode_tree_fireworks() {
  const unsigned len = SEGLEN;
  const unsigned count = 1 + (SEGMENT.intensity >> 6);                    // 1..4 at once
  if (!SEGENV.allocateData(4 * sizeof(Burst))) { fallbackStatic(); return; }
  Burst* b = reinterpret_cast<Burst*>(SEGENV.data);
  if (SEGENV.call == 0) SEGENV.step = strip.now;
  uint32_t dt = strip.now - SEGENV.step; if (dt > 100) dt = 100; SEGENV.step = strip.now;
  const uint16_t life = 600 + (255 - SEGMENT.speed) * 8;
  const uint8_t maxR = 30 + (SEGMENT.custom1 >> 1);
  for (unsigned k = 0; k < count; k++) {
    if (!b[k].active) {
      if (hw_random8() < 12) {
        TreePixel p = treePx(hw_random16(len ? len : 1));
        b[k] = { (uint8_t)(40 + scale8(p.h, 200)), p.a, hw_random8(), 1, 0 };
      }
      continue;
    }
    b[k].age += dt;
    if (b[k].age >= life) b[k].active = 0;
  }
  const uint32_t bg = SEGCOLOR(1);
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint32_t c = bg;
    for (unsigned k = 0; k < count; k++) {
      if (!b[k].active) continue;
      uint32_t prog = (uint32_t)b[k].age * 255 / life;                    // 0..255
      uint16_t r = (uint16_t)(sqrt32_bw(prog * 255) * maxR / 255);           // fast at first, slowing
      uint16_t d = treeDistance(p.h, p.a, b[k].h, b[k].a);
      int ring = abs((int)d - (int)r);
      if (ring > 10) continue;
      uint8_t v = (uint8_t)((255 - ring * 25) * (255 - prog) / 255);
      uint32_t bc = SEGMENT.check1 ? CRGBW(CHSV32((uint16_t)(b[k].hue << 8), 255, 255)).color32 : pal(b[k].hue);
      if (prog < 30) bc = color_blend(bc, 0xFFFFFF, 200);                 // white flash at the start
      c = color_add(c, scaleColor(bc, v));
    }
    putPx(i, p, c);
  }
}
static const char _data_FX_TREE_FIREWORKS[] PROGMEM = "Tree Fireworks@!,Bursts,Size,,,Rainbow;,!;!;;sx=120,ix=128,c1=120,pal=11";

// Light rises up the tree, the star bursts, then the tree fades from the top down
void mode_tree_starburst() {
  const unsigned len = SEGLEN;
  const uint32_t period = 2500 + (255 - SEGMENT.speed) * 30;
  const uint32_t ph = (strip.now % period) * 1000 / period;               // 0..999
  const bool usePal = !SEGMENT.check1;
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint32_t col = usePal ? pal(p.h) : SEGCOLOR(0);
    uint32_t c = BLACK;
    if (p.kind == KIND_STAR) {
      // star: dark while the light climbs, flash white -> gold, then glow and fade
      uint32_t starC = (T.starMode == STARMODE_FOLLOW || T.starMode == STARMODE_OFF) ? T.starColor : starColorFor(p, T.starColor);
      uint32_t sc = BLACK;
      if (ph >= 550 && ph < 620) sc = 0xFFFFFF;
      else if (ph >= 620 && ph < 900) sc = scaleColor(starC, 255 - (ph - 620) * 120 / 280);
      else if (ph >= 900) sc = scaleColor(starC, 135 - (ph - 900) * 135 / 100);
      else sc = scaleColor(starC, 25);
      if (T.starMode != STARMODE_OFF) SEGMENT.setPixelColor(i, sc); else SEGMENT.setPixelColor(i, BLACK);
      continue;
    }
    if (ph < 550) {
      int front = ph * 255 / 550;                                         // rising front 0..255
      if (p.h <= front) {
        int behind = front - p.h;
        c = behind < 6 ? color_blend(col, 0xFFFFFF, 180) : scaleColor(col, max(70, 255 - behind));
      }
    } else if (ph < 620) {
      // whole tree flashes with the star
      c = color_blend(col, 0xFFFFFF, (uint8_t)(255 - (ph - 550) * 255 / 70));
    } else if (ph < 900) {
      c = col;
      if ((hash32(i + strip.now / 60) & 0xFF) < 10) c = 0xFFFFFF;          // sparkle
    } else {
      int front = 255 - (int)(ph - 900) * 255 / 100;                       // fades from the top down
      c = p.h <= front ? col : BLACK;
    }
    if (p.kind == KIND_DARK) c = BLACK;
    SEGMENT.setPixelColor(i, c);
  }
}
static const char _data_FX_TREE_STARBURST[] PROGMEM = "Tree Star Burst@!,,,,,Use colour 1;!;!;;sx=128,pal=11";

// Gift wrap: base colour with two ribbons crossing over the top and a glittering finish
void mode_tree_giftwrap() {
  const unsigned len = SEGLEN;
  const bool mine = SEGMENT.check1;
  const uint32_t paper  = mine ? SEGCOLOR(0) : 0xB00010;
  const uint32_t ribbon = mine ? SEGCOLOR(1) : 0xFFB000;
  const uint8_t half = 4 + (SEGMENT.intensity >> 4);                      // ribbon half-width (angle)
  const uint8_t rot = (uint8_t)(ticks(SEGMENT.speed, 13));
  const uint8_t glitter = SEGMENT.custom1;
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint8_t a = p.a + rot;
    // two ribbons over the top = four vertical bands around the cone
    uint8_t d = a & 63; if (d > 32) d = 64 - d;
    // ribbons look the same width all the way up: widen the angle near the top
    uint16_t w = (uint16_t)half * 255 / max<uint16_t>(40, 255 - p.h);
    uint32_t c = paper;
    if (d <= w) c = ribbon;
    else if (d <= w + 2) c = color_blend(paper, ribbon, 110);
    if (glitter && (hash32(i * 31u + (strip.now >> 6)) & 0xFF) < (glitter >> 3)) c = color_blend(c, 0xFFFFFF, 200);
    // star = the bow: ribbon colour
    putPx(i, p, p.kind == KIND_STAR ? ribbon : c);
  }
}
static const char _data_FX_TREE_GIFTWRAP[] PROGMEM = "Tree Gift Wrap@Rotate,Ribbon width,Glitter,,,Use my colours;!,!;;;sx=0,ix=60,c1=60";

// Breathing glow, each height band slightly out of step: a calm "the tree is breathing" look
void mode_tree_glow() {
  const unsigned len = SEGLEN;
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint8_t v = 70 + scale8(sin8_t((uint8_t)ticks(SEGMENT.speed + 8, 9) - (p.h >> 1) * (SEGMENT.intensity >> 6)), 185);
    uint32_t c = pal(p.h);
    putPx(i, p, scaleColor(c, v));
  }
}
static const char _data_FX_TREE_GLOW[] PROGMEM = "Tree Glow@!,Ripple;;!;;sx=60,ix=128,pal=48";

// DIY picture drawn in the editor at /xmastree, wrapped onto the tree
void mode_tree_picture() {
  const unsigned len = SEGLEN;
  if (!T.pic || !T.picLoaded) { mode_tree_twinkle(); return; }
  const bool frontOnly = !SEGMENT.check1;
  const uint8_t spin = SEGMENT.speed ? (uint8_t)(ticks(SEGMENT.speed, 14)) : 0;
  const uint8_t front = SEGMENT.custom1 + spin;                           // angle facing the viewer
  const uint32_t bg = SEGCOLOR(1);
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    int x;
    if (frontOnly) {
      int da = (int8_t)(uint8_t)(p.a - front);                              // -128..127
      if (da < -64 || da >= 64) { putPx(i, p, bg); continue; }              // back of the tree
      // the picture is a flat, front-on view of the tree: project the LED onto it.
      // sin() gives the sideways position, (1 - h) shrinks it towards the narrow top.
      float s = sinf((float)da * 6.2832f / 256.0f) * (1.0f - (float)p.h / 255.0f);   // -1..1 across the base
      x = (int)((s + 1.0f) * 0.5f * (XT_PIC_W - 1) + 0.5f);
    } else {
      x = (uint8_t)(p.a - front) * XT_PIC_W / 256;
    }
    int y = (255 - p.h) * (XT_PIC_H - 1) / 255;
    const uint8_t* q = T.pic + (y * XT_PIC_W + x) * 3;
    uint32_t c = RGBW32(q[0], q[1], q[2], 0);
    if (!(c & 0xFFFFFF)) c = bg;
    putPx(i, p, c);
  }
}
static const char _data_FX_TREE_PICTURE[] PROGMEM = "Tree Picture@Spin,,Turn,,,Wrap all round;,!;;;sx=0,c1=0";

// Layout check: every string gets its own colour; first LED of each string green, last red,
// star yellow, unused LEDs dark. "Highlight" picks a single string (0 = show all).
void mode_tree_layouttest() {
  const unsigned len = SEGLEN;
  const uint8_t only = SEGMENT.custom1 * (T.runs + 1) / 256;              // 0 = all, n = string n
  int prevTag = -1;
  for (unsigned i = 0; i < len; i++) {
    TreePixel p = treePx(i);
    uint32_t c = BLACK;
    if (p.kind == KIND_STAR) c = 0xFFD000;
    else if (p.kind == KIND_TREE) {
      bool first = (int)p.tag != prevTag;
      bool last = (i + 1 >= len) || treePx(i + 1).tag != p.tag || treePx(i + 1).kind != KIND_TREE;
      prevTag = p.tag;
      if (only && (unsigned)(p.tag + 1) != only) c = BLACK;
      else if (first) c = 0x00FF00;
      else if (last) c = 0xFF0000;
      else {
        CRGBW hc = CHSV32((uint16_t)((p.tag * 47u) << 8), 230, 120);       // dim, distinct per string
        c = hc.color32;
        // a white dot that climbs every string shows which way is up
        if ((uint8_t)(p.h - (uint8_t)(strip.now >> 3)) < 8) c = 0xFFFFFF;
      }
    }
    SEGMENT.setPixelColor(i, c);
  }
}
static const char _data_FX_TREE_LAYOUT[] PROGMEM = "Tree Layout Test@,,Highlight string;;;;c1=0";

} // namespace

// ---------------------------------------------------------------------------------------------
//  USERMOD
// ---------------------------------------------------------------------------------------------
class XmasTreeUsermod : public Usermod {
  bool enabled = true;
  bool initDone = false;
  bool needRebuild = true;
  uint8_t layout = LAYOUT_STRINGS;
  uint8_t starPos = STAR_END;
  uint8_t starMode = STARMODE_TWINKLE;
  char starColorHex[8] = "FFB000";

  static const char _name[];

  void freeMap() { if (T.map) { free(T.map); T.map = nullptr; } T.mapLen = 0; }

  bool buildCustomMap() {
    File f = WLED_FS.open(XT_CSV_FILE, "r");
    if (!f) { T.lastError = "no /xmastree.csv uploaded"; return false; }
    CsvMapper cm;
    char line[96];
    while (f.available()) { size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1); line[n] = 0; cm.observe(line); }
    cm.finish();
    if (!cm.points) { f.close(); T.lastError = "CSV has no valid x,y,z lines"; return false; }
    f.seek(0);
    // count LED lines first so we allocate exactly
    uint32_t total = 0;
    while (f.available()) {
      size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1); line[n] = 0;
      TreePixel tmp; if (cm.place(line, tmp)) total++;
    }
    total += T.cfg.offset;
    if (total > XT_MAX_LEDS) total = XT_MAX_LEDS;
    T.map = (TreePixel*)malloc(total * sizeof(TreePixel));
    if (!T.map) { f.close(); T.lastError = "out of memory"; return false; }
    uint32_t n = 0;
    for (; n < T.cfg.offset && n < total; n++) T.map[n] = { 0, 0, KIND_DARK, 0 };
    f.seek(0);
    while (f.available() && n < total) {
      size_t k = f.readBytesUntil('\n', line, sizeof(line) - 1); line[k] = 0;
      if (cm.place(line, T.map[n])) n++;
    }
    f.close();
    T.mapLen = n;
    return true;
  }

  void rebuildMap() {
    freeMap();
    T.lastError = nullptr;
    T.cfg.layout = layout;
    T.cfg.starPos = starPos;
    T.starMode = starMode;
    T.starColor = (uint32_t)strtoul(starColorHex[0] == '#' ? starColorHex + 1 : starColorHex, nullptr, 16) & 0xFFFFFF;
    if (!enabled) return;
    if (layout == LAYOUT_CUSTOM) {
      buildCustomMap();
    } else {
      uint32_t total = totalLength(T.cfg);
      if (total > XT_MAX_LEDS) { total = XT_MAX_LEDS; T.lastError = "tree too long, truncated to 4096 LEDs"; }
      if (total) {
        T.map = (TreePixel*)malloc(total * sizeof(TreePixel));
        if (T.map) T.mapLen = buildMap(T.cfg, T.map, total);
        else T.lastError = "out of memory";
      }
    }
    // stats
    T.treeLeds = 0; uint8_t maxTag = 0;
    for (unsigned i = 0; i < T.mapLen; i++) if (T.map[i].kind == KIND_TREE) { T.treeLeds++; if (T.map[i].tag > maxTag) maxTag = T.map[i].tag; }
    T.runs = (layout == LAYOUT_STRINGS) ? max<uint16_t>(1, T.cfg.runs) : (uint16_t)(maxTag + 1);
    needRebuild = false;
  }

  void loadPicture() {
    if (!T.pic) T.pic = (uint8_t*)malloc(XT_PIC_BYTES);
    if (!T.pic) return;
    File f = WLED_FS.open(XT_PIC_FILE, "r");
    T.picLoaded = false;
    if (!f) return;
    if (f.size() == XT_PIC_BYTES) T.picLoaded = f.read(T.pic, XT_PIC_BYTES) == XT_PIC_BYTES;
    f.close();
  }

  void registerWeb() {
    // tree map: {"n":..,"runs":..,"layout":..,"w":32,"h":48,"px":"<hex h a kind tag ...>"}
    server.on(F("/xmastree/map"), HTTP_GET, [](AsyncWebServerRequest* request) {
      AsyncResponseStream* s = request->beginResponseStream(F("application/json"));
      s->printf("{\"n\":%u,\"runs\":%u,\"layout\":%u,\"aspect\":%u,\"w\":%u,\"h\":%u,\"err\":\"%s\",\"px\":\"",
                T.mapLen, T.runs, T.cfg.layout, T.aspect10, XT_PIC_W, XT_PIC_H, T.lastError ? T.lastError : "");
      static const char hx[] = "0123456789abcdef";
      char buf[9];
      for (unsigned i = 0; i < T.mapLen; i++) {
        const TreePixel& p = T.map[i];
        uint8_t v[4] = { p.h, p.a, p.kind, p.tag };
        for (int k = 0; k < 4; k++) { buf[k * 2] = hx[v[k] >> 4]; buf[k * 2 + 1] = hx[v[k] & 15]; }
        buf[8] = 0;
        s->print(buf);
      }
      s->print("\"}");
      request->send(s);
    });
    // picture download
    server.on(F("/xmastree/pic"), HTTP_GET, [](AsyncWebServerRequest* request) {
      if (WLED_FS.exists(XT_PIC_FILE)) request->send(WLED_FS, XT_PIC_FILE, F("application/octet-stream"));
      else request->send(404, F("text/plain"), F("no picture"));
    });
    // picture upload: raw RGB bytes, XT_PIC_W x XT_PIC_H, row 0 = top
    server.on(F("/xmastree/pic"), HTTP_POST,
      [](AsyncWebServerRequest* request) {
        bool ok = T.pic && T.picLoaded;
        request->send(ok ? 200 : 400, F("application/json"), ok ? F("{\"ok\":true}") : F("{\"ok\":false}"));
      },
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (total != XT_PIC_BYTES || !T.pic) return;
        if (index == 0) T.picLoaded = false;
        if (index + len <= XT_PIC_BYTES) memcpy(T.pic + index, data, len);
        if (index + len == total) {
          File f = WLED_FS.open(XT_PIC_FILE, "w");
          if (f) { f.write(T.pic, XT_PIC_BYTES); f.close(); }
          T.picLoaded = true;
        }
      });
    // editor page (registered last: "/xmastree" also matches "/xmastree/..." in AsyncWebServer)
    server.on(F("/xmastree"), HTTP_GET, [](AsyncWebServerRequest* request) {
      AsyncWebServerResponse* r = request->beginResponse_P(200, F("text/html"), (const uint8_t*)XT_PAGE_HTML, strlen_P(XT_PAGE_HTML));
      r->addHeader(F("Cache-Control"), F("no-cache"));
      request->send(r);
    });
  }

 public:
  void setup() override {
    strip.addEffect(255, &mode_tree_spiral,     _data_FX_TREE_SPIRAL);
    strip.addEffect(255, &mode_tree_candycane,  _data_FX_TREE_CANDYCANE);
    strip.addEffect(255, &mode_tree_twinkle,    _data_FX_TREE_TWINKLE);
    strip.addEffect(255, &mode_tree_ornaments,  _data_FX_TREE_ORNAMENTS);
    strip.addEffect(255, &mode_tree_snow,       _data_FX_TREE_SNOW);
    strip.addEffect(255, &mode_tree_meteors,    _data_FX_TREE_METEORS);
    strip.addEffect(255, &mode_tree_fireworks,  _data_FX_TREE_FIREWORKS);
    strip.addEffect(255, &mode_tree_starburst,  _data_FX_TREE_STARBURST);
    strip.addEffect(255, &mode_tree_giftwrap,   _data_FX_TREE_GIFTWRAP);
    strip.addEffect(255, &mode_tree_rise,       _data_FX_TREE_RISE);
    strip.addEffect(255, &mode_tree_rotate,     _data_FX_TREE_ROTATE);
    strip.addEffect(255, &mode_tree_glow,       _data_FX_TREE_GLOW);
    strip.addEffect(255, &mode_tree_picture,    _data_FX_TREE_PICTURE);
    strip.addEffect(255, &mode_tree_layouttest, _data_FX_TREE_LAYOUT);
    registerWeb();
    loadPicture();
    rebuildMap();
    initDone = true;
  }

  void loop() override {
    if (needRebuild && initDone) rebuildMap();
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray info = user.createNestedArray(F("Xmas Tree"));
    if (!enabled) { info.add(F("disabled")); return; }
    if (T.lastError) { info.add(T.lastError); return; }
    String s = String(T.treeLeds) + F(" tree LEDs, ") + String(T.mapLen) + F(" mapped");
    info.add(s);
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[F("enabled")]        = enabled;
    top[F("layout")]         = layout;
    top[F("offset")]         = T.cfg.offset;
    top[F("strings")]        = T.cfg.runs;
    top[F("ledsPerString")]  = T.cfg.ledsPerRun;
    top[F("zigzag")]         = T.cfg.serpentine;
    top[F("firstStringUp")]  = T.cfg.firstRunUp;
    top[F("ledsAtEachBend")] = T.cfg.turnGap;
    top[F("spiralLeds")]     = T.cfg.spiralLeds;
    top[F("spiralTurnsx10")] = T.cfg.spiralTurns10;
    top[F("spiralFromTop")]  = T.cfg.spiralFromTop;
    top[F("clockwise")]      = T.cfg.clockwise;
    top[F("heightToWidthx10")] = T.aspect10;
    top[F("starLeds")]       = T.cfg.starLeds;
    top[F("starPosition")]   = starPos;
    top[F("starMode")]       = starMode;
    top[F("starColour")]     = starColorHex;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    bool complete = !top.isNull();
    complete &= getJsonValue(top[F("enabled")],        enabled, true);
    complete &= getJsonValue(top[F("layout")],         layout, (uint8_t)LAYOUT_STRINGS);
    complete &= getJsonValue(top[F("offset")],         T.cfg.offset, (uint16_t)0);
    complete &= getJsonValue(top[F("strings")],        T.cfg.runs, (uint16_t)8);
    complete &= getJsonValue(top[F("ledsPerString")],  T.cfg.ledsPerRun, (uint16_t)50);
    complete &= getJsonValue(top[F("zigzag")],         T.cfg.serpentine, true);
    complete &= getJsonValue(top[F("firstStringUp")],  T.cfg.firstRunUp, true);
    complete &= getJsonValue(top[F("ledsAtEachBend")], T.cfg.turnGap, (uint8_t)0);
    complete &= getJsonValue(top[F("spiralLeds")],     T.cfg.spiralLeds, (uint16_t)300);
    complete &= getJsonValue(top[F("spiralTurnsx10")], T.cfg.spiralTurns10, (uint16_t)80);
    complete &= getJsonValue(top[F("spiralFromTop")],  T.cfg.spiralFromTop, false);
    complete &= getJsonValue(top[F("clockwise")],      T.cfg.clockwise, false);
    complete &= getJsonValue(top[F("heightToWidthx10")], T.aspect10, (uint8_t)20);
    complete &= getJsonValue(top[F("starLeds")],       T.cfg.starLeds, (uint16_t)0);
    complete &= getJsonValue(top[F("starPosition")],   starPos, (uint8_t)STAR_END);
    complete &= getJsonValue(top[F("starMode")],       starMode, (uint8_t)STARMODE_TWINKLE);
    String sc = top[F("starColour")] | String("FFB000");
    strlcpy(starColorHex, sc.c_str(), sizeof(starColorHex));
    if (T.aspect10 < 5) T.aspect10 = 5;
    if (layout > LAYOUT_CUSTOM) layout = LAYOUT_STRINGS;
    needRebuild = true;  // rebuild in loop() (filesystem access is safer there)
    return complete;
  }

  void appendConfigData(Print& s) override {
    s.print(F("dd=addDropdown('XmasTree','layout');"
              "addOption(dd,'Strings (zig-zag / multi-output / all up)',0);"
              "addOption(dd,'Spiral wrapped round a cone',1);"
              "addOption(dd,'Custom map (/xmastree.csv)',2);"
              "dd=addDropdown('XmasTree','starPosition');"
              "addOption(dd,'No star',0);addOption(dd,'After the tree (end of strip)',1);addOption(dd,'Before the tree (start of strip)',2);"
              "dd=addDropdown('XmasTree','starMode');"
              "addOption(dd,'Follow the effect',0);addOption(dd,'Solid star colour',1);addOption(dd,'Twinkle',2);"
              "addOption(dd,'Pulse',3);addOption(dd,'Rainbow',4);addOption(dd,'Off',5);"
              "addInfo('XmasTree:offset',1,'unused LEDs at the start of the segment');"
              "addInfo('XmasTree:strings',1,'strings / runs up the tree (Strings layout)');"
              "addInfo('XmasTree:zigzag',1,'strings alternate up and down');"
              "addInfo('XmasTree:ledsAtEachBend',1,'dark LEDs used by each bend');"
              "addInfo('XmasTree:spiralTurnsx10',1,'e.g. 80 = 8 turns');"
              "addInfo('XmasTree:heightToWidthx10',1,'tree height / base width, e.g. 20 = twice as tall as wide');"
              "addInfo('XmasTree:starColour',1,'hex, e.g. FFB000');"
              "addInfo('XmasTree:enabled',1,'<a href=\"/xmastree\" target=\"_blank\">Open the tree designer</a>');"));
  }

  uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

const char XmasTreeUsermod::_name[] PROGMEM = "XmasTree";

static XmasTreeUsermod xmas_tree;
REGISTER_USERMOD(xmas_tree);
