# Letterbox / Cinematic Mode

> Menu: **Post Processing → Lens & Screen → Letterbox** · Setting id(s): `post.letterbox` · Owner module: `post`
> Values: Off / 2.39:1 / 2.00:1 / 1.85:1 · Presets: not preset-driven

## Definition

Letterboxing draws black bars at the top and bottom of the screen so the
visible image has a wider, cinematic aspect ratio (2.39:1 is the "scope" film
standard). Games use it for cutscenes, photo modes, or as a stylistic option.
Pillarboxing is the vertical equivalent for ultrawide screens showing content
locked to 16:9.

## How it works

1. Target aspect `A`, screen aspect `S = w/h`. If `A > S`, bars go top and
   bottom with height fraction `(1 − S/A)/2` each; otherwise pillarbox
   left/right with `(1 − A/S)/2`.
2. In the PostLDR pass: `if (uv.y < bar || uv.y > 1-bar) color = barColor;`,
   with a 1-pixel smoothstep for an anti-aliased edge.
3. The bar fraction animates from 0 to target over the fade time when
   toggled (cutscene transitions).
4. Note: a *proper* letterbox also tells the UI module the safe area so
   subtitles sit inside the bars; the module exposes `letterboxFraction()`.

## Breakdown

| Piece | Where |
|---|---|
| Math | `postldr.frag` (`applyLetterbox`) |
| Animation | `PostModule::update` (CPU lerp) |

## Optimization

Free in the uber pass. A real optimization: shrink the *render viewport* to
the visible area (scissor), saving 25% of shading at 2.39:1 on 16:9. Not done
here because it changes the camera projection that other modules use.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.letterbox` | Off | +0.7 | +2.8 | +0.0 | n/a |
| `post.letterbox` | 2.39:1 (scope) | +29.9 | +26.0 | +0.0 | n/a |
| `post.letterbox` | 2.00:1 | +0.1 | +0.1 | +0.0 | n/a |
| `post.letterbox` | 1.85:1 | +4.3 | +2.5 | +0.0 | n/a |

## Rebuild (from scratch)

```glsl
float S = uScreen.x/uScreen.y;
float bar = max(0.0, (1.0 - S/uTargetAspect)*0.5) * uFade;
float px = 1.0/uScreen.y;
float m = smoothstep(bar-px, bar, uv.y) * smoothstep(bar-px, bar, 1.0-uv.y);
color = mix(uBarColor, color, m);
```

Files: `src/modules/post/post_module.cpp` (update, runLdr), `shaders/post/postldr.frag`
