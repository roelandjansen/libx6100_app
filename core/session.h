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
