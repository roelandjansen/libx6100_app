# Phase 4: rudimentary controls spec

Status: draft for review, values filled in from wfview `main`
Scope: setting receive-side rig state from the UI: frequency, step, mode, filter, VFO, AGC, preamp, attenuator, scope span and mode, lock. Nothing that keys the transmitter.
Out of scope: PTT, TX audio, ATU tune, anything that transmits (phase 5).
Depends on: phase 3 (command table, model, polling).

Reference: wfview `main` `src/cachingqueue.cpp`, `src/MainController.cpp`, `src/ReceiverController.cpp`, and `X6100.rig`. Behaviour identical to wfview.

---

## 1. Flow

```
UI action
  -> command queue (UI thread to network thread)
  -> lookup in rig file table (must be settable)
  -> value check against rig file range
  -> frame builder -> CI-V stream
  -> confirmation via reply / unsolicited / next poll
  -> model update -> UI
```

A control the rig file does not mark settable does not exist in the UI for that rig.

---

## 2. Control set (phase 4)

All present only when the rig file defines them, matching the X6100 operating screen:

| Control | UI element (X6100 screen) | Notes |
|---|---|---|
| frequency | large readout, drag / encoder / keypad | Hz, set via frequency command |
| tuning step | STEP 9K | client-side only, see section 4 |
| mode | AM / USB / LSB / CW / ... | from Modes |
| filter | FIL1 / FIL2 / FIL3 | from Filters |
| VFO | A / B, swap, equal | as rig file |
| AGC | AGC FAST / MID / SLOW | as rig file |
| preamp / attenuator | as rig file | |
| RF gain, squelch | as rig file | optional in phase 4 |
| scope span | CENTER ±24k | from Spans |
| scope mode | center / fixed | |
| lock | LOCK | client-side only, see section 5 |

AF gain: the rig AF gain is a rig control (as rig file); playback volume is local (phase 2). Both exist, labelled clearly, never conflated.

---

## 3. Queue and coalescing

As wfview:
- UI-originated sets go into the queue at priority **Immediate**, which is served before any periodic poll on the next tick (phase 3 section 5).
- Frequency sets (tuning, keypad, scope tap) use `addUnique`: a new frequency set removes any queued, unsent one for the same function and receiver. Fast tuning sends only the latest frequency.
- Other sets: queued as is (`add`), except where wfview uses `addUnique` (ATU / tuner status, and the controls wfview marks unique).
- Each tick sends one item, so the set rate is bounded by the tick interval (25 ms on the X6100).

---

## 4. Tuning and step

- Step: as wfview `roundFrequency(freq, steps, stepsHz)` on direct sets; step list per profile.
- Relative tuning (drag / encoder): `target = current ± delta`; refused when the current frequency is 0, when the result would go below 0, or when locked.
- Direct entry: checked against the rig file range; out of range is refused in the UI, never sent.
- Scope tap: frequency from the row edges (phase 3 section 8), rounded to the step.

---

## 5. Lock

As wfview `setFrequencyLock`:
- Client-side, and it locks **frequency changes only** (tuning, keypad, scope tap). Mode, filter and other controls stay usable.
- Radio-side changes still update the model.
- Per profile, survives reconnect.

---

## 6. Commanded vs confirmed state

As wfview:
- Frequency: the model is updated with the commanded value immediately (`setFrequencyA(f, false)`), then the set is queued. The next poll or unsolicited frame from the rig overwrites it, so the rig is the truth in the end.
- Other controls: the model follows the rig's reply / next poll.
- The queue updates its cache when a non-immediate item is sent; immediate items update the cache on insert.

---

## 7. Core API additions

```c
//
// Controls (UI thread, non-blocking, queued)
//

int ctl_set_freq(struct session *s, int receiver, uint64_t hz);
int ctl_set_mode(struct session *s, int receiver, int mode_id, int data);
int ctl_set_filter(struct session *s, int receiver, int filter_id);
int ctl_set(struct session *s, const char *cmd_name, int receiver, int64_t value);

//
// Client-side state
//

void ctl_set_step(struct session *s, uint32_t hz);
void ctl_set_freq_lock(struct session *s, int locked);
```

`ctl_set()` is the generic path, by rig file command name. The named functions are thin wrappers for the frequent ones. All return immediately; the result arrives as a model change.

Return values: 0 queued, negative for not settable on this rig, out of range, frequency locked, or not connected.

---

## 8. Acceptance for phase 4

Against `wfview --6100`:

1. Tune by drag and by keypad; the radio follows, the model confirms.
2. Fast drag: the radio ends on the final frequency, no lag backlog.
3. Step change applies; tuning snaps to the new grid.
4. Mode and filter changes from the app reach the radio and show on its own screen.
5. AGC, preamp, attenuator: set from the app, confirmed.
6. Scope span and center/fixed: set from the app, row edges follow.
7. Tap on the scope tunes to that frequency.
8. Lock on: no frequency change reaches the radio; mode and filter still work; radio-side tuning still updates the app.
9. Out-of-range keypad entry refused in the UI.
10. A control missing from the X6100 rig file is not shown.
11. Same results from the SDL build.

---

## 9. Decisions

1. ATU: `Tuner/ATU Status` (`1C 01`) value 0 / 1 switches the ATU off / on in phase 4; value 2 (tune cycle, transmits) is phase 5.
