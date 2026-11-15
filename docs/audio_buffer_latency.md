# Audio buffer size and latency

> Menu: **Audio → Latency → Audio buffer size / Buffer count** · Setting ids: `audio.output.buffer`, `audio.output.periods` · Owner module: `audio`
> Values: 64, 128, 256, 480, 512, 1024, 2048, 4096 frames · 2–4 buffers · Presets: none

## Definition
The sound card plays from a small queue of buffers that the game keeps filled. Smaller buffers mean
the time from "gun fires" to "you hear it" is shorter, but the game has to wake up more often and
must never be late, or you hear crackles and pops. Larger buffers are safer but laggy. Rhythm
games, music apps and competitive shooters care most; this is the audio twin of the
graphics low-latency mode (Reflex / frame queue).

## How it works
The device asks for one **period** (`bufferFrames`) at a time and keeps `periods` of them queued.
Buffer latency is:

```
latency_ms = bufferFrames × periods / sampleRate × 1000
480 × 2 / 48 000 = 20 ms       128 × 2 = 5.3 ms       4096 × 2 = 171 ms
```

Add the game's own delay (the sound is triggered during the frame, picked up at the next callback)
and the OS/driver/DAC latency (Windows shared mode ≈ 10 ms, Bluetooth 100–250 ms).

Each callback has fixed overhead: thread wake-up, `sync` (copying 512 voice descriptions under the
lock), voice selection. Smaller periods pay it more often per second. The mixer itself works in
256-frame chunks regardless of period size, so a 4096 period is processed as 16 chunks.

If the callback ever takes longer than one period (another thread hogs the CPU, a page fault, a
lock), the device runs dry: an **underrun**, heard as a click. Rules on the audio thread: no
allocation, no file I/O, no blocking locks (this module only `try_lock`s), no logging.

## Breakdown
1. `openDevice`: `cfg.periodSizeInFrames`, `cfg.periods`, `performanceProfile = low_latency`.
2. The OS may round the request; the actual values are read back from
   `internalPeriodSizeInFrames/internalPeriods` and shown in Audio stats ("period 480 × 2 = 20.0 ms")
   and counter `audio.latency_ms`.
3. Callback: `sync` → `renderToFormat` in 256-frame chunks.

## Optimization
Measured (`tools/audio_bench`, 69 voices, 48 kHz; CPU includes per-callback sync):

| Buffer | Latency (ms) | Audio CPU (% of 1 core) | Voice-select stage | Device buffer RAM |
|---|---|---|---|---|
| 64 × 2 | 2.7 | 8.43 | 0.091 | 0.5 KB |
| 128 × 2 | 5.3 | 7.09 | 0.047 | 1 KB |
| 256 × 2 | 10.7 | 6.57 | 0.027 | 2 KB |
| 480 × 2 | 20.0 | 6.55 | 0.026 | 3.75 KB |
| 1024 × 2 | 42.7 | 6.58 | 0.028 | 8 KB |
| 2048 × 2 | 85.3 | 6.60 | 0.026 | 16 KB |
| 4096 × 2 | 170.7 | 6.74 | 0.029 | 32 KB |

Below 256 frames overhead climbs fast (+29% CPU at 64 frames) and, more importantly in practice,
the deadline shrinks to 1.3 ms: any hiccup becomes a pop. Above 480 there's no CPU gain, just lag.
Δ GPU 0. 256–512 frames is the usual game default; 128 for rhythm games with WASAPI exclusive mode.

## Rebuild
1. Open the device with an explicit period size and count; read back what you actually got.
2. Keep the callback deterministic: pre-allocate, no locks that can block, no syscalls.
3. Process in fixed internal chunks so DSP code doesn't care about the period size.
4. Detect underruns (callback time > period, or OS xrun notifications) and expose them.
5. Verify: set 64 frames and load the CPU (e.g. Max voices 256 + HRTF); you'll hear crackles where
   the stats show callback CPU approaching 100% of the period.

Files: `src/modules/audio/audio_system.cpp` (`openDevice`, `onAudio`), `audio_mixer.cpp` (`renderToFormat`).
