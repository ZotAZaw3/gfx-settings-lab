# Async Compute (info: not available on GL 4.5 / D3D11)

> Menu: not exposed (explained here because many games list it) · Owner: core

## Definition
Async compute lets the GPU run compute work (SSAO, particle simulation, light culling, post effects) **at
the same time** as graphics work from a separate hardware queue, filling execution units that would
otherwise sit idle during shadow-map or depth-only rendering. Games that offer the toggle (Doom Eternal,
Forza, many UE5 titles) typically gain 5–15 % on GPUs with good support (AMD GCN/RDNA, NVIDIA Turing+).

## How it works
D3D12 and Vulkan expose multiple queues: a graphics (direct) queue and one or more compute queues. The
engine submits, for example, the shadow-map pass on the graphics queue and the SSAO of the previous
stage on the compute queue, synchronized with fences. The GPU scheduler interleaves wavefronts from both.
Overlap only helps when the two workloads stress different units (shadow rendering is rasterizer/ROP
bound, SSAO is ALU/texture bound).

Why it isn't a real toggle here: OpenGL 4.5 and D3D11 have a single implicit queue; the driver may
overlap independent work on its own, but the application can't schedule it. A D3D12/Vulkan backend
would be needed (this prototype's RHI keeps passes as independent callbacks with explicit resources,
which is the prerequisite for a render graph that could assign passes to queues).

## Breakdown (how it maps in a D3D12/Vulkan version of this engine)
1. Tag passes as `async` (e.g. AO, particle simulation, light clustering).
2. Render graph splits the frame: graphics queue runs Shadow + G-buffer while the compute queue runs AO
   for the **previous** stage's inputs; fences at the join points.
3. Resources shared between queues need queue-ownership transfers (Vulkan) or common state (D3D12).

## Optimization
- Gains depend on overlap and on the GPU; poorly paired workloads can get *slower* from cache thrashing.
- Profile with GPU-level tools (PIX timing captures, Radeon GPU Profiler, Nsight Systems).
- Not measurable in this app; see the D3D12 docs for numbers on real hardware.

## Rebuild
1. Create a compute queue; record async passes into its command lists.
2. Signal/wait fences between producer and consumer passes across queues.
3. Pick pairs of passes with complementary bottlenecks; measure with and without.
