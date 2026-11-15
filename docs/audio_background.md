# Audio in background

> Menu: **Audio → Background → Audio in background / Background volume** · Setting ids: `audio.background.mode`, `audio.background.duck` · Owner module: `audio`
> Values: Keep playing, Duck volume, Mute, Pause device · Background volume 0–100% · Presets: none

## Definition
What the game's sound does when you alt-tab to another window: keep playing (handy while waiting
in a queue), get quieter, go silent, or stop the audio engine entirely to save CPU and battery.

## How it works
The engine reports window focus each frame (`Engine::windowFocused()`, from GLFW focus events). The
master stage ramps a **background gain** toward its target over 250 ms (no click):

| Mode | Target when unfocused | Device |
|---|---|---|
| Keep playing | 1 | running |
| Duck volume | Background volume (default 20%) | running |
| Mute | 0 | running, still mixing silence |
| Pause device | 0 | **stopped** once the fade reaches 0, restarted on focus |

Mute still runs the whole mixer (voices advance, streams drain), so un-focusing and returning is
seamless. Pause stops the device (`ma_device_stop`), so the audio thread sleeps and costs nothing;
voices are frozen and resume where they were, and the fade-in on return hides the restart.

Some games also pause the game simulation on focus loss; that's a gameplay decision separate from
audio.

## Breakdown
1. Game thread: `setWindowFocused(ctx.engine.windowFocused())`.
2. Audio thread, stage **Master**: `bg = approach(target, n / (0.25 s × rate))`, applied with master volume.
3. Game thread, `AudioSystem::update`: Pause mode stops the device once `backgroundGain ≤ 0.001`,
   restarts it on focus.

## Optimization
Measured (`tools/audio_bench`, window unfocused, 69 voices):

| Value | Audio CPU (% of 1 core) | RAM |
|---|---|---|
| Keep playing | 6.70 | 0 |
| Duck volume | 6.68 | 0 |
| Mute | 6.81 | 0 |
| Pause device | 0.24 (only the 250 ms fade, then 0) | 0 |

Muting saves no CPU at all: the mixer still runs. Only Pause drops audio CPU to zero (plus the OS
audio service stops mixing your stream, which matters on laptops). Δ GPU 0, Δ RAM 0.

## Rebuild
1. Listen for focus events (`WM_ACTIVATEAPP`, `glfwSetWindowFocusCallback`, `NSApplicationDidResignActive`).
2. Ramp a master multiplier over ~250 ms toward the mode's target.
3. For pause: wait for the ramp to finish, then stop the device; restart on focus and ramp up.
4. Don't tie this to the minimized state only; borderless windows lose focus without minimizing.
5. Verify: alt-tab during music; listen for a smooth fade, and check the audio CPU counter in Pause mode.

Files: `src/modules/audio/audio_mixer.cpp` (master stage), `audio_system.cpp` (`update`), `audio_module.cpp`.
