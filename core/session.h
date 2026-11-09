// SPDX-License-Identifier: GPL-3.0-or-later
//
// One connection to one rig behind a wfview server: owns the three UDP
// sockets and the network thread, wires control, CI-V and audio streams.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/control.h"
#include "model/radio_state.h"

#define SESSION_HOST_MAX	256

struct profile {

	//
	// Server
	//
	char host[SESSION_HOST_MAX];
	uint16_t port_control;

	//
	// Credentials
	//
	char username[PASSCODE_MAX + 1];
	char password[PASSCODE_MAX + 1];
	char client_name[9];

	//
	// Audio request
	//
	uint8_t rx_codec;
	uint32_t rx_sample;
	uint16_t rx_latency;
	uint8_t tx_codec;
	uint32_t tx_sample;
	uint16_t tx_latency;

	//
	// Playback device rate (0 = no RX audio pipeline)
	//
	uint32_t dev_rate;

	//
	// Rig files directory and polling interval (0 = derive from baud rate, as wfview)
	//
	char rig_dir[256];
	int poll_ms;

	//
	// TX (phase 5): capture device rate (0 = no TX audio), PTT settings as
	// wfview EnablePTT / PTTTimeoutEnabled / PTTTimeoutSeconds, heartbeat
	// guard for hold-to-talk (0 = off), mic gain in percent
	//
	uint32_t cap_rate;
	bool enable_ptt;
	bool ptt_timeout_enabled;
	int ptt_timeout_s;
	int ptt_heartbeat_ms;
	unsigned int mic_gain;
};

enum session_state {
	SESSION_IDLE,
	SESSION_RESOLVING,
	SESSION_CONNECTING,
	SESSION_CONNECTED,
	SESSION_RADIO_GONE,
	SESSION_LOST,
	SESSION_FAILED,
	SESSION_CLOSED
};

enum session_reason {
	SESSION_REASON_NONE,
	SESSION_REASON_RESOLVE,
	SESSION_REASON_SOCKET,
	SESSION_REASON_AUTH,
	SESSION_REASON_CONN_FAILED,
	SESSION_REASON_IN_USE,
	SESSION_REASON_LINK_LOST,
	SESSION_REASON_UNKNOWN_MODEL
};

struct session_info {

	//
	// State
	//
	enum session_state state;
	enum session_reason reason;
	enum control_state control_state;
	char status[CONTROL_STATUS_LEN];

	//
	// Radio
	//
	char radio_name[33];
	uint8_t civ_addr;
	uint32_t baudrate;
	char connection[17];
	uint16_t civ_port;
	uint16_t audio_port;

	//
	// Counters
	//
	uint32_t ayt_count;
	uint32_t civ_frames;
	uint32_t audio_packets;
	uint32_t audio_dropped_late;
	int audio_lateness_ms;
	int audio_latency_ms;
	uint32_t audio_underruns;
	uint32_t audio_overrun_frames;
	uint32_t audio_decode_errors;
	float audio_peak;
	uint8_t rx_codec;
	uint32_t retransmitted;
	uint32_t requests_sent;

	//
	// Controls
	//
	uint32_t ctl_queued;
	uint32_t ctl_rejected;
	int ctl_last_error;

	//
	// TX (phase 5)
	//
	bool tx_available;
	bool ptt;
	int ptt_remaining_s;
	uint8_t tx_codec;
	uint32_t tx_sample;
	uint32_t tx_packets;
	uint32_t tx_bytes;
	uint32_t tx_blocks;
	uint32_t tx_encode_errors;
	uint32_t tx_overrun_frames;
	float tx_peak;
	float tx_rms;
	uint32_t tx_notices;
	char tx_notice[64];
};

struct session_hooks {

	//
	// Called on the network thread; must only post to another thread
	//
	void (*notify)(void *ctx);
	void (*civ_data)(void *ctx, const uint8_t *civ, size_t len);
	void (*audio_data)(void *ctx, uint32_t seq, const uint8_t *payload, size_t len);
	void (*audio_start)(void *ctx);
	void (*audio_stop)(void *ctx);

	void *ctx;
};

struct session;

//
// Session lifecycle
//

int session_connect(struct session **out, const struct profile *p, const struct session_hooks *h);
void session_disconnect(struct session *s);

//
// State reporting
//

void session_get_info(struct session *s, struct session_info *out);
const char *session_state_name(enum session_state st);
const char *session_reason_name(enum session_reason r);
const char *control_state_name(enum control_state st);

//
// CI-V out (any thread)
//

int session_civ_send(struct session *s, const uint8_t *civ, size_t len);

//
// RX audio (device callback / any thread)
//

void session_audio_read(struct session *s, float *out, size_t frames);
void session_set_volume(struct session *s, uint8_t volume);

//
// Radio state (any thread): consistent snapshots
//

void session_get_state(struct session *s, struct radio_state *out);
bool session_get_scope_row(struct session *s, int receiver, struct radio_scope_row *out);

//
// Controls (any thread, non-blocking). Queued to the network thread; the
// result shows up as a state change. Rejections there are counted in
// session_info.ctl_rejected / ctl_last_error.
//

#define SESSION_ERR_LOCKED	(-10)
#define SESSION_ERR_FULL	(-11)
#define SESSION_ERR_NO_TX	(-12)
#define SESSION_ERR_PTT_DISABLED	(-13)
#define SESSION_ERR_NOT_CONNECTED	(-14)

enum session_ctl_op {
	SESSION_CTL_FREQ,
	SESSION_CTL_TUNE,
	SESSION_CTL_MODE,
	SESSION_CTL_VALUE,
	SESSION_CTL_SPAN,
	SESSION_CTL_GET,
	SESSION_CTL_PTT,
	SESSION_CTL_ATU_TUNE,
	SESSION_CTL_CAPTURE_LOST
};

int session_set_freq(struct session *s, uint8_t vfo, uint64_t hz);
int session_tune(struct session *s, uint8_t vfo, int steps);
int session_set_mode(struct session *s, uint8_t vfo, int mode_num, int data, int filter);
int session_set_value(struct session *s, const char *type, uint8_t receiver, int value);
int session_set_span(struct session *s, uint8_t receiver, uint32_t half_span_hz);
int session_get_value(struct session *s, const char *type, uint8_t receiver);

//
// Client-side settings (any thread)
//

void session_set_step(struct session *s, uint32_t hz);
uint32_t session_get_step(struct session *s);
void session_set_freq_lock(struct session *s, bool locked);
bool session_get_freq_lock(struct session *s);

//
// TX (phase 5, UI thread, non-blocking). PTT on needs a connected session,
// a TX path (radio announced TX, codec usable, capture rate set) and
// enable_ptt. With ptt_heartbeat_ms set, PTT is released when no
// heartbeat arrives within that time while keyed.
//

int tx_ptt(struct session *s, int on);
int tx_ptt_heartbeat(struct session *s);
int tx_tune(struct session *s);
void tx_set_mic_gain(struct session *s, int percent);
void tx_set_ptt_timeout(struct session *s, int enabled, int seconds);

//
// TX capture (capture device callback / platform)
//

void tx_capture_write(struct session *s, const float *in, size_t frames, int channels);
void tx_capture_lost(struct session *s);
