# Shader Execution Reordering (SER)

Settings (Ray Tracing tab, "Advanced (documented)"): `Shader Execution Reordering / Ray
Sorting`, greyed out. Hardware SER needs RTX 40+ with NVAPI, Vulkan
`VK_NV_ray_tracing_invocation_reorder`, or DXR 1.2.

## Definition

GPUs run threads in groups of 32 (NVIDIA warp) or 32/64 (AMD wave) that must
execute the same instruction. Primary rays are coherent: neighbouring pixels
hit the same object with the same shader. After a bounce, neighbouring threads
hit **different objects with different materials**, so a warp ends up running
several material shaders one after another with most threads idle
(**divergence**). Memory access becomes scattered too.

**SER** lets the shader say "regroup threads by what they're about to do"
before shading. The hardware shuffles threads across warps so threads hitting
the same material run together. NVIDIA reports 20-40% speed-ups on path
tracing (up to 2x in heavy scenes); Cyberpunk RT Overdrive and Alan Wake 2
use it.

## How it works

With NVAPI HLSL extensions (DXR 1.0 style):

```hlsl
NvHitObject hit = NvTraceRayHitObject(scene, flags, mask, 0, 1, 0, ray, payload);
uint key = materialIdOf(hit);              // optional coherence hint
NvReorderThread(hit, key, /*numBits*/ 4);  // regroup threads
NvInvokeHitObject(scene, hit, payload);     // run closest-hit shader now coherent
```

The hit object separates *tracing* from *shading*, so reordering happens in
between. A coherence hint lets you sort by anything (material, Russian
roulette survival, ray direction octant).

DXR 1.2 (2025) standardises this as `HitObject` + `ReorderThread` in Shader
Model 6.9; AMD and Intel can implement it (or treat it as a no-op).

### Software emulation (what this app does)

Without SER hardware, compute renderers use **wavefront path tracing**:

1. Trace all rays of a bounce, write `(pixel, hitTri, material)` to a queue.
2. **Sort/bin** the queue by material id (counting sort, one pass per bounce).
3. Run one shading dispatch per material bin (or one dispatch over the sorted
   array) so warps see uniform materials.
4. Generate next-bounce rays into a new queue; repeat.

Our prototype has simple uniform materials (one shading function with
parameters), so sorting mostly helps **traversal** coherence: the `Ray
sorting` option sorts bounce rays by direction octant + origin Morton code.

## Breakdown

| Component | Cost / benefit |
|---|---|
| Hardware reorder | ~tens of µs overhead, saves 20-40% of shading time |
| Software sort per bounce | ~0.2-0.5 ms at 1080p, gains vary 0-30% |

## Optimization

- Only reorder **after the first bounce** (primary rays are already coherent).
- Use few key bits (2-4) – sorting on too many bits costs more than it saves.
- Combine with Russian roulette: include "will terminate" as a key bit so dead
  threads are packed together and whole warps retire early.

## Rebuild (from scratch)

Hardware: switch from `TraceRay` to the hit-object API, compute a key,
`ReorderThread`, `InvokeHitObject`. Software: convert the megakernel path
tracer into a wavefront loop with ray queues and a counting sort between
trace and shade.

Status in this app: documented; the `Ray sorting` toggle is greyed out in the
megakernel compute path.
