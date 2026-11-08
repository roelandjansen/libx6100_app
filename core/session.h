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
	SESSION_REASON_LINK_LOST
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

//
// CI-V out (any thread)
//

int session_civ_send(struct session *s, const uint8_t *civ, size_t len);
