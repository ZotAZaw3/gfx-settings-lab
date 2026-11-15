# Frame Generation (DLSS FG / Multi Frame Gen, FSR 3 FG, interpolation)

> Menu: **Anti-Aliasing & Upscaling → Frame Generation, Optical Flow Search** · Setting id(s): `aa.framegen`, `aa.framegen.flow_radius`, `aa.framegen.generated_only` (debug) · Owner module: `aa.framegen`
> Values: Off / 2x Interpolation / DLSS FG* / FSR 3 FG* (greyed out); radius 0-4 · Presets: not in presets (like games, it's opt-in)

Settings shown in the menu: `Frame Generation: Off | 2x Interpolation (ours) | DLSS FG* | FSR 3 FG*`, `Debug: Show Generated Frames Only`, `Optical Flow Quality`

## Definition

Frame generation inserts synthetic frames between real rendered frames, raising the displayed frame rate (e.g. 60 rendered → 120 displayed) without rendering more scenes. Motion looks smoother, but input latency does not improve, it gets slightly worse (the newest real frame must be held back to interpolate toward it). That's why it's always paired with a latency reducer (Reflex / Anti-Lag, see `latency_sync.md`).

- **DLSS 3 FG** (RTX 40+): hardware optical-flow accelerator + game motion vectors + neural network. **DLSS 4 Multi Frame Generation** (RTX 50): up to 3 generated frames per real one, flow predicted by the network.
- **FSR 3 FG** (any DX12/Vulkan GPU): compute-shader optical flow + game motion vectors, analytic interpolation.
- **Extrapolation** (e.g. Intel's research, VR "asynchronous spacewarp"): predict a *future* frame from the past instead of interpolating; no added latency, more artefacts.

## How it works

Given real frames N-1 and N, generate frame N-½:

1. **Motion sources:**
   - *Game motion vectors* (exact for geometry, wrong for shadows, reflections, particles, UI);
   - *Optical flow* computed from the two colour images (catches shadows/reflections, noisy in flat areas).
2. **Flow fields:** convert motion to two half-way fields: for each pixel of the in-between frame, where to sample in N-1 and in N. Done by *forward splatting* the motion vectors to the mid-point (each source pixel writes its half-way position; conflicts resolved by depth, nearest wins), then filling holes.
3. **Warp both frames** to t = 0.5 and **blend** with weights from disocclusion masks: a region that just appeared in N must come only from N, one that disappeared only from N-1.
4. **UI:** the HUD is rendered separately and composited onto the generated frame without warping (otherwise text tears). Our engine's `UI` stage runs after `FrameGen` for exactly this reason.
5. **Pacing:** present generated and real frames at even intervals: real N is queued, interpolated N-½ shown first, then N half a frame-time later. Uneven pacing makes FG feel worse than no FG.

## Breakdown

| Stage | Ours | DLSS FG | FSR 3 FG |
|---|---|---|---|
| Motion | game MVs + block-matching optical flow (compute) | game MVs + HW optical flow | game MVs + compute optical flow |
| Interpolator | analytic warp + blend | neural network | analytic |
| Added latency | ~1 rendered frame | ~1 frame (offset by Reflex) | ~1 frame (offset by Anti-Lag) |
| Recommended base FPS | 60+ | 60+ (40+ OK) | 60+ |
| API | OpenGL/D3D11 | D3D12/Vulkan (Streamline) | D3D12/Vulkan (FidelityFX) |

Neither DLSS FG nor FSR 3 FG supports OpenGL or D3D11, so on this prototype's main path only our educational interpolator runs; the SDK hooks exist for a future D3D12 backend.

Code: `src/modules/aa/frame_gen.cpp`, `shaders/aa/fg_optical_flow.frag` (3x3 block matching on luma at quarter resolution, searching around the game motion vector), `fg_interpolate.frag` (gather interpolation with a fixed-point motion search and depth-based disocclusion). The module keeps the previous final (pre-UI) frame; in "2x" mode the core presents twice per rendered frame (generated then real) with pacing via `glfwSwapInterval`/timer.

## Optimization

- Run optical flow at quarter resolution on luma only; refine motion vectors with it only where they disagree (shadows, reflections).
- Use game motion vectors as the starting guess for the flow search (much smaller search window).
- Disable FG under 40 base fps: artefacts last long enough to be seen and latency gets too high.
- Turn on Reflex/Anti-Lag automatically with FG.
- With V-Sync off and VRR on, cap the *output* rate just below the refresh rate.
- Skip generation (just repeat) on camera cuts, detected by a flow confidence drop.

### Measured quality

Measured with `tests/aa/quality_test.py` (512x288 procedural scene: thin lines, a moving rotated square, HDR highlights; Mesa llvmpipe). PSNR against a 16x16 supersampled reference, higher is better.

Object moving 6 px/frame, generated frame compared against the true rendered mid-frame (share of pixels visibly wrong, lower is better):

| Method | Wrong pixels |
|---|---|
| Repeat previous frame (no FG) | 0.88% |
| Naive 50/50 blend of N-1 and N | 1.46% (double image) |
| Our interpolation | 0.43% (thin seams at the object's edges) |

The optical-flow refinement only overrides motion vectors where they clearly fail; letting block matching win more often made results worse, because block matching is unreliable on flat and repetitive areas.

## Rebuild (from scratch)

Simplest interpolator using game motion vectors (`fg_interpolate.frag` reference path):

```glsl
// velocity at frame N, converted to UV (current minus previous)
vec2 mv   = velocityToUV(texture(uVelocityN, uv).rg);
// a surface moved by mv between N-1 and N, so at t=0.5 it is half way:
vec2 uvPrev = uv - 0.5 * mv;          // where to read in N-1
vec2 uvCurr = uv + 0.5 * mv;          // where to read in N (approximation: uses mv sampled at uv)
vec3 a = texture(uFrameNm1, uvPrev).rgb;
vec3 b = texture(uFrameN,   uvCurr).rgb;
// disocclusion: if the depths at both reads disagree strongly, trust the nearer one
float za = texture(uDepthNm1, uvPrev).r, zb = texture(uDepthN, uvCurr).r;
float w = clamp(0.5 + (zb - za) * uDepthSharpness, 0.0, 1.0);  // reversed-Z: larger = nearer
o = vec4(mix(a, b, w), 1);
```

Steps:
1. Keep the last frame's final colour, depth and velocity (before UI).
2. Implement the gather interpolator above; display only generated frames in debug mode to see artefacts.
3. Improve mid-point motion: our shader iterates `m = flow(uv + 0.5*m)` a few times (fixed-point search), a cheap stand-in for forward splatting (atomicMax on packed depth+index), which is what production interpolators use.
4. Add optical flow for non-geometric motion.
5. Present real and generated frames with even pacing; measure with a frame-time graph.
