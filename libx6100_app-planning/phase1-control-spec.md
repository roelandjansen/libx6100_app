# Phase 1: control session spec

Status: draft for review, values filled in from wfview `v3-initial`
Scope: connect, login, radio selection, stream request, keepalive, token handling, retransmit, disconnect; opening the CI-V and audio streams.
Out of scope: CI-V content and rig file use (phase 3), audio content (phase 2).

Reference: wfview `v3-initial` (github.com/wf-group/wfview), files:
- `include/packettypes.h`
- `include/icomudpbase.h`, `src/radio/icomudpbase.cpp`
- `src/radio/icomudphandler.cpp`
- `src/radio/icomudpcivdata.cpp`
- `src/radio/icomudpaudio.cpp`
- `src/rigcommander.cpp`, `src/radio/icomcommander.cpp` (model detection)

Behaviour is identical to wfview. The only deviations are listed in section 13.

---

## 1. Inputs

A profile holds:

| Field | Notes |
|---|---|
| host | hostname or IPv4 address, internal or external; resolved to IPv4 once per connect |
| port_control | user-supplied (e.g. 50001, 50091) |
| username | max 16 chars (encoding limit) |
| password | max 16 chars (encoding limit) |
| client name | first 8 chars used, `-wfview` appended, as wfview |

The CI-V and audio ports are announced by the server in the status packet. One control port = one rig.

---

## 2. Constants (from `packettypes.h`)

| Name | Value | Use |
|---|---|---|
| `AREYOUTHERE_PERIOD` | 500 ms | are-you-there repeat |
| `PING_PERIOD` | 500 ms | ping |
| `IDLE_PERIOD` | 100 ms | idle; restarted on every tracked send |
| `RETRANSMIT_PERIOD` | 100 ms | retransmit request scan |
| `WATCHDOG_PERIOD` | 500 ms | CI-V and audio stream watchdog check |
| `TOKEN_RENEWAL` | 60000 ms | token renew |
| `BUFSIZE` | 500 | packets kept in TX buffer and RX seq tracking |
| `MAX_MISSING` | 50 | gap or missing-list limit before reset |
| `STALE_CONNECTION` | 15 s | server-side client timeout |
| `AUDIO_PERIOD` | 20 ms | audio framing |
| `GUIDLEN` | 16 | |

Packet sizes:

| Packet | Size |
|---|---|
| control | 0x10 |
| watchdog | 0x14 |
| ping | 0x15 |
| CI-V / data header | 0x15 + payload |
| open/close | 0x16 |
| audio header | 0x18 + payload |
| retransmit range | 0x18 |
| token | 0x40 |
| status | 0x50 |
| login response | 0x60 |
| login | 0x80 |
| conninfo | 0x90 |
| capabilities | 0x42 + n × 0x66 |

Packets are dispatched **by length**, as wfview does.

Byte order: the common header (`len`, `type`, `seq`, `sentid`, `rcvdid`) is little endian. Fields marked BE below are big endian. Structures are packed (`#pragma pack(1)`).

---

## 3. Common header (0x10)

| Offset | Field | Size |
|---|---|---|
| 0x00 | len | 4 |
| 0x04 | type | 2 |
| 0x06 | seq | 2 |
| 0x08 | sentid | 4 |
| 0x0c | rcvdid | 4 |

| type | Meaning |
|---|---|
| 0x00 | idle / data |
| 0x01 | retransmit request |
| 0x03 | are you there |
| 0x04 | I am here |
| 0x05 | disconnect |
| 0x06 | are you ready / I am ready |
| 0x07 | ping |

### 3.1 Session IDs

Per stream, after binding the local socket:

```
my_id = ((ip >> 8) & 0xff) << 24 | (ip & 0xff) << 16 | (local_port & 0xffff)
```

`ip` is the local IPv4 address as a 32-bit host-order value. `remote_id` is taken from `sentid` of the I-am-here (0x04) packet.

### 3.2 Tracked vs untracked

- Tracked: gets the next `seq` of the stream, is stored in the TX buffer (max `BUFSIZE`, oldest dropped), and restarts the idle timer. Used for idle, login, token, conninfo, CI-V data, open/close, audio.
- Untracked: `seq` set explicitly, not buffered. Used for are-you-there, are-you-ready, disconnect, ping, retransmissions.
- When `seq` wraps to 0, the TX buffer is cleared.

---

## 4. Ping (0x15)

| Offset | Field | Size |
|---|---|---|
| 0x10 | reply | 1 |
| 0x11 | time | 4 |

- Sent every `PING_PERIOD`, untracked, `seq = ping_seq`, `reply = 0`, `time = ms since local midnight`.
- Reply from the peer with `reply = 1` and the same seq: `ping_seq++`.
- A ping request from the peer (`reply = 0`): answer immediately with `reply = 1`, same `seq`, same `time`.

---

## 5. Control stream sequence

```
IDLE
 └─ connect() ──────────► AYT
AYT                          send 0x03 (untracked, seq 0) every 500 ms, no limit
 │                           after 20 attempts: status "Radio not responding", keep trying
 └─ rx 0x04 ────────────► READY_WAIT
                             remote_id = sentid; stop AYT; start ping 500 ms + idle 100 ms
                             send 0x06 (untracked, seq 1)
READY_WAIT
 └─ rx 0x06 ────────────► LOGIN
                             send login (tracked)
LOGIN
 ├─ rx login resp, error 0xfeffffff ─► FAILED(auth)      "Invalid Username/Password"
 └─ rx login resp, tokrequest match ─► AUTHENTICATED
                             token = resp.token; send token 0x02; start token timer 60 s
AUTHENTICATED
 └─ rx capabilities ────► RADIO_LIST
RADIO_LIST
 └─ rx conninfo (per radio, from server) ─► see 5.6
STREAM_REQ                   conninfo request sent (tracked)
 ├─ rx status, error 0xffffffff, not opened ─► FAILED(conn)   "Connection failed, try rebooting the radio"
 ├─ rx status, error 0, disc 1 ─► RADIO_GONE               close CI-V + audio, keep control
 └─ rx status, otherwise ─► STREAMS_OPEN
                             civ_port, audio_port from status (BE); open CI-V + audio streams
CONNECTED
 ├─ token renew every 60 s
 └─ disconnect() ───────► CLOSING
```

wfview has no timeouts on these states. Only are-you-there gives up visibly (status message after 20 attempts) and keeps trying.

### 5.1 Login (0x80)

| Offset | Field | Value |
|---|---|---|
| 0x10 | payloadsize | BE, `0x80 - 0x10` |
| 0x14 | requestreply | 0x01 |
| 0x15 | requesttype | 0x00 |
| 0x16 | innerseq | BE, `auth_seq++` |
| 0x1a | tokrequest | random 16-bit, kept for matching |
| 0x1c | token | 0 |
| 0x40 | username | encoded, 16 bytes |
| 0x50 | password | encoded, 16 bytes |
| 0x60 | name | client name, 16 bytes |

### 5.2 Login response (0x60)

| Offset | Field | Notes |
|---|---|---|
| 0x1a | tokrequest | must match ours |
| 0x1c | token | stored |
| 0x30 | error | 0xfeffffff = invalid username/password |
| 0x40 | connection | 16-byte string |

Connection string handling, as wfview:
- `"WFVIEW"`: Opus allowed.
- anything else (a radio's own LAN): RX or TX codec ≥ 0x40 (Opus) is forced to 0x04 (LPCM 16 mono), with a notice.
- `"FTTH"`: high-bandwidth connection flag.

Ignore a login response with `type == 0x01`.

### 5.3 Token (0x40)

| Offset | Field | Value |
|---|---|---|
| 0x10 | payloadsize | BE, `0x40 - 0x10` |
| 0x14 | requestreply | 0x01 |
| 0x15 | requesttype | magic |
| 0x16 | innerseq | BE, `auth_seq++` |
| 0x1a | tokrequest | ours |
| 0x1c | token | stored token |
| 0x24 | resetcap | BE, 0x0798 |
| 0x30 | response | in replies |

| magic | Meaning |
|---|---|
| 0x01 | remove token (disconnect) |
| 0x02 | confirm token (after login) |
| 0x05 | renew (every 60 s) |

Renew reply (`requesttype 0x05`, `requestreply 0x02`, `type != 0x01`):
- `response == 0`: OK, restart the 60 s timer; if no stream is open yet, send the stream request.
- `response == 0xffffffff`: renewal rejected; take `remote_id`, `tokrequest`, `token` from the reply, mark the stream closed, send the stream request again.

### 5.4 Capabilities (0x42 + n × 0x66)

Recognised by `(len - 0x42) % 0x66 == 0`. Header field `numradios` at 0x40. Each radio entry (0x66):

| Offset | Field | Notes |
|---|---|---|
| 0x00 | guid[16] or {commoncap at 0x07, mac[6] at 0x0a} | `commoncap == 0x8010` means MAC, else GUID |
| 0x10 | name[32] | radio name |
| 0x30 | audio[32] | audio device name |
| 0x50 | conntype | |
| 0x52 | civ | CI-V address |
| 0x53 | rxsample | sample-rate capability |
| 0x55 | txsample | sample-rate capability; `< 2` = no TX |
| 0x5a | baudrate | BE |
| 0x5e | capf | |
| 0x61 | capg | |

### 5.5 Conninfo from server (0x90)

The server sends one per radio after capabilities, and again whenever another client connects or disconnects.

| Offset | Field |
|---|---|
| 0x20 | guid or commoncap/mac (match against capabilities) |
| 0x40 | name[32] |
| 0x60 | busy (4) |
| 0x64 | computer[16] |
| 0x84 | ipaddress (4) |

### 5.6 Radio selection

As wfview, with exactly one radio per control port:
- Not busy: select radio 0 and send the stream request.
- Busy by another computer (ip non-zero, computer name different from ours): status "`<radio>` in use by `<computer>` (`<ip>`)"; answer with an untracked idle carrying the conninfo `seq`. No stream request.
- Already open: ignore.

### 5.7 Stream request (conninfo 0x90, client to server)

Before sending: pick two free local UDP ports (bind to port 0, read the port, close), one for CI-V and one for audio.

| Offset | Field | Value |
|---|---|---|
| 0x10 | payloadsize | BE, `0x90 - 0x10` |
| 0x14 | requestreply | 0x01 |
| 0x15 | requesttype | 0x03 |
| 0x16 | innerseq | BE, `auth_seq++` |
| 0x1a | tokrequest | ours |
| 0x1c | token | stored |
| 0x20 | guid, or commoncap 0x8010 + mac | from the selected radio |
| 0x40 | name | radio name |
| 0x60 | username | encoded |
| 0x70 | rxenable | 1 |
| 0x71 | txenable | 1 if radio txsample > 1, else 0 |
| 0x72 | rxcodec | phase 2 |
| 0x73 | txcodec | phase 5, only if txenable |
| 0x74 | rxsample | BE, Hz |
| 0x78 | txsample | BE, Hz |
| 0x7c | civport | BE, our local CI-V port |
| 0x80 | audioport | BE, our local audio port |
| 0x84 | txbuffer | BE, TX latency (ms) |
| 0x88 | convert | 1 |

If the radio cannot transmit (`txsample < 2`), TX sample rate and codec are forced to 0.

### 5.8 Status (0x50)

| Offset | Field | Notes |
|---|---|---|
| 0x30 | error | 0xffffffff = connection failed |
| 0x40 | disc | 0x01 = radio disconnected |
| 0x42 | civport | BE |
| 0x46 | audioport | BE |

Ignore status with `type == 0x01`.

---

## 6. CI-V stream

Opened on `civ_port` from the status packet, local port as requested.

- On open: send 0x03 (untracked, seq 0) immediately; start ping 500 ms, idle 100 ms, are-you-there 500 ms, watchdog 500 ms.
- rx 0x04: stop are-you-there.
- rx 0x06: `remote_id = sentid`; send open (open/close packet, magic 0x04); repeat the open every 100 ms until CI-V data arrives.
- CI-V data received: stop the open repeat; record receive time.
- Watchdog: no CI-V data for 2 s → start the open repeat again (once per silence period).
- Close: send open/close with magic 0x00.

Open/close packet (0x16):

| Offset | Field | Value |
|---|---|---|
| 0x10 | data | 0x01c0 |
| 0x13 | sendseq | BE, CI-V inner counter |
| 0x15 | magic | 0x04 open, 0x00 close |

CI-V data packet (0x15 + n), sent tracked:

| Offset | Field | Value |
|---|---|---|
| 0x10 | reply | 0xc1 |
| 0x11 | datalen | payload length |
| 0x13 | sendseq | BE, CI-V inner counter (shared with open/close) |
| 0x15 | payload | CI-V frames |

Received data is valid when `datalen + 0x15 == len` and `type != 0x01`; payload is everything from 0x15. Scope splitting (wfview "split waterfall" option): phase 3.

---

## 7. Audio stream

Opened on `audio_port` from the status packet, local port as requested.

- On open: start audio, watchdog 500 ms, are-you-there 500 ms.
- Watchdog: no audio for 30 s → stop the RX and TX audio threads, log. The session is not closed.
- Audio packet layout and codecs: phase 2.

---

## 8. Retransmit (all streams, from `icomudpbase.cpp`)

### 8.1 Receiving a request

- Single (control packet, `type 0x01`, `len 0x10`): `seq` is the requested packet. Found in the TX buffer: resend untracked. Not found: log only.
- Multiple (`type 0x01`, `len != 0x10`): from offset 0x10, a list of 16-bit little-endian seqs. Per seq: found, resend untracked; not found, send an untracked idle carrying that seq.

### 8.2 Tracking received packets

For every packet with `type 0x00`, `seq != 0`, `len != 0x15`:
- RX tracking empty: insert.
- `seq < lowest tracked` or `seq - highest tracked > MAX_MISSING`: treat as rollover; clear RX tracking and the missing list, insert.
- New seq beyond `highest + 1`: insert all intermediate seqs; each one except the received one goes into the missing list with count 0.
- Already tracked and in the missing list: remove from missing (arrived late).
- RX tracking is capped at `BUFSIZE`, oldest dropped.

### 8.3 Requesting (every `RETRANSMIT_PERIOD`)

- Missing list empty: nothing.
- Missing list larger than `MAX_MISSING`: flush missing list and RX tracking.
- Otherwise, per missing seq with count < 4: add it (each seq written twice, as wfview does), count++. Count reaches 4: drop it.
- Exactly one seq: send a control packet `type 0x01`, `seq = missing`.
- More: control header `type 0x01`, `seq 0`, `len = 0x10 + list length`, followed by the list.

---

## 9. Password encoding

`passcode()` from `icomudpbase.h`, applied to username and password, max 16 chars:

```
for i in 0 .. min(len, 16) - 1:
	p = ascii[i] + i
	if p > 126: p = 32 + p % 127
	out[i] = table[p]
```

The 128-entry table is copied verbatim from `icomudpbase.h` into `core/passcode.c`, with the wfview copyright notice.

---

## 10. Model and rig file

Not part of the control stream. As wfview:
- After the CI-V stream is up, the model ID comes from the CI-V Transceiver ID reply (command `0x19 0x00`).
- The ID is matched against `CIVAddress` in the `[Rig]` section of the `.rig` files (decimal; e.g. `IC-785x.rig`: `CIVAddress=142` = 0x8E).
- No match: hard error `FAILED(unknown_model)`, e.g. `unknown model 0x61: no rig file`; the session is disconnected. wfview instead loads `DEFAULT-ICOM.rig`; this is deviation 3 (section 13).

Details: phase 3.

---

## 11. Disconnect

As wfview `shutdown()`:
1. Stop all control-stream timers (token, are-you-there, ping, idle, watchdog, retransmit).
2. If streams are open: close audio, close CI-V (sends open/close magic 0x00, then 0x05 from the stream teardown), send token 0x01.
3. Control: send 0x05 disconnect (untracked, seq 0), once.
4. Close the socket.

---

## 12. Threading and API

The core owns one network thread per session, started by `session_connect()` and joined by `session_disconnect()`. It runs all three UDP streams, timers and retransmit. The UI thread talks to the session through a command queue (in) and model change notifications (out).

```c
//
// Session lifecycle
//

int session_connect(struct session *s, const struct profile *p);
int session_disconnect(struct session *s);

//
// State reporting
//

enum session_state session_state(const struct session *s);
enum session_reason session_reason(const struct session *s);
const char *session_status_text(const struct session *s);
```

---

## 13. Deviations from wfview

| # | Item | wfview v3-initial | This client |
|---|---|---|---|
| 1 | address family | IPv4, falls back to IPv6 | IPv4 only (your decision) |
| 2 | link loss | no detection on the client; only CI-V 2 s and audio 30 s watchdogs | LOST when nothing has been received on any stream for `STALE_CONNECTION` (15 s), wfview's own server value. On LOST: PTT off locally, all streams torn down, reason shown. Required by the TX guards (phase 5) |
| 3 | unknown model | falls back to `DEFAULT-ICOM.rig` | hard error `FAILED(unknown_model)`, disconnect |

Findings that affect other phases:

- **X6100 rig file:** not in `v3-initial` by design; it comes from libx6100 `contrib/` and is dropped into the rig file directory. Zero changes to `v3-initial`.
- **The wfview server does not drop PTT when a client goes stale.** It deletes stale connections after 15 s (`icomserver.cpp`), with no PTT handling in that path. `v3-initial` stays unchanged, so phase 5 test 6 (app killed while keyed) relies on the rig's own TX time-out.
- **Server ping stale check is commented out** in `icomServer::sendPing`; the stale sweep runs elsewhere in the server.

---

## 14. Acceptance for phase 1

Against `wfview --6100` on control port 50091:

1. Connect, login, capabilities, stream request, status: CI-V and audio streams open, CI-V data arrives.
2. Wrong password: FAILED(auth) with "Invalid Username/Password".
3. Wrong control port: are-you-there repeats; after 20 attempts the status shows "Radio not responding" and it keeps trying until the user disconnects.
4. Radio busy by another client: status shows "in use by", no stream request.
5. Session held 30 minutes with token renewals every 60 s.
5a. Network dropped mid-session: LOST after 15 s, PTT off, all streams torn down.
5b. `X6100.rig` removed: `FAILED(unknown_model 0x61)`, session disconnected.
6. Packet loss injected (e.g. `tc netem loss 5%` on the server side): retransmit requests visible, CI-V stays consistent.
7. Disconnect: server shows the client gone; immediate reconnect works.
8. Same results from the SDL build.
