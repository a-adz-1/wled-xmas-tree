// Host-side tests for tree_geometry.h:  g++ -std=c++17 -I../src test_geometry.cpp -o t && ./t
#include "tree_geometry.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
using namespace xmastree;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

int main() {
  // 1. Zig-zag single strip, 15 runs x 60 LEDs, 20 star LEDs at the end
  {
    TreeConfig c; c.layout = LAYOUT_STRINGS; c.runs = 15; c.ledsPerRun = 60; c.serpentine = true;
    c.starLeds = 20; c.starPos = STAR_END;
    CHECK(totalLength(c) == 920);
    std::vector<TreePixel> m(2000);
    uint32_t n = buildMap(c, m.data(), m.size());
    CHECK(n == 920);
    CHECK(m[0].h == 0 && m[59].h == 255);              // run 0 goes up
    CHECK(m[60].h == 255 && m[119].h == 0);            // run 1 comes down
    CHECK(m[840].h == 0 && m[899].h == 255);           // run 14 (odd count) goes up, ends at the top
    CHECK(m[0].a == m[59].a && m[60].a != m[0].a);     // one angle per run
    CHECK(m[60].tag == 1 && m[899].tag == 14);
    CHECK(m[900].kind == KIND_STAR && m[919].kind == KIND_STAR && m[919].tag == 255);
    CHECK(m[899].kind == KIND_TREE);
    // runs are spread evenly: run r at (r+0.5)/15 turns
    CHECK(abs((int)m[0].a - (int)lround(0.5/15*256)) <= 1);
  }
  // 2. 4 channels x (up+down) = 8 runs, 2 dark LEDs at each bend, star first, 5 unused LEDs before
  {
    TreeConfig c; c.layout = LAYOUT_STRINGS; c.runs = 8; c.ledsPerRun = 40; c.turnGap = 2;
    c.starLeds = 10; c.starPos = STAR_START; c.offset = 5;
    std::vector<TreePixel> m(1000);
    uint32_t n = buildMap(c, m.data(), m.size());
    CHECK(n == 5 + 10 + 8 * 40 + 7 * 2);
    CHECK(m[0].kind == KIND_DARK && m[4].kind == KIND_DARK);
    CHECK(m[5].kind == KIND_STAR && m[14].kind == KIND_STAR);
    CHECK(m[15].kind == KIND_TREE && m[15].h == 0);
    CHECK(m[55].kind == KIND_DARK && m[56].kind == KIND_DARK && m[55].h == 255); // bend at the top
    CHECK(m[57].kind == KIND_TREE && m[57].h == 255);
  }
  // 3. All strings start at the base (parallel)
  {
    TreeConfig c; c.runs = 4; c.ledsPerRun = 10; c.serpentine = false; c.starPos = STAR_NONE;
    std::vector<TreePixel> m(100);
    uint32_t n = buildMap(c, m.data(), m.size());
    CHECK(n == 40);
    for (int r = 0; r < 4; r++) CHECK(m[r * 10].h == 0 && m[r * 10 + 9].h == 255);
  }
  // 4. Spiral: monotonic height, correct number of turns, denser near... wait: more LEDs per turn at the base
  {
    TreeConfig c; c.layout = LAYOUT_SPIRAL; c.spiralLeds = 400; c.spiralTurns10 = 80; c.starPos = STAR_NONE;
    std::vector<TreePixel> m(400);
    uint32_t n = buildMap(c, m.data(), m.size());
    CHECK(n == 400);
    CHECK(m[0].h == 0 && m[399].h == 255);
    bool mono = true; for (int i = 1; i < 400; i++) if (m[i].h < m[i - 1].h) mono = false;
    CHECK(mono);
    CHECK(m[399].tag == 8);
    int firstTurn = 0, lastTurn = 0;
    for (int i = 0; i < 400; i++) { if (m[i].tag == 0) firstTurn++; if (m[i].tag == 7) lastTurn++; }
    CHECK(firstTurn > lastTurn * 3);   // bottom turn is much longer than top turn
    // from top: first LED at the top
    c.spiralFromTop = true; buildMap(c, m.data(), m.size());
    CHECK(m[0].h == 255 && m[399].h == 0);
  }
  // 5. Clockwise reverses angle direction
  {
    TreeConfig c; c.runs = 4; c.ledsPerRun = 2; c.starPos = STAR_NONE;
    std::vector<TreePixel> a(8), b(8);
    buildMap(c, a.data(), 8); c.clockwise = true; buildMap(c, b.data(), 8);
    CHECK((uint8_t)(a[2].a + b[2].a) == 0);
  }
  // 6. Truncation never overruns
  {
    TreeConfig c; c.runs = 10; c.ledsPerRun = 100; c.starLeds = 50;
    std::vector<TreePixel> m(300);
    CHECK(buildMap(c, m.data(), 300) == 300);
  }
  // 7. CSV mapper
  {
    const char* lines[] = { "# x,y,z", "1,0,0", "0,1,1", "-1,0,2", "-", "0,-1,3,2", "", "garbage" };
    CsvMapper cm;
    for (auto l : lines) cm.observe(l);
    cm.finish();
    CHECK(cm.points == 4);
    std::vector<TreePixel> out;
    for (auto l : lines) { TreePixel p; if (cm.place(l, p)) out.push_back(p); }
    CHECK(out.size() == 6);                 // comment + blank skipped; "-" and "garbage" occupy slots
    CHECK(out[0].h == 0 && out[0].a == 0);
    CHECK(out[1].a == 64);                  // quarter turn
    CHECK(out[2].a == 128);
    CHECK(out[3].kind == KIND_DARK);
    CHECK(out[4].kind == KIND_STAR && out[4].h == 255 && out[4].a == 192);
    CHECK(out[5].kind == KIND_DARK);
  }
  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("all geometry tests passed\n");
  return 0;
}
