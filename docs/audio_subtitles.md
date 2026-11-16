# Subtitles and closed captions

> Menu: **Audio → Subtitles → Subtitles / Closed captions / Speaker names / Subtitle size / Subtitle background** · Setting ids: `audio.subtitles.enabled`, `audio.subtitles.captions`, `audio.subtitles.speaker_names`, `audio.subtitles.size`, `audio.subtitles.background` · Owner module: `audio`
> Values: on/off · size 0.75–2.5× · background 0–1 · Presets: none

## Definition
**Subtitles** show spoken dialogue as text. **Closed captions** also describe meaningful sounds,
like `[Explosion]` or `[Car passing]`, with an arrow when they come from the side. **Speaker names**
prefix each line with who is talking. Size and background box keep text readable on any scene.

## How it works
Dialogue is played with `AudioSystem::playVoiceLine(sound, params, speaker, text, seconds)`: the voice
goes to the Voice bus and a `Subtitle` entry is added with the same duration. Sounds played with a
`caption` add a caption entry (2.5 s). At most 4 lines are kept; each has a countdown and fades out
over its last 0.3 s.

`AudioModule::drawSubtitles` renders bottom-centre with ImGui's foreground draw list:

- font size `= base × 1.4 × Subtitle size`, wrapped at 60% of the screen width;
- speaker name in amber, dialogue in white, captions in light blue;
- a rounded box behind each line with opacity = Subtitle background;
- captions with a position get `<` or `>` when the sound is more than 30° to the side.

Subtitles live in the audio module because they are driven by audio events and must stay in sync
with playback; the text itself would come from a localization table in a real game.

## Breakdown
1. Game thread: `playVoiceLine` / `play(caption)` → `subtitles_`; `update(dt)` counts down.
2. UI (`onGui`): filter by enabled/captions, compute layout, draw boxes + text.

## Optimization
Subtitles cost a few ImGui text draws per frame: no measurable change in frame CPU or GPU ms,
Δ RAM < 1 KB (strings). The design constraints are readability, not performance:

- Minimum size around 46 px at 1080p for TV distance (Xbox Accessibility Guidelines); default here is
  ~25 px at 1×, so offer at least 2×.
- 2 lines max, ~40 characters per line, ≥ 1.5 s per line.
- A background box (≥ 50% opacity) beats outlines for contrast over bright scenes.
- Name the speaker when they are off-screen; describe sounds that carry gameplay information.

## Rebuild
1. Store lines with speaker id, text key, start time and duration, triggered by the same event that
   starts the voice.
2. Render on top of everything (after tone mapping and post-processing) in screen space.
3. Scale with UI scale and the user's size option; wrap; clamp the number of lines.
4. For captions, tag sounds with a label and a minimum importance; add direction from the listener basis.
5. Verify: turn everything on, set size 2.5× and background 0, and check legibility on the
   brightest part of the scene.

Files: `src/modules/audio/audio_module.cpp` (`drawSubtitles`), `audio_system.cpp` (`playVoiceLine`, `showSubtitle`), `audio_demo.cpp` (lines).
