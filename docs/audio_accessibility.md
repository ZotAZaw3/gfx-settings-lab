# Audio accessibility: mono audio, balance, sound visualization

> Menu: **Audio → Accessibility → Mono audio / Left/right balance / Visualize sound effects** · Setting ids: `audio.access.mono`, `audio.access.balance`, `audio.access.visualize` · Owner module: `audio`
> Values: Mono on/off · Balance −1 (left) … +1 (right) · Visualize on/off · Presets: none

## Definition
Options for players with hearing loss in one ear, one earbud, or deaf/hard-of-hearing players.
**Mono audio** plays the same mix in every speaker so nothing is missed on one side. **Balance**
shifts the mix toward the stronger ear. **Visualize sound effects** draws on-screen indicators that
show where important sounds come from (Fortnite, Apex and The Last of Us Part II popularised this).

## How it works
**Mono.** In stage Master, for each frame: `m = average of all non-LFE channels`, then every
non-LFE channel = `m`. This happens after panning, so directional cues are lost by design; HRTF
becomes pointless with mono on.

**Balance.** Each speaker is left or right by its azimuth. `balance > 0` scales left speakers by
`1 − balance`; `balance < 0` scales right speakers by `1 + balance`. Center and LFE are untouched.

**Sound visualization.** Sounds played with a caption (`PlayParams::caption`, e.g. "[Explosion]",
"[Car passing]") create a `SoundEvent` with world position. The UI (`AudioModule::drawVisualization`)
draws a ring around screen centre and, for each recent event (3 s), an arc and label at the event's
listener-relative azimuth, fading with age. Direction is computed with the same listener basis as the
audio, so it always matches what you would hear.

## Breakdown
1. Stage **Master** (audio thread): mono → balance → master gain.
2. Game thread: captioned `play()` → `events_`.
3. UI (`onGui`): `ImGui::GetForegroundDrawList()` ring, arcs and labels.

## Optimization
Measured (`tools/audio_bench`, 69 voices):

| Value | Audio CPU (% of 1 core) | Master stage | RAM |
|---|---|---|---|
| Mono off | 6.55 | 0.111 | 0 |
| Mono on | 6.65 | 0.109 | 0 |

Mono and balance are a few multiply-adds per frame: free. Visualization is a handful of ImGui draw
calls per frame (no measurable GPU or CPU change at 1080p in the overlay). Δ VRAM 0, Δ RAM 0.

## Rebuild
1. Implement mono as a final down-mix stage (after spatialization, before the limiter).
2. Balance as per-side gains on the final mix.
3. Tag gameplay-relevant sounds (footsteps, gunfire, vehicles, alarms) with a label and importance.
4. Draw indicators from the listener-relative direction; size or opacity from loudness; fade over 2–3 s.
5. Follow the Game Accessibility Guidelines (gameaccessibilityguidelines.com): separate volume per
   category, mono, subtitles, visual cues for all gameplay-critical audio.

Files: `src/modules/audio/audio_mixer.cpp` (master stage), `audio_module.cpp` (`drawVisualization`), `audio_system.cpp` (`play`, events).
