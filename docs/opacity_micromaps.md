# Opacity Micromaps (OMM) and Displacement Micromaps (DMM)

Settings (Ray Tracing tab, "Advanced (documented)"): `Opacity Micromaps`, greyed out.
Hardware OMM needs RTX 40+ / DXR 1.2 / Vulkan `VK_EXT_opacity_micromap`. The compute BVH
treats alpha-masked geometry as opaque.

## Definition

Foliage, fences, chain-link and hair are modelled as a few triangles with an
**alpha-tested texture** (cut-outs). For ray tracing this is the worst case:
the BVH says "the ray hits this triangle", but whether it really hits depends
on a texture lookup, so the RT hardware has to **stop and run an any-hit
shader** on the shader cores for every candidate leaf, then resume traversal.
A ray through a tree canopy can invoke dozens of any-hit shaders.

**Opacity micromaps** pre-bake the alpha mask onto each triangle as a grid of
micro-triangles (subdivision levels up to 12 → 4^12 micro-triangles), each
marked **opaque**, **transparent** or **unknown**. The RT hardware resolves
opaque and transparent micro-triangles on its own, and only calls the any-hit
shader for the "unknown" ones along the mask's edges. NVIDIA reports up to 2x
faster RT in foliage-heavy scenes.

**Displacement micromaps** (Ada) do the same for geometry detail: a base
triangle + baked displacement, decoded by RT hardware, so you can ray trace
highly tessellated surfaces without storing millions of triangles in the BVH.

## How it works

1. Offline (or at load): for each alpha-tested triangle, rasterise its UV
   footprint into micro-triangles, sample the alpha texture, classify each as
   opaque (all alpha ≥ cutoff), transparent (all < cutoff) or unknown.
   2-state (1 bit) or 4-state (2 bits) formats.
2. Build an OMM array (`VkMicromapEXT` / D3D12 OMM array) and reference it
   from the BLAS geometry, with per-triangle indices into the array.
3. At trace time the hardware walks into the micromap after a triangle hit.

NVIDIA's OMM SDK (`omm-sdk`) does the baking, including GPU baking.

## Breakdown

| Mode | Cost through foliage |
|---|---|
| Treat as opaque | cheapest, but wrong silhouettes (leaf "cards" visible in shadows) |
| Full any-hit | correct, slowest (shader call per candidate) |
| OMM 4-state | correct, near-opaque speed |

## Optimization

- Use 4-state OMM with a low subdivision level (e.g. 6-8); unknown only on
  edges.
- Exclude foliage from some ray types via instance masks (e.g. reflection rays
  see foliage as opaque proxies).
- For GI/AO rays, an opaque approximation is often acceptable.

## How it applies to the compute path (this app)

The compute BVH has no hardware traversal to protect, but the same logic
applies: every leaf hit on an alpha-tested triangle costs a texture fetch +
branch. A software equivalent is a **per-triangle opacity bitmask** (e.g.
64-bit = 8×8 micro-grid in barycentric space) stored next to the triangle:
look up the bit for the hit's barycentrics; only fall back to the texture on
"unknown" cells.

## Rebuild (from scratch)

Hardware: bake with the OMM SDK → create the micromap → attach to BLAS
triangle geometry → done (no shader changes). Software: bake a bitmask per
alpha-tested triangle, test it in `traceAny/traceClosest` before accepting a
hit.

Status in this app: documented; the prototype scene's materials are opaque,
so the toggle is greyed out.
