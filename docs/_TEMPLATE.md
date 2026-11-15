# <Setting name>

> Menu: **<Category> → <Label>** · Setting id(s): `area.setting` · Owner module: `area.feature`
> Values: Off / Low / Medium / High / Ultra (or range) · Presets: Low=…, Medium=…, High=…, Ultra=…

## Definition
One paragraph a player understands: what the option changes on screen and why it exists.

## How it works
The technique in plain engineering terms: the inputs it reads, the math or algorithm, what each
quality level changes (sample count, resolution, radius, steps…). Short diagrams or pseudo-code welcome.

## Breakdown
Where it sits in the frame and the exact steps:

1. Stage `<Stage>` — pass "<Pass name>": reads `<targets>`, writes `<targets>`.
2. …

| Resource | Format | Size | Notes |
|---|---|---|---|

## Optimization
- Cost drivers (resolution, samples, bandwidth, overdraw, CPU submission).
- Quality vs cost trade-offs per level, and what games typically ship.
- Tricks used here (half res, temporal accumulation, early-outs, bilateral upsample, LUTs…).

Measured cost at 1920×1080 (`gfxlab --bench-settings`), GPU: `<name / driver>`

| Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|

## Rebuild
How to implement it from scratch in another engine: data you need, step-by-step, shader sketch,
pitfalls (precision, reversed-Z, temporal ghosting, color space), and how to verify it works.

Files: `src/modules/<area>/<file>.cpp`, `shaders/<area>/<file>.glsl`
