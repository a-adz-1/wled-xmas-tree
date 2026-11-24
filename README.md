# 🎄 WLED Christmas Tree usermod

Turn any LED Christmas tree into a smart tree with effects like the commercial app-controlled ones. These include spirals, snowfall, shooting stars, fireworks and a star burst, plus a **pixel-art designer** for drawing your own pictures on the tree.

You describe how your LEDs are arranged **once**, and every effect then works out where each LED sits on the tree (its height and its angle around it). The same effect looks right on very different builds:

| Layout | Example |
|---|---|
| **Zig-zag single strip** | One strip running up and down the cone, e.g. 15 strings × 60 LEDs |
| **Multi-output** | Several controller outputs, each going up and back down (e.g. 4 outputs = 8 strings) |
| **All strings up** | Every string starts at the base, wired in parallel |
| **Spiral** | One strip wound round a cone frame |
| **Custom 3D map** | Lights on a real tree, with positions from a CSV file (e.g. from a camera-mapping tool) |

A star on top is supported on every layout, at the end or the start of the strip.

## Effects

| Effect | What it does |
|---|---|
| **Tree Spiral** | Stripes wrapping round the tree, using the palette or two colours |
| **Tree Candy Cane** | Red and white spiral stripes |
| **Tree Twinkle** | Classic fairy lights, multicolour or warm white |
| **Tree Ornaments** | Baubles glowing up and fading over a green tree |
| **Tree Snowfall** | Snowflakes fall down the strings and pile up at the base, then melt |
| **Tree Shooting Stars** | Comets with trails racing up (or down) the strings |
| **Tree Fireworks** | Bursts expanding across the surface of the tree |
| **Tree Star Burst** | Light climbs to the top, the star flashes, and the tree sparkles then fades |
| **Tree Gift Wrap** | Wrapping-paper colour with ribbons crossing over the top and glitter |
| **Tree Rising Colours** | Palette bands moving up or down |
| **Tree Rotate** | Palette slices turning round the tree |
| **Tree Glow** | A slow breathing glow that ripples up the tree |
| **Tree Picture** | Shows a picture drawn in the designer |
| **Tree Layout Test** | Colours each string, marks the first LED of each string green and the last red, and runs a dot upwards. Use it to check your settings |

The star follows its own setting (twinkle, pulse, solid, rainbow, off, or follow the effect), so it stays a star whatever the tree is doing.

## Install

This needs **WLED 16** (or newer) built from source with PlatformIO. The usermod is pulled in straight from GitHub.

1. Get the WLED source (`git clone https://github.com/wled/WLED.git`, then check out tag `v16.0.1`).
2. Create `platformio_override.ini` in the WLED folder:

   ```ini
   [platformio]
   default_envs = esp32dev_tree

   [env:esp32dev_tree]
   extends = env:esp32dev
   custom_usermods = audioreactive https://github.com/a-adz-1/wled-xmas-tree.git
   ```

   Change `esp32dev` to match your board (e.g. `esp32s3dev_8MB_opi`). An ESP32 is strongly recommended. The usermod has not been tested on ESP8266.
3. Build and upload: `pio run -e esp32dev_tree -t upload`, or use the PlatformIO buttons in VS Code.

[`examples/platformio_override.ini`](examples/platformio_override.ini) has a few more board examples.

## Set it up

1. **LED Preferences:** set up your outputs as usual. With several outputs, WLED joins them end to end in order.
2. **Segments:** use **one segment covering the whole tree and star**, with Reverse, Mirror, Grouping and Spacing left at their defaults. Turn **2D off**.
3. **Config → Usermods → XmasTree:** describe the tree:

| Setting | Meaning |
|---|---|
| `layout` | Strings / Spiral / Custom map |
| `offset` | Unused LEDs at the start of the segment, before the tree begins |
| `strings`, `ledsPerString` | Number of strings and LEDs on each one (Strings layout) |
| `zigzag` | Strings alternate up and down. Untick it if every string starts at the base |
| `firstStringUp` | Untick it if the strip starts at the top |
| `ledsAtEachBend` | LEDs that sit in each bend and should stay dark |
| `spiralLeds`, `spiralTurnsx10`, `spiralFromTop` | Spiral layout (`80` = 8 turns) |
| `clockwise` | Direction the strings or spiral advance round the tree, seen from above |
| `heightToWidthx10` | Tree height ÷ base width × 10 (`20` = twice as tall as wide). Makes snow and fireworks round instead of stretched |
| `starLeds`, `starPosition` | LEDs in the star, and whether they come after or before the tree |
| `starMode`, `starColour` | How the star behaves (Twinkle, Pulse, Solid, Rainbow, Off, Follow effect) |

4. Pick **Tree Layout Test**. Every string should show a green LED at its first end and a red LED at its last, and white dots should travel **up** every string. If they go down, flip `firstStringUp`. If strings light in the wrong order around the tree, flip `clockwise`.

### Examples

* **Single strip, 15 strings × 60 LEDs, 20-LED star at the top:** `strings 15`, `ledsPerString 60`, `zigzag ✓`, `firstStringUp ✓`, `starLeds 20`, `starPosition: after the tree`. Use an odd number of strings so the strip finishes at the top, next to the star.
* **4 outputs, each going up 40 LEDs and back down:** `strings 8`, `ledsPerString 40`, `zigzag ✓`. The star can go on a 5th output, after the tree.
* **Spiral of 300 LEDs, 8 turns, starting at the base:** `layout: Spiral`, `spiralLeds 300`, `spiralTurnsx10 80`.

## Tree Designer (pixel art)

Open **`http://<your-wled>/xmastree`**. There is also a link on the usermod settings page.

* Draw with pen, fill, erase and colour picker, or start from a built-in picture (bell, star, snowman, heart, present, candy cane, snowflake, reindeer, stripes), add text, or import an image.
* The preview uses **your tree's real LED positions**, so you can see how much detail your tree can actually show before saving.
* **Save & show** stores the picture on the device and switches to **Tree Picture**.
* **Install Christmas presets + playlist** adds 13 ready-made presets (slots 200–212) and a "Christmas show" playlist (slot 230) that cycles through them every 3 minutes.

The picture is drawn as the tree looks **from the front**, so keep it inside the dotted outline. In **Tree Picture**, the *Turn* slider chooses which side faces the room. *Spin* rotates it. *Wrap all round* stretches the picture right round the tree instead, which suits patterns.

## Custom 3D map (lights on a real tree)

Upload `/xmastree.csv` with WLED's file editor (`http://<your-wled>/edit`), then set `layout` to **Custom map**. Use one line per LED, in strip order:

```
# x, y, z   (any units, z = up)
0.12, -0.30, 0.05
0.10, -0.28, 0.09
-                      <- an LED that isn't on the tree
0.00,  0.00, 1.80, 2   <- optional 4th column: 0 = unused, 1 = tree, 2 = star
```

Coordinates from camera-mapping tools (for example the "GIFT" CSV format popularised by Matt Parker's Christmas tree) work as-is. See [`examples/tree_coords_example.csv`](examples/tree_coords_example.csv).

## Power and wiring tips

* A full tree draws a lot of current. 900 × WS2812B can exceed 50 A at full white. Set WLED's **current limiter** to match your power supply, and inject power at several points.
* For big trees, **12 V strips** (WS2815 / WS2811) suffer much less voltage drop than 5 V ones.
* At each bend, either cut the strip and bridge it with three short wires, or twist it and count the dark LEDs in `ledsAtEachBend`.

## HTTP endpoints

| Endpoint | |
|---|---|
| `GET /xmastree` | Designer page |
| `GET /xmastree/map` | JSON: LED count, layout and each LED's height, angle, kind and string (hex encoded) |
| `GET /xmastree/pic` | Saved picture, raw RGB, 32×48, top row first |
| `POST /xmastree/pic` | Upload a picture in the same format |

## Development

* `src/tree_geometry.h` holds the layout maths. It has no WLED dependencies and has host-side tests: `cd test && g++ -std=c++17 -I../src test_geometry.cpp -o t && ./t`
* `web/designer.html` is the designer page. After editing it, run `python3 tools/embed_page.py` to regenerate `src/xmas_tree_page.h`. You can open the HTML file directly in a browser to work on it with a demo tree.

## Licence

MIT © a-adz-1
