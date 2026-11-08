# Phase 5: TX spec

Status: draft for review, values filled in from wfview `v3-initial`
Scope: PTT, TX audio from the phone microphone, ATU tune, TX safety.
Depends on: phases 1–4.

Reference: wfview `v3-initial`:
- `src/radio/icomudpaudio.cpp` (TX packets)
- `src/radio/icomudphandler.cpp` (TX enable in the stream request)
- `src/audio/audioconverter.cpp` (Opus encoder)
- `src/MainController.cpp` (PTT, PTT timeout, tune)
- `src/radio/icomserver.cpp` (server-side stale clients)
- `X6100.rig`

Behaviour identical to wfview, except the guards listed in section 4 (agreed).

---

## 1. TX enable

As wfview (phase 1 section 5.7):
- The stream request sets `txenable = 1`, TX codec and TX sample rate when the radio's capabilities announce `txsample > 1`.
- `txsample < 2`: TX codec and sample rate forced to 0; no TX path, no PTT shown.
- Opus TX (≥ 0x40) only against a `"WFVIEW"` connection; otherwise forced to 0x04.

Profile setting `enable PTT`, as wfview `EnablePTT` (default on). Off: no PTT and no tune from this client.

---

## 2. TX audio

As `icomudpaudio.cpp`:

- **Mono only.** Icom LAN TX audio is 1 channel; a multi-channel packet is rejected.
- Chunking: encoded audio is split into chunks of at most 1364 bytes, one audio packet each, sent tracked.
- Audio packet header (0x18):

| Offset | Field | Value |
|---|---|---|
| 0x10 | ident | 0x9781 when the chunk is 0xa0 bytes, else 0x0080 |
| 0x12 | sendseq | BE, TX audio counter |
| 0x16 | datalen | BE, chunk length |

- TX levels: peak and RMS ×255 per packet.
- Opus encoder (as `audioconverter.cpp`): `OPUS_APPLICATION_AUDIO`, bitrate 64000, VBR on, signal voice, complexity per wfview setting.
- TX latency (`txbuffer` in the stream request): default 150 ms.

Pipeline in this client:

```
microphone (AAudio input / SDL capture)
  -> capture callback: copy into TX ring
network thread:
  -> read TX ring -> mic gain -> mono -> resample to TX rate (own resampler)
  -> encode (as negotiated) -> chunk (1364) -> audio packets
```

---

## 3. PTT

As `MainController.cpp`:
- PTT on/off: rig file command `Transceiver Status` (`1C 00`), value true/false, queued at priority Immediate.
- PTT on arms the PTT timeout (section 4); repeated PTT-on requests do not extend it.
- PTT off stops the timeout.
- PTT state shown from the model (rig's `Transceiver Status`), not from the button.

UI: hold-to-talk by default; toggle optional per profile.

Preconditions in this client before PTT on: session CONNECTED, state valid, TX path present, `enable PTT`, capture running.

---

## 4. TX safety

| Guard | Source | Action |
|---|---|---|
| PTT timeout | wfview, default on, 300 s, range 30–999 s | PTT off, notice "PTT timeout" |
| link loss (phase 1 LOST, 15 s) | this client (deviation 2) | PTT off locally |
| capture failure / mic lost | this client | PTT off |
| app to background without microphone foreground service | this client | PTT off |
| UI thread stall while held (no heartbeat) | this client | PTT off |
| rig in TX without our PTT | this client | show it; do not unkey |

The PTT timeout replaces the earlier 180 s proposal: wfview already has it, default 300 s.

**Server side:** the wfview server (`icomserver.cpp`) deletes a client after 15 s of silence (`STALE_CONNECTION`), but does not send PTT off. `v3-initial` stays unchanged. When the app dies while keyed, nothing from the client or the server unkeys the rig. The protection is the rig's own TX time-out, the same as for any other TOT-equipped rig behind wfview.

---

## 5. RX during TX

As wfview. Scope and meters keep updating; TX meters (power, SWR, ALC) from phase 3. X6100.rig has no COMP and no Id meter.

---

## 6. ATU

As `MainController.cpp`, rig file command `Tuner/ATU Status` (`1C 01`), queued Immediate, unique:
- value 1 / 0: ATU on / off (phase 4).
- value 2: tune cycle. Transmits; only with `enable PTT`; same link-loss guard as PTT.

---

## 7. Android specifics

- Permission RECORD_AUDIO, requested when TX is first used, not at install.
- Foreground service types: mediaPlayback and microphone.
- Input routing: built-in, wired headset, Bluetooth SCO; SCO also moves output, handled as a device change (phase 2), not a reconnect.
- Input preset: unprocessed where available, so Android AGC and noise suppression do not colour TX audio.

---

## 8. Core API additions

```c
//
// TX (UI thread, non-blocking)
//

int tx_ptt(struct session *s, int on);
int tx_ptt_heartbeat(struct session *s);
int tx_tune(struct session *s);
void tx_set_mic_gain(struct session *s, int percent);
void tx_set_ptt_timeout(struct session *s, int enabled, int seconds);

//
// TX stats
//

const struct tx_stats *tx_stats(const struct session *s);
```

---

## 9. Acceptance for phase 5

Against wfserver on the X6100, into a dummy load:

1. Radio without TX capability: no PTT shown, stream request without TX.
2. Hold PTT: rig keys, modulation from the phone mic, power meter shows output.
3. Release: rig back to RX.
4. PTT timeout set to 30 s: rig unkeys at 30 s, notice shown; holding PTT does not extend it.
5. Network dropped while keyed: client PTT off; the rig keeps transmitting until its own TX time-out (server does not unkey), as with any TOT rig.
6. App killed while keyed: same as 5.
7. ATU tune: tune cycle runs, SWR meter updates.
8. Bluetooth headset: TX and RX via the headset, no reconnect.
9. Opus and LPCM 16 TX both produce clean audio (monitor on a second receiver).
10. Same results from the SDL build.

---

## 10. Decisions

1. App or link loss while keyed: covered by the rig's own TX time-out, as for other TOT rigs. No server change.
