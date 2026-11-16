# HDR Calibration UI

> Menu: **Post Processing → HDR Calibration** · Setting id(s): `post.hdrcal` (writes core's `display.hdr.peakNits`, `display.hdr.paperWhiteNits`, `display.hdr.blackLevelNits`) · Owner module: `post`
> Values: Show calibration screen On/Off · Presets: not preset-driven

(The core owns the output encoding: PQ/scRGB swap chain, gamma, brightness.
This module owns the tonemapper that consumes these values and the
calibration screen that sets them.)

## Definition

HDR displays vary hugely: one panel peaks at 400 nits, another at 1500. If the
game assumes the wrong peak, highlights either clip to flat white or never
use the display's range. The HDR calibration screen lets the player find
three numbers: **peak brightness** (brightest value the display can show
before clipping), **paper white** (how bright UI and a "white" piece of paper
should be, controls overall image brightness) and **black level / shadow
detail**. Windows' "HDR Calibration" app and the console system calibration
do the same; games read those values when available.

## How it works

1. **Peak brightness test:** draw a logo at exactly `X` nits on a background
   at the display maximum (e.g. 10 000 nits encoded in PQ). The player raises
   X until the logo just disappears; at that point X equals the display's real
   clipping point.
2. **Paper white:** show a reference scene with UI; the player picks a
   comfortable brightness (typically 100–250 nits).
3. **Black level:** show a logo at a value slightly above black on true black;
   the player lowers it until barely visible, avoiding crushed or raised
   blacks.
4. **Use the values:** the tonemapper maps scene white to paper white and
   compresses highlights toward the measured peak, instead of 1.0. With
   ACES/AgX this means changing the curve's shoulder to end at
   `peak / paperWhite` in relative units.
5. On SDR displays the screen shows only the gamma/brightness test (a logo
   that should be barely visible), and the values feed the core's gamma.

## Breakdown

| Piece | Where |
|---|---|
| Settings | `hdr.peakNits`, `hdr.paperWhiteNits`, `hdr.blackLevel` registered in the post module |
| Pattern screen | `PostModule::onGui` draws the 3 steps; the pattern itself is a fullscreen pass `hdr_calib.frag` writing nits / paperWhite so the core's PQ encode turns it into exact nits |
| Consumption | tonemap pass: `uPeak = peakNits/paperWhiteNits` scales the curve's white point |

## Optimization

No runtime cost; the pattern only renders while the calibration screen is
open. Read OS calibration (Windows: `IDXGIOutput6::GetDesc1` gives
`MaxLuminance`, `MinLuminance`, `MaxFullFrameLuminance`) as defaults so most
players never need the screen.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.hdrcal` | Off | -2.6 | -4.0 | +0.0 | n/a |
| `post.hdrcal` | On | -5.6 | -4.0 | +0.0 | n/a |

## Rebuild (from scratch)

1. Create an HDR swap chain (core thread) and query `GetDesc1`.
2. Calibration pattern shader: `out = (uv inside logo) ? vec3(uTestNits) : vec3(10000.0);`
   divided by paper white (the core's PQ encode multiplies it back).
3. Slider for `uTestNits`; when the player confirms, store it as `peakNits`.
4. Tonemapper: `x = x * exposure; y = tonemap(x / peakRel) * peakRel;`
   where `peakRel = peakNits / paperWhiteNits`, then the core encodes
   `y * paperWhiteNits` to PQ.

Files: `src/modules/post/post_module.cpp` (onGui, runLdr), `shaders/post/postldr.frag` (calibration pattern)
