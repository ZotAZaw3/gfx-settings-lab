# Streaming buffer

> Menu: **Audio → Streaming → Streaming buffer** (and Debug → Simulate slow disk) · Setting ids: `audio.stream.buffer_ms`, `audio.stream.slow_disk` · Owner module: `audio`
> Values: 50–4000 ms · Presets: Low=250, Medium=500, High=500, Ultra=1000

## Definition
Music, long ambiences and dialogue are too big to keep fully decoded in memory, so they are
**streamed**: read from disk and decoded a little ahead of playback. The streaming buffer is how far
ahead. Too small and a busy or slow drive makes music stutter or drop out; bigger costs memory.
This is the "buffered stream size" in some games' advanced audio options.

## How it works
Each stream has:

- a **decoder thread** that reads the file (WAV/FLAC/MP3 via miniaudio's decoder), converts it to
  float, and pushes it into
- a lock-free **single-producer / single-consumer ring buffer** of `bufferMs × sourceRate` frames
  (two atomic counters, write position and read position, monotonically increasing);
- the **audio thread** pops from the ring in `Mixer::fetch` as the voice's resampler needs frames.

The decoder wakes every 5 ms and decodes in chunks of a quarter of the buffer whenever that much
space is free. Before the first read the ring is pre-filled completely (what a loading screen does).

If the audio thread finds fewer frames than it needs, it fills the gap with silence and counts an
**underrun** (counter `audio.stream_underruns`). The debug option **Simulate slow disk** makes every
read take 20 ms and every 8th read stall 250 ms, like a fragmented HDD shared with texture streaming,
so underruns become visible.

The demo music is generated at start-up, written to a WAV in the temp folder, then streamed back,
so the path is the same as a real soundtrack.

## Breakdown
1. `AudioSystem::addStream` → `StreamSource::open(path, bufferMs, threaded)`; changing the size
   restarts the device and re-opens streams (from the start: a production version would seek to
   the current position).
2. Decoder thread: `decodeInto(chunk)` → ring.
3. Audio thread: `StreamSource::read` inside stage **Resample** (stream voices are never
   virtualized, since the ring must keep draining).

| Resource | Size | Notes |
|---|---|---|
| Ring buffer | `bufferMs × 44 100 × 2 ch × 4 B` | 500 ms ≈ 172 KB |
| Decode scratch | quarter of the ring | |

## Optimization
Measured RAM and CPU (`tools/audio_bench`, one 44.1 kHz stereo stream):

| Value | Audio CPU (% of 1 core) | RAM (KB) |
|---|---|---|
| 100 ms | 6.71 | 2022 |
| 250 ms | 7.08 | 2087 |
| 500 ms | 7.53 | 2194 |
| 1000 ms | 6.74 | 2410 |
| 4000 ms | 6.98 | 3702 |

RAM grows ≈ 431 KB per second of buffer per stereo stream; audio-thread CPU doesn't depend on it
(the differences are run noise). The decoder thread costs ≈ 0.02% of a core for WAV (counter
`audio.stream_cpu_pct`); MP3/Vorbis/Opus decoding costs 10–50× more.

Underruns with the slow-disk simulation, 3 s real time:

| Stream buffer | Underruns |
|---|---|
| 100 ms | 140 |
| 250 ms | 36 |
| 500 ms | 0 |
| 1000 ms | 0 |

The buffer must cover the worst stall of the storage (250 ms here) plus one decode chunk. On NVMe
SSDs 100–250 ms is plenty; HDDs and consoles with shared I/O queues want 500 ms – 2 s. Games with
many simultaneous streams (dialogue + music + ambience layers) multiply the RAM.

## Rebuild
1. One decoder thread (or a job on an I/O thread pool) per stream, or one thread serving all streams.
2. SPSC ring buffer with atomic indices: producer writes then `store(release)`, consumer
   `load(acquire)`; no locks.
3. Pre-fill before playback starts; decode in chunks; prioritize streams with the least buffered.
4. On underrun output silence, never block the audio thread waiting for disk.
5. Seek-and-refill when the buffer size changes or the player skips.
6. Verify: enable slow disk, lower the buffer until the counter climbs and you hear gaps.

Files: `src/modules/audio/audio_stream.*`, `audio_mixer.cpp` (`fetch`), `audio_demo.cpp` (music file).
