# Phase 3: CI-V, meters and scope spec

Status: draft for review, values filled in from wfview `v3-initial` and libx6100 `contrib/wfview/X6100.rig`
Scope: the CI-V stream, the rig-file-driven command table, polling, decoding of state, meters and scope data into the model. Read-only: phase 3 sets nothing on the rig except scope and data output enable.
Out of scope: controls (phase 4), TX and PTT (phase 5), waterfall rendering (UI, follows the model).
Depends on: phase 1 (CI-V session on the announced port, rig file loaded from the announced model), phase 2 threading.

Reference and timing source: wfview `rigcommander`, `icomcommander`, `cachingqueue`, `rigidentities`, `udpcivdata`, `packettypes.h`, `.rig` files. **Behaviour identical to wfview, no deviations.** All values below are placeholders until taken from the wfview source.

---

## 1. Pipeline

```
udp civ session (phase 1 session code)
  -> civ packet unwrap (per wfview civ data packet)
  -> frame splitter (FE FE .. FD)
  -> address filter (to = our address or broadcast, from = rig address)
  -> command match via rig file table
  -> typed decode (freq, mode, filter, meter, scope, ...)
  -> model update + change notification
```

Outbound:

```
poll queue (priorities from rig file)  ─┐
command queue from UI (phase 4)        ─┼─> frame builder -> civ packet wrap -> udp civ session
scope / data output enable at start    ─┘
```

All of this runs on the network thread.

---

## 2. Transport

- CI-V data packet wrapper as wfview (header, reply byte, data length, send sequence). To verify against `packettypes.h`.
- CI-V stream open and close packets sent as wfview does at stream start and before disconnect.
- One UDP packet may contain a partial frame, one frame, or several frames. The splitter keeps state across packets.
- Sequence, retransmit and keepalive: phase 1 common session code.

---

## 3. Addresses and model detection

| Item | Value |
|---|---|
| rig address | from the Transceiver ID reply (`19 00`), as wfview |
| controller address | as wfview uses |
| broadcast | 0x00 |

Model detection, as wfview (`icomcommander.cpp`, `rigcommander.cpp`):
1. Send Transceiver ID `19 00` to the broadcast address.
2. The reply carries the model ID; the reply's source address becomes the rig address for all further frames.
3. The ID is matched against `CIVAddress` in the `[Rig]` section of every `.rig` file (decimal).
4. Match: load that file. No match: hard error `FAILED(unknown_model)`, session disconnected (phase 1 section 13, deviation 3).

X6100: `CIVAddress=97` (0x61) in `X6100.rig` from libx6100 `contrib/wfview/`. x6100_trx answers the `19 00` broadcast with 0x61 when `civaddr 61` is set on the radio. With the default `civaddr a4` it identifies as an IC-705 and `IC-705.rig` is used.

Frames not addressed to us or broadcast are dropped. Frames from an address other than the rig are dropped.

---

## 4. Rig file as command table

The loaded `.rig` file is the only source of rig-specific knowledge. Nothing rig-specific in C.

Format: QSettings INI, `[General] Version=2.10`, one `[Rig]` section. Scalar keys plus QSettings arrays (`Name\size=n`, `Name\<i>\<Key>=value`, 1-based). Command strings are escaped hex text (`\\x27\\x00`), decoded to bytes by the parser.

Scalar keys used (X6100 values for reference):

| Key | X6100.rig |
|---|---|
| Model | X6100 |
| CIVAddress | 97 |
| NumberOfReceivers | 1 |
| NumberOfVFOs | 2 |
| SpectrumSeqMax | 11 |
| SpectrumAmpMax | 160 |
| SpectrumLenMax | 475 |
| HasSpectrum | true |
| HasTransmit | true |
| HasCommand29 | false |
| HasLAN | false |

Arrays used:

| Array | X6100.rig size | Used for |
|---|---|---|
| Commands | 100 | Type (function name), String, Min, Max, Bytes, Command29, GetCommand, SetCommand, Admin |
| Periodic | 32 | polled commands and their priority |
| Modes | 6 | mode list and codes |
| Filters | 3 | filter names and codes |
| Spans | 6 | scope spans |
| ScopeModes | 4 | center / fixed / ... |
| Meters | 42 | meter calibration points |
| Attenuators | 2 | |
| Preamps | 2 | |
| Inputs | 3 | TX input sources (phase 5) |
| Bands | 17 | band edges, band stack |
| CTCSS, DTCS, Antennas, Roofing | 48, 104, 0, 0 | parsed, unused in v1 |

Command names are the `Type` strings, exactly as in the file (e.g. `Scope Wave Data` = `27 00`, `Scope On/Off` = `27 10`, `Scope Data Output` = `27 11`). The core maps them to its own function IDs with one table, the same names wfview uses in `rigcommander` / `icomcommander`.

Parser: current schema (Version 2.10) only; refuses other versions. Unknown keys inside a known section are ignored (as wfview).

Lookup: a decoded frame is matched by longest prefix of command bytes against the table. Unmatched frames are counted and logged on the debug overlay; never an error.

---

## 5. Polling

As wfview `cachingqueue.cpp`:

Priorities (names as in the rig file `Periodic\<i>\Priority`):

| Name | Value |
|---|---|
| None | 0 |
| Immediate | 1 |
| Highest | 2 |
| High | 3 |
| Medium High | 5 |
| Medium | 7 |
| Medium Low | 11 |
| Low | 19 |
| Lowest | 23 |

Scheduler, one item per tick:
- Tick interval: see below.
- If any Immediate item is queued, send that.
- Otherwise a counter picks the priority: the first of Highest, High, Medium High, Medium, Medium Low, Low, Lowest whose value divides the counter. At Lowest the counter resets to 1. Counter++ each tick.
- From the chosen priority, the most recently queued item is taken. Recurring (periodic) items are put back at the same priority.
- Adding a recurring item also queues one immediate non-recurring read of it (not for Transceiver ID).
- A recurring item is not added twice at the same priority.

Tick interval, as `MainController.cpp`:
- Before rig caps are known: 250 ms.
- After: user polling setting if non-zero; otherwise derived from the baud rate in the capabilities packet (phase 1 section 5.4):
  `ms = max(25, (9600000 / baud) × 20 / 1000)`,
  used as is when `HasFDComms=true`, ×4 otherwise.
- X6100: `HasFDComms=true`, so the tick is 25 ms at typical baud rates.

Periodic list: the `Periodic` array of the rig file (`Command`, `Priority`, `VFO`). X6100.rig: 32 entries (1 Highest, 4 High, 4 Medium High, 13 Medium, 10 Medium Low).

Unsolicited (transceive) frames update the model directly.

---

## 6. Decoded state (model)

Minimum set for phase 3, all optional per rig file:

| Model field | Source |
|---|---|
| frequency VFO A / B, main / sub | frequency commands, 5-byte BCD (byte order per CI-V) |
| mode, data mode | mode commands, mapped via Modes |
| filter | via Filters |
| meters: S, power, SWR, ALC, COMP, Vd, Id | meter commands, raw 0000–0255 BCD, converted via calibration |
| AGC, preamp, attenuator | as rig file |
| TX state | TX status command |
| scope config: mode (center/fixed), span, edges, ref level | scope commands |
| scope row | waveform data (section 8) |

A field the rig file does not define does not exist in the model for that rig. The UI hides it (capability-driven UI, as agreed).

Units in the model: frequency in Hz (uint64), meters in calibrated units plus raw value, scope bins as uint8 amplitude 0..amp max.

---

## 7. Startup sequence

After the CI-V stream is READY:

1. Model detection (section 3): Transceiver ID `19 00`, load the rig file.
2. Read the initial state: every command flagged gettable and periodic, once, in wfview's startup order.
3. Enable scope (`Scope On/Off`) and scope data output (`Scope Data Output`), if `HasSpectrum=true` (as wfview).
4. Start periodic polling (`Periodic` array).
5. Model marks the session "state valid"; the UI shows the operating screen only from here.

On disconnect (phase 1 section 11): disable scope data output first, as wfview does.

---

## 8. Scope data

As wfview `icomscopeframeassembler.cpp`. Payload of `Scope Wave Data` (`27 00`), after the receiver byte:

| Byte | Content |
|---|---|
| 0 | sequence (BCD) |
| 1 | sequence max (BCD) |

Sequence 1 (header):

| Byte | Content |
|---|---|
| 2 | mode (0 = center, else fixed) |
| 3 .. 3+n−1 | frequency A, BCD, n = 5 bytes (6 for model 0xAC at 491/16 bytes) |
| 3+n .. 3+2n−1 | frequency B, BCD |
| 3+2n | out-of-range flag |
| after | bins, only when sequence max is 1 |

- Center mode: A is center, B is half-span: low = A − B, high = A + B. Fixed mode: low = A, high = B.
- Out of range: publish a row of zeros of the expected length.

Sequences 2 .. max: bins from byte 2 on, appended.

Rules:
- Sequence 0, max 0, or sequence > max: discard, reset.
- A later sequence without a valid header, or not the expected next one: discard the partial row, reset.
- Complete row whose length ≠ `SpectrumLenMax`: discard ("incomplete scope frame").
- Only complete, valid rows are published. No half rows.

X6100: IC-705 layout, 11 divisions, `SpectrumLenMax=475`, amplitude 0 .. 160.

Phase 3 does no DSP on scope data. Model publishes per row: timestamp, low Hz, high Hz, length, bins, receiver.

---

## 9. Meter handling

As wfview `rigcommander.cpp` `getMeterCal()`:
- Calibration points per meter from the `Meters` array (`Meter`, `RigVal`, `ActualVal`, `RedLine`).
- Value at or below the first point: that point's value. Above the last point: the last point's value. In between: linear interpolation between the two neighbouring points.
- Raw value kept alongside the calibrated one.
- `RedLine` marks the point from which the UI draws red.
- Peak hold and decay: UI concern.

X6100.rig: 42 meter points (e.g. ALC: 0→0, 120→1, 255→2, red from 255).

---

## 10. Core API additions

```c
//
// Model access (UI thread)
//

int model_snapshot(const struct session *s, struct radio_state *out);
int model_scope_row(const struct session *s, int receiver, struct scope_row *out);

//
// Change notification
//

void model_set_notify(struct session *s, void (*fn)(void *ctx, uint32_t changed), void *ctx);

//
// CI-V stats
//

const struct civ_stats *civ_stats(const struct session *s);
```

`changed` is a bitmask of field groups (frequency, mode, meters, scope, ...). The notify callback runs on the network thread and must only post to the UI thread; it never touches LVGL directly.

Scope rows: single-producer ring of complete rows, the UI reads the newest; dropped rows when the UI is slow are acceptable and counted.

---

## 11. Acceptance for phase 3

Against `wfview --6100`:

1. After CONNECTED, the model shows frequency, mode and filter matching the radio.
2. Tuning on the radio: model frequency follows within one poll interval or immediately when sent unsolicited.
3. Mode and filter changes on the radio follow.
4. S-meter moves with signal; values match what wfview shows for the same signal.
5. Scope rows arrive complete, edges match the radio span; no partial rows.
6. Span and center/fixed changes on the radio reflect in the row edges.
7. A field not in the X6100 rig file is absent from the model.
8. Debug overlay: unmatched frames count, dropped scope rows count.
9. Same results from the SDL build.

Against a 7851 (stereo, dual scope), once available:

10. Main and sub frequency, mode and scope kept separately.

---

## 12. Open points

None on values; all from wfview `v3-initial` and `X6100.rig`.
