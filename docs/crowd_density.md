# Crowd / Population Density

> Menu: **World → Crowd Density** · Setting id: `world.crowdDensity` (Low / Medium / High / Ultra = 25 / 75 / 200 / 500 agents) · Owner module: `geometry.characters`
> Presets: Low…Ultra
> Files: `src/modules/geometry/characters.cpp`, `shaders/geometry/crowd.vert`

## Definition

Crowd density (also "population density", "NPC density", "traffic density") sets how
many pedestrians, vehicles and animals the game spawns around the player. It is mostly
a **CPU** setting: every agent needs AI, pathfinding, animation and physics, plus a draw
call or instance.

## How it works

1. **Spawning.** The game keeps a target count around the player
   (`density × zoneBaseCount`) and spawns/despawns agents outside view to reach it.
2. **Simulation LOD.** Near agents get full AI (perception, navmesh paths, collision
   avoidance via RVO/ORCA). Far agents follow simple flow fields or splines, or freeze.
3. **Animation LOD.** Near agents: full skeletal blending on the CPU/GPU. Mid: update the
   skeleton every 2–4 frames. Far: **vertex animation textures (VAT)**, where animation
   frames are baked into a texture and the vertex shader reads the pose; thousands of
   characters for the cost of instancing.
4. **Rendering LOD.** Mesh LODs → impostor billboards at long range; all instanced.

## Breakdown

**In this prototype:**

1. Stage `GBuffer`, order 20 — one instanced capsule draw; each agent's position is a closed-form function of
   (id, time) evaluated at t and t − dt in the vertex shader, so motion vectors are correct for TAA/motion blur.
2. In a real game the CPU (AI, pathfinding, animation) dominates; the prototype shows only the GPU side.


| Level  | Agents near player | Full-AI radius | Animation update     |
|--------|--------------------|----------------|----------------------|
| Low    | 50                 | 15 m           | every 4th frame far  |
| Medium | 150                | 25 m           | every 2nd frame far  |
| High   | 400                | 40 m           | every frame          |
| Ultra  | 1000               | 60 m           | every frame          |

CPU: AI + animation (often the bottleneck in city games, especially on many-core
imbalanced CPUs). GPU: vertex skinning + overdraw; shadows of all those characters.

## Optimization

- Instancing + VAT for distant crowds.
- Job-system parallel AI; time-slice updates (each agent thinks every N frames).
- Despawn behind the player; spawn just outside view.
- Share animation poses between similar agents (pose caching).

## Rebuild (from scratch)

The prototype uses simple instanced capsules walking on circles:
```glsl
// crowd.vert — instance i walks around a circle, bobbing with a baked "walk" cycle
uniform float uTime;
void main(){
    float id = float(gl_InstanceID);
    float r = 8.0 + mod(id * 7.31, 40.0), sp = 0.2 + fract(id * 0.137) * 0.3;
    float a = id * 2.399 + uTime * sp;
    vec3 base = vec3(cos(a) * r, 0.0, sin(a) * r);
    float bob = abs(sin(uTime * 6.0 + id)) * 0.05;
    vec3 p = aPos + base + vec3(0, bob, 0);
    gl_Position = uViewProj * vec4(p, 1);
}
```
`glDrawElementsInstanced(GL_TRIANGLES, capsuleIndexCount, GL_UNSIGNED_INT, 0, count)` with
`count` from the table. Turning density up shows the GPU side cheaply scaling while in
a real game the CPU (AI/animation) would dominate.
