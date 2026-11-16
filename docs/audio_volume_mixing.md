# Volume sliders and mixer buses

> Menu: **Audio → Volume** · Setting ids: `audio.volume.master`, `audio.volume.music`, `audio.volume.sfx`, `audio.volume.voice`, `audio.volume.ambient`, `audio.volume.ui`, `audio.volume.mute` · Owner module: `audio`
> Values: 0–100% each, Mute on/off · Presets: none (personal preference, never touched by quality presets)

## Definition
Separate loudness for each category of sound (music, sound effects, dialogue, ambience, menu
sounds) plus one master slider. It lets a player turn the music down to hear footsteps, or push
dialogue up over loud combat.

## How it works
Every sound is played on a **bus** (a sub-mix). The mixer adds all voices of a bus into one
buffer, multiplies that buffer by the bus volume, and sums the buses into the master. The master
volume is applied at the very end, after the compressor, so it acts like the volume knob on your
speakers and does not change how the compressor behaves.

Sliders are stored as linear gain 0–1 (`percent / 100`). Perceived loudness is roughly
logarithmic, so 50% is −6 dB, 25% is −12 dB. Many games map the slider through a curve
(e.g. `gain = pct²` or a dB range of −60…0) so the bottom half of the slider is usable; this
prototype keeps it linear so the numbers in the stats window match the slider.

Every gain change is ramped linearly across one 256-frame block (5.3 ms at 48 kHz). Jumping a gain
instantly produces a click ("zipper noise") because the waveform gets a step discontinuity.

Mute sets the master target to 0, but the slider values are kept, so un-muting restores them.

## Breakdown
1. Voices write into `busBuf_[bus][channel][256]` (`Mixer::renderVoice`).
2. Stage **Bus mix**: `master[c][k] += bus[b][c][k] * lerp(prevGain[b], gain[b], k/n)` for each of 5 buses.
3. Stage **Master**: `out *= lerp(prevMaster, master × backgroundGain, k/n)` then the safety limiter.

| Resource | Format | Size | Notes |
|---|---|---|---|
| Bus buffers | float, planar | 5 buses × channels × 256 | 10 KB stereo, 40 KB at 7.1 |
| Master buffer | float, planar | channels × 256 | |

## Optimization
- Cost is one multiply-add per sample per bus: **Bus mix ≈ 0.02% of one core** in every run, and the
  master stage ≈ 0.1% (it also holds the limiter and meters). Volumes have no measurable cost and use
  no extra RAM; there is nothing to optimise, which is why games expose them freely.
- A bus at 0 could skip its voices entirely; real engines do this ("bus mute → virtualize"). The
  voice selector here already multiplies bus volume into each voice's audibility score, so a muted
  bus's voices are the first to go virtual when the voice limit is reached.

Measured (`tools/audio_bench`, cloud container, 4 vCPU): Bus mix stage 0.017% of one core, Master
0.10–0.11%. Δ GPU 0, Δ VRAM 0, Δ RAM 0.

## Rebuild
1. Give every sound a bus id when it starts.
2. Mix voices into per-bus float buffers, then sum buses × bus gain into the master buffer.
3. Ramp every gain over a block (store the previous gain per bus).
4. Apply master gain last, then a limiter or hard clip.
5. Optional: nested buses (SFX → Weapons, Footsteps…) are just more of the same tree; ducking
   (lowering music while dialogue plays) is a side-chain: the Voice bus level drives Music bus gain.

Verify: play a loop, drag the slider fast; you should hear smooth changes with no clicks.

Files: `src/modules/audio/audio_mixer.cpp` (`renderChunk`, bus + master stages), `audio_module.cpp` (sliders).
