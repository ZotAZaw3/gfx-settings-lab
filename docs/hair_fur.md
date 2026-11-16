# Hair and Fur Quality (Strand Hair)

> Menu: **Characters → Fur Quality** · Setting id: `characters.furQuality` (Off / Low / Medium / High = 0 / 8 / 16 / 32 shells) · Owner module: `geometry.characters`
> Presets: Off, Low, Medium, High
> Files: `src/modules/geometry/characters.cpp`, `shaders/geometry/fur.{vert,frag}`

## Definition

Hair quality selects how character hair and animal fur are represented and simulated:
from cheap textured **hair cards** (strips of polygons with a hair texture), up to
tens of thousands of individually simulated **strands** (AMD TressFX, NVIDIA HairWorks,
Unreal Groom). Fur can also be faked with **shells**: the mesh is drawn several times,
each copy pushed out along the normal and showing fewer hair pixels.

## How it works

**Hair cards.** 50–200 alpha-tested or alpha-blended polygon strips with painted hair.
Lit with an anisotropic model (Kajiya–Kay / Marschner) so highlights stretch along the
hair direction. Problems: transparency sorting and aliasing.

**Strand hair.**
1. Each hair is a polyline of 8–32 vertices; 20 k–100 k guide or render strands.
2. Simulation in a compute shader per strand: Verlet integration, gravity, wind,
   length constraints (keep segment lengths), shape constraints (pull back to the groom),
   collision against capsules on the head/body. Often only *guide* strands are simulated
   and others are interpolated.
3. Rendering: lines expanded to camera-facing ribbons, ~1 px wide. Needs anti-aliasing
   (coverage computed analytically) and **order-independent transparency** (per-pixel
   linked lists or k-buffer) because hair is semi-transparent and overlaps itself.
   Self-shadowing with deep opacity maps.
4. Newer engines rasterize strands in a **compute software rasterizer** (UE5 Groom,
   TressFX 5) because hardware rasterization of 1-px lines is inefficient.

**Shell fur (used in this prototype).** Draw the mesh `N` times; shell `i` is
offset by `i/N * furLength` along the normal. A noise texture decides per texel whether
a hair passes through; outer shells keep fewer texels (thinner tips). Gravity/wind
bends outer shells more. Cheap and convincing for short fur and grass-like carpets.

## Breakdown

**In this prototype:**

1. Stage `GBuffer`, order 20 — "Characters": the fur ball on the plaza is one instanced draw of
   `shells + 1` instances (instance 0 = skin). Strand hair and cards are explained below for reference; the
   prototype implements the shell technique only.


| Mode          | Representation        | Cost driver                         |
|---------------|-----------------------|-------------------------------------|
| Cards         | ~2–10 k triangles     | alpha overdraw                      |
| Shells (N)    | mesh × N draws        | N × pixels covered (overdraw)       |
| Strands Low   | ~20 k strands, guides simulated | compute sim + line raster + OIT |
| Strands High  | ~100 k strands, full sim, shadows | 2–6 ms per character (!)   |

## Optimization

- LOD by screen size: strands up close, cards at mid distance, baked texture far away.
- Simulate guides, interpolate render strands.
- Reduce strand count and increase width with distance (keeps density).
- Shells: fewer shells far away; use fins at silhouettes.
- Use TAA to resolve sub-pixel strand aliasing instead of heavy per-strand AA.

### Measured cost (llvmpipe, relative only)

Measured with `gfxlab --bench-settings` on Mesa llvmpipe (a CPU software renderer), 640×360, 5 warm-up + 30 measured frames per value, showcase camera on the terrain overview. Each row is the change versus the High preset with only that one setting moved. On llvmpipe the GPU work runs on the CPU, so GPU and CPU columns move together, and two runs of the same configuration differed by up to ~35 ms, so treat anything under ~30 ms as noise. Use the table for direction and rough ratios only; rerun `--bench-settings` on real hardware for actual numbers. This feature sits on the plaza, which is small or off-screen from that camera, so these rows are mostly noise; set `world.showcaseCamera` to the plaza or close-up view before benchmarking it on real hardware.

<!-- bench: characters.furQuality -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits  geometry` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits__geometry.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Fur Quality | Off | -6.990 | +2.486 | +2.502 | +0.0 | +0.0 |
| Fur Quality | Low | +20.024 | +24.795 | +24.794 | +0.0 | +0.0 |
| Fur Quality | Medium | +47.003 | +49.909 | +49.938 | +0.0 | +0.0 |
| Fur Quality | High | +24.843 | +22.930 | +22.943 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild (from scratch, shell fur)

```glsl
// fur_shell.vert
uniform float uShell, uShellCount, uFurLen; uniform vec3 uGravity;
void main(){
    float t = uShell / uShellCount;
    vec3 p = aPos + aNormal * uFurLen * t + uGravity * t * t * uFurLen;
    vT = t; vUV = aUV * 8.0; gl_Position = uViewProj * uModel * vec4(p,1);
}
// fur_shell.frag
void main(){
    float n = hash12(floor(vUV * 64.0));          // one hair per cell
    if (vT > 0.0 && n < vT) discard;              // hairs thin out towards the tip
    float ao = mix(0.4, 1.0, vT);                 // roots darker
    gAlbedo = vec4(furColor * ao, 1.0);
}
```
C++: `for (int i = 0; i < shells; ++i) { setUniform("uShell", float(i)); drawMesh(); }`
(or one instanced draw with `gl_InstanceID` as the shell index).
