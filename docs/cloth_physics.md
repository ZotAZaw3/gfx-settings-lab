# Cloth Physics and Physics/Destruction Quality

Menu: **Physics → Cloth Simulation** (`Off | Low | High`), **Physics Detail / Debris**
(`Low | Medium | High`)
Code: `GeoVfxSettings::cloth`, `physicsDetail`; shader `geometry/cloth_sim.comp`

## Definition

**Cloth physics** makes capes, flags, curtains and clothing move and collide
realistically instead of being rigidly skinned to a skeleton. **Physics detail /
destruction** controls how many dynamic objects, debris pieces and fracture chunks the
game simulates (broken walls, shattered glass, rubble from explosions) and how long they
persist.

## How it works

**Cloth (position-based dynamics / XPBD, the standard method):**
1. The cloth is a grid of particles connected by **constraints**: stretch (neighbours),
   shear (diagonals), bend (two apart).
2. Each step: predict `x* = x + v*dt + gravity*dt²`.
3. Iterate constraints a few times: for each constraint, move the two particles so the
   distance returns towards its rest length (weighted by inverse mass). XPBD adds a
   compliance term so stiffness does not depend on iteration count.
4. Collide against simple shapes (spheres/capsules of the character), push particles out.
5. `v = (x* - x) / dt; x = x*`.
6. Pinned particles (shoulders, flagpole) have inverse mass 0.
On the GPU each constraint set is coloured (graph colouring) so constraints of one colour
can be solved in parallel without conflicts. Wind is a force with noise, applied per triangle
using its normal.

**Destruction:** objects are pre-fractured (Voronoi) offline; at runtime chunks become
rigid bodies when hit. A quality setting caps the chunk count, the debris lifetime, and
whether small debris uses full physics or simple particles.

## Breakdown

| Cloth   | Grid       | Iterations | Self-collision |
|---------|------------|------------|----------------|
| Off     | skinned to skeleton (animated, no sim) | – | – |
| Low     | 16 × 16    | 4          | no             |
| High    | 48 × 48    | 12         | no (yes in film tools) |

Cost: `particles × iterations × constraints`. Cloth is often CPU-side in games (PhysX
cloth, Havok Cloth); destruction costs CPU (rigid-body solver) and also draw calls
for the chunks.

## Optimization

- Simulate at lower resolution and skin the render mesh to the sim mesh.
- Fixed timestep with substeps; skip simulation for off-screen/far characters (freeze or
  switch to animation).
- Use XPBD so fewer iterations still look stiff enough.
- Debris: put to sleep quickly, fade out after the lifetime, convert tiny pieces to
  particles.

## Rebuild (from scratch)

```glsl
// cloth_sim.comp: one invocation per particle, Jacobi-style distance constraints
layout(local_size_x = 64) in;
layout(std430, binding=0) buffer Pos  { vec4 pos[]; };   // w = inverse mass
layout(std430, binding=1) buffer Prev { vec4 prev[]; };
uniform int uW, uH; uniform float uDt, uRest; uniform vec3 uGravity, uWind;
uniform vec4 uSphere;                                     // collider xyz, r
void main(){
    int i = int(gl_GlobalInvocationID.x); if (i >= uW*uH) return;
    vec4 p = pos[i]; if (p.w == 0.0) return;
    vec3 v = p.xyz - prev[i].xyz;
    vec3 x = p.xyz + v*0.99 + (uGravity + uWind) * uDt*uDt;
    ivec2 c = ivec2(i % uW, i / uW);
    const ivec2 nb[4] = ivec2[](ivec2(1,0),ivec2(-1,0),ivec2(0,1),ivec2(0,-1));
    for (int k = 0; k < 4; ++k){
        ivec2 n = c + nb[k]; if (any(lessThan(n, ivec2(0))) || n.x>=uW || n.y>=uH) continue;
        vec3 q = pos[n.y*uW+n.x].xyz; vec3 d = x - q; float L = length(d);
        x -= 0.5 * (L - uRest) * d / max(L, 1e-5) * 0.5;
    }
    vec3 s = x - uSphere.xyz; float sl = length(s);
    if (sl < uSphere.w) x = uSphere.xyz + s / sl * uSphere.w;
    prev[i] = p; pos[i] = vec4(x, p.w);       // (use ping-pong buffers in real code)
}
```
Dispatch it `iterations` times per frame, then draw the grid as an indexed triangle mesh
reading positions from the SSBO in the vertex shader.
