# Phase 2: RX audio spec

Status: draft for review, values filled in from wfview `main`
Scope: receiving the audio stream, decoding, buffering, playback on Android (AAudio) and SDL (Tumbleweed).
Out of scope: TX audio (phase 5), CI-V (phase 3).
Depends on: phase 1 (audio stream on the port from the status packet; codec and sample rate requested in the stream request).

Reference: wfview `main`:
- `src/radio/icomudpaudio.cpp`
- `include/audioconverter.h`, `src/audio/audioconverter.cpp`
- `src/audio/audiohandlerbase.cpp`, `src/audio/audiohandlerqtoutput.cpp`
- `include/pingdelaytracker.h`
- `src/MainController.cpp` (defaults)

Behaviour on the wire and in timing is identical to wfview. Platform output (AAudio / SDL) and the resampler implementation are this client's own (section 9).

---

## 1. Pipeline

```
audio stream (phase 1 session code, seq/retransmit/ping)
  -> late-packet drop (section 4)
  -> payload from offset 0x18
  -> decode per codec (section 3)
  -> mono to L+R (1-channel codecs)
  -> resample to device rate (own resampler)
  -> output buffer, size = latency (section 5)
  -> device callback (AAudio / SDL)
```

From the output buffer on, everything is stereo at device rate.

---

## 2. Audio packet (0x18 header)

| Offset | Field | Notes |
|---|---|---|
| 0x00 | common header | len, type, seq, sentid, rcvdid |
| 0x10 | ident | |
| 0x12 | sendseq | BE |
| 0x14 | unused | |
| 0x16 | datalen | BE |
| 0x18 | payload | codec data |

Receive rules, as wfview:
- Packets of length 0x10: control, handled by the session code.
- Audio: `type != 0x01` and `len >= 0x20`.
- `seq == 0` increments a 16-bit prefix, giving an extended 32-bit sequence `prefix << 16 | seq`.
- The audio stream starts its player on the first audio packet if it is not running (also after the 30 s watchdog stopped it, phase 1 section 7).

---

## 3. Codecs

From `audioconverter.h`:

| ID | Codec | Channels | Sample format after decode |
|---|---|---|---|
| 0x01 | µ-law | 1 | float |
| 0x02 | PCM 8-bit | 1 | unsigned 8-bit |
| 0x04 | PCM 16-bit | 1 | signed 16-bit LE |
| 0x08 | PCM 8-bit | 2 | unsigned 8-bit |
| 0x10 | PCM 16-bit | 2 | signed 16-bit LE |
| 0x20 | µ-law | 2 | float |
| 0x40 | Opus | 1 | float |
| 0x41 | Opus | 2 | float |
| 0x80 | ADPCM | 1 | signed 16-bit |

Decode rules:
- PCM 16: little endian, interleaved L/R for 2 channels.
- PCM 8: unsigned.
- µ-law: bit-exact G.711 to float [-1, 1].
- Opus: libopus decoder at the stream sample rate and channel count; frame size from `opus_packet_get_nb_samples()`; `opus_decode_float()`. A packet with no samples (`-1`) is skipped.
- ADPCM: adpcm-xq by David Bryant (BSD), vendored in `third_party/adpcm/` as wfview does in `src/audio/adpcm/`, licence and copyright notice retained. Supported in v1.

Selection rules, as wfview:
- Default codec: 0x04 (LPCM 16 mono), default RX sample rate 48000 Hz.
- Opus (≥ 0x40) only when the login response connection string is `"WFVIEW"`; otherwise forced to 0x04 (phase 1 section 5.2).
- Codec and sample rate are per profile, sent in the stream request.

X6100: wfserver on the radio; mono. Request 0x04 or 0x40.

---

## 4. Late-packet drop

Two checks, both as wfview:

1. **Network side** (`icomudpaudio.cpp`): the ping lateness (`pingDelay`, measured against the recent best-case delay, phase 1 section 4) is compared with
   `threshold = max(latency + 20, latency × 3 / 2)` ms.
   Lateness above the threshold: drop the packet, count it (`dropped latency packets`), log at most every 2 s.
2. **Output side** (`audiohandlerqtoutput.cpp`): a decoded packet older than `latency × 1.5` ms (arrival to output) is dropped.

---

## 5. Output buffer

As wfview's output handler:

- Buffer size = `latency` ms of device-format audio. Default latency 150 ms, per profile.
- On open: pre-fill half the buffer with silence.
- Underrun: on the next write, first fill up to half the buffer (bounded by free space) with silence, then write the audio. Clears the underrun flag.
- Current latency estimate: `0.8 × previous + 0.2 × (time since last packet + buffered duration)`.
- Levels (peak, RMS ×255) and latency / underrun / overrun flags are published per packet, as wfview's `haveLevels`.

Network side levels: averaged over 4 packets (max peak, mean RMS) for the status display, as `icomudphandler.cpp`.

---

## 6. Threading

| Thread | Owner | Does |
|---|---|---|
| network | core, one per session | UDP streams, timers, retransmit, late drop, decode, resample, buffer write |
| audio callback | AAudio / SDL | buffer read, volume |
| UI | platform | LVGL, user input |

- Network to audio: lock-free SPSC ring holding the output buffer; nothing else shared.
- UI to network: command queue (volume, latency change, later CI-V commands and PTT).
- Network to UI: model change notifications; the UI reads a consistent snapshot.
- The callback does no allocation, no locks, no logging, no syscalls.

---

## 7. Platform output

| Platform | API | Notes |
|---|---|---|
| Android | AAudio, low-latency performance mode, stereo int16 or float | foreground service of type mediaPlayback; reopen the stream on device change (headphones, Bluetooth), keep the ring |
| SDL (Tumbleweed) | SDL audio callback | same ring and callback code |

---

## 8. Controls

- Volume: as wfview `setVolume` (0–255 internally), applied in the callback.
- Latency change at runtime: as wfview `changeLatency`; resizes the buffer target, takes effect on the next write.

---

## 9. Own implementation (not wire-visible)

| Item | wfview | This client |
|---|---|---|
| resampler | `wf_resampler` (speex-derived), quality 4 | own polyphase resampler in C, `core/resample.c` (your decision) |
| output | Qt / PortAudio / RtAudio | AAudio / SDL |
| mono handling | device channel count | always duplicated to L+R |

---

## 10. Core API additions

```c
//
// RX audio
//

int audio_rx_start(struct session *s, const struct audio_dev *d);
int audio_rx_stop(struct session *s);
void audio_rx_set_volume(struct session *s, uint8_t volume);
void audio_rx_set_latency(struct session *s, uint16_t ms);

//
// Audio stats
//

const struct audio_stats *audio_rx_stats(const struct session *s);
```

`struct audio_stats`: peak, RMS, current latency, underrun, overrun, dropped-late counts.

---

## 11. Acceptance for phase 2

Against wfserver on the X6100 (mono):

1. After the streams open, audio plays on both ears with codec 0x04 at 48 kHz.
2. Codec 0x40 (Opus) plays; against a non-wfview server it is forced to 0x04.
3. 30 minutes without audible dropouts on a stable link at 150 ms latency.
4. Delay spike (netem delay on the server side): late packets are dropped, latency returns to target after the spike.
5. Headphones plugged and unplugged on Android: audio continues on the new device, no reconnect.
6. App in the background: audio continues (foreground service).
7. Network dropped: phase 1 LOST after 15 s, audio stops cleanly.
8. Same results from the SDL build.

Against a stereo rig (7851, 7610, 7760), once available:

9. Codecs 0x10 / 0x41: main on L, sub on R, as in wfview.

---

## 12. Open points

None.
