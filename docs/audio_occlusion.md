# Sound occlusion

> Menu: **Audio → 3D Audio → Sound occlusion** · Setting id: `audio.3d.occlusion` · Owner module: `audio`
> Values: Off, Single ray, Multi ray (5) · Presets: Low=Single, Medium=Single, High=Multi, Ultra=Multi

## Definition
When a wall is between you and a sound, it should get quieter and muffled, like voices through a
door. Occlusion traces lines from your head to each sound to find out. It is what lets you tell
"enemy behind the wall" from "enemy in the open".

## How it works
**Game thread (rays).** `AudioSystem::update` re-tests a round-robin batch of voices each frame
(24 voices per frame with Multi ray, 64 with Single ray), so cost stays bounded no matter how many
sounds play. Each test is a segment from the listener to the source against the scene:

- **Single ray**: one segment; result is 0 (clear) or 1 (blocked).
- **Multi ray**: five segments to the source and to 4 points 0.6 m around it (perpendicular to the
  line of sight). Result = fraction blocked, so a source half around a corner gets 0.4–0.6.

The scene query here is a slab test against axis-aligned boxes (`addOccluderBox`; the demo puts an
invisible 6 m wall at z = −9 between you and the orbiting hum) plus an optional engine callback
(`setRayQuery`) that can call a physics/BVH ray cast and return material transmission.

**Audio thread (filter).** The occlusion value is smoothed per block (`occ += (target − occ)·0.2`)
and applied as:

```
gain   *= lerp(1.0, 0.3, occ)                 // ≈ −10 dB when fully blocked
cutoff  = 20 kHz · (600 Hz / 20 kHz)^occ       // log interpolation, 600 Hz when blocked
reverb send *= 1 + 0.5·occ                    // you hear the room more than the source
```

Terminology: **occlusion** = fully enclosed (both direct and reverb muffled); **obstruction** =
direct path blocked but the room is shared (only direct muffled). This prototype does a blend of the
two. Full solutions (Steam Audio, Project Acoustics, Wwise Spatial Audio) also find **diffraction**
paths around corners and **portals** between rooms.

## Breakdown
1. Game thread, `AudioSystem::update`: ray tests for the next batch → `VoiceDesc::occlusion`.
2. Audio thread, stage **Spatialize**: smoothing, gain, cutoff, reverb send.
3. Audio thread, stage **Filter**: one-pole low-pass (shared with air absorption).

## Optimization
Measured (`tools/audio_bench`; the ray column is the game-thread time to test every 3D voice once):

| Value | Audio CPU (% of 1 core) | Ray tests, 67 voices (µs) | RAM |
|---|---|---|---|
| Off | 6.73 | 0.2 | 0 |
| Single ray | 6.47 | 0.8 | 0 |
| Multi ray (5) | 6.29 | 4.2 | 0 |

In the running app with the round-robin budget the ray work is ≈ 0.005 ms per frame (Audio stats
window). Δ GPU 0, Δ RAM 0. The audio-thread filter cost is shared with air absorption (Filter stage
≈ 1.5%), so turning occlusion off saves nothing there unless air absorption is off too.

Ray tests against real level geometry (thousands of triangles, BVH) cost 1–10 µs each, so in a full
game Multi ray × 100 voices × every frame would be 0.5–5 ms: hence round-robin batches, lower update
rates for distant voices, and caching results per emitter.

## Rebuild
1. Pick a ray query you already have (physics raycast, BVH, voxel grid).
2. Every frame, test a fixed budget of voices (nearest/loudest first), store a smoothed 0–1 value.
3. On the audio side map it to gain + low-pass cutoff (log-interpolated) and a reverb send boost.
4. Multi-ray or cone sampling for partial occlusion; per-material transmission (glass ≈ 0.7, concrete ≈ 0.1).
5. Verify: walk behind a wall while a loop plays; it should dull smoothly, not snap.

Files: `src/modules/audio/audio_system.cpp` (`computeOcclusion`, `update`), `audio_mixer.cpp` (`renderVoice`), `audio_demo.cpp` (wall).
