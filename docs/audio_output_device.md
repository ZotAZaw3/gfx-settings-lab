# Output device

> Menu: **Audio → Output → Output device** · Setting id: `audio.output.device` · Owner module: `audio`
> Values: System default, then every playback device the OS reports · Presets: none

## Definition
Chooses which hardware plays the game: speakers, a USB headset, the monitor's HDMI/DisplayPort
audio, a Bluetooth headset, a virtual device used for streaming. "System default" follows whatever
the OS has selected.

## How it works
At start-up the module asks the audio API for the list of playback endpoints
(`enumerateOutputDevices()` → `ma_context_get_devices`). Each entry has a display name and an
opaque id (a WASAPI endpoint string on Windows, a PulseAudio sink name or ALSA hw id on Linux,
a CoreAudio `AudioDeviceID` on macOS). Selecting one closes the current device and opens the new
one with that id.

When opened, the device reports its *native* format (channels, sample rate, sample type). The
mixer renders in the format you asked for and miniaudio converts to native if they differ, so the
"Format" line in the Audio stats window shows both.

If the chosen device fails to open, the code falls back to the default device with float stereo;
if no backend works at all (a server without a sound card, CI), it uses the **null backend**, which
consumes audio at real-time speed without playing it, so the rest of the app and every measurement
still work.

"System default" is not the same as picking the device that is currently default: with the default
id, WASAPI and PulseAudio move the stream automatically when the user switches outputs in the OS;
with a fixed id, the game stays on that device.

## Breakdown
1. `registerSettings`: enumerate devices once, build the enum options.
2. On change: `AudioSystem::applySettings` → `closeDevice()` (`ma_device_uninit` stops and joins
   the audio thread) → `openDevice()` with `cfg.playback.pDeviceID`.
3. `Mixer::configure` re-allocates buffers for the new rate/channels; playing voices continue (they
   are re-promoted to real voices on the next block).

## Optimization
- Switching costs one device restart (≈50–150 ms of silence on real hardware; ~1 ms on the null
  backend). Steady-state CPU and RAM do not depend on which device is chosen, only on the format it
  forces (see [sample rate & bit depth](audio_sample_rate_bit_depth.md), [speaker config](audio_speaker_config.md)).
- Bluetooth devices add 100–250 ms of latency of their own that no game setting can remove.
- HDMI outputs often run at 48 kHz 8-channel natively; asking for stereo means the OS up-mixes.

Measured: Δ CPU 0, Δ RAM 0 between devices (device buffer bytes follow the buffer-size setting, see
[audio_buffer_latency](audio_buffer_latency.md)). Δ GPU 0.

## Rebuild
1. Enumerate endpoints: Windows `IMMDeviceEnumerator::EnumAudioEndpoints(eRender)`, macOS
   `kAudioHardwarePropertyDevices`, Linux PulseAudio `pa_context_get_sink_info_list`.
2. Persist the device **id**, not the index (indices change when devices are plugged in), and fall
   back to default if the id is missing next launch. (This prototype stores the index for
   simplicity.)
3. Handle hot-plug: register for device-change notifications (`IMMNotificationClient`,
   miniaudio's `notificationCallback`) and reopen on "device lost".
4. Never open the device on the game thread inside a frame you care about; it can take 100+ ms.

Files: `src/modules/audio/audio_system.cpp` (`init`, `openDevice`, `enumerateOutputDevices`).
