// SPDX-License-Identifier: GPL-3.0-or-later
//
// Control stream: login, token, radio capabilities, stream request, status.
//
// Port of icomUdpHandler from wfview v3-initial src/radio/icomudphandler.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//
// No sockets here. The session supplies I/O and receives events.
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/passcode.h"
#include "core/udp_stream.h"

#define CONTROL_MAX_RADIOS	8
#define CONTROL_STATUS_LEN	128
#define CONTROL_AYT_WARN	20

enum control_state {
	CTL_STATE_AYT,
	CTL_STATE_READY_WAIT,
	CTL_STATE_LOGIN,
	CTL_STATE_AUTHENTICATED,
	CTL_STATE_STREAM_REQ,
	CTL_STATE_STREAMS_OPEN,
	CTL_STATE_FAILED,
	CTL_STATE_CLOSED
};

enum control_reason {
	CTL_REASON_NONE,
	CTL_REASON_AUTH,
	CTL_REASON_CONN_FAILED,
	CTL_REASON_RADIO_GONE,
	CTL_REASON_IN_USE
};

struct control_cfg {

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
	uint8_t tx_codec;
	uint32_t rx_sample;
	uint32_t tx_sample;
	uint32_t tx_latency;
};

struct radio_cap {

	//
	// Identity
	//
	uint8_t guid[16];
	uint16_t commoncap;
	uint8_t mac[6];
	char name[33];
	char audio[33];

	//
	// Capabilities
	//
	uint16_t conntype;
	uint8_t civ;
	uint16_t rxsample;
	uint16_t txsample;
	uint32_t baudrate;
	uint16_t capf;
	uint16_t capg;
};

struct control_events {

	//
	// Pick free local ports for the CI-V and audio streams (bind them)
	//
	int (*alloc_ports)(void *ctx, uint16_t *civ_local, uint16_t *audio_local);

	//
	// Status packet accepted: open CI-V and audio towards the announced ports
	//
	void (*open_streams)(void *ctx, uint16_t civ_port, uint16_t audio_port);

	//
	// Close CI-V and audio (radio disconnected, or shutdown)
	//
	void (*close_streams)(void *ctx);

	//
	// Status text changed / error reported
	//
	void (*status)(void *ctx, const char *msg);
	void (*error)(void *ctx, enum control_reason reason, const char *msg);

	//
	// Random 16-bit value for the token request
	//
	uint16_t (*rand16)(void *ctx);

	void *ctx;
};

struct control {

	//
	// Stream and wiring
	//
	struct udp_stream s;
	struct control_events ev;
	struct control_cfg cfg;

	//
	// Protocol state
	//
	enum control_state state;
	enum control_reason reason;
	uint16_t auth_seq;
	uint16_t tok_request;
	uint32_t token;
	bool authenticated;
	bool stream_opened;
	int64_t t_token;
	char connection[17];
	bool high_bandwidth;
	bool ayt_warned;

	//
	// Radios from the capabilities packet
	//
	size_t n_radios;
	struct radio_cap radios[CONTROL_MAX_RADIOS];

	//
	// Selected radio
	//
	bool use_guid;
	uint8_t guid[16];
	uint8_t mac[6];
	char dev_name[33];
	uint8_t civ_id;
	uint16_t rx_sample_rates;
	uint16_t tx_sample_rates;
	uint32_t baudrate;
	uint16_t civ_local;
	uint16_t audio_local;
	uint16_t civ_port;
	uint16_t audio_port;

	char status[CONTROL_STATUS_LEN];
};

//
// Lifecycle
//

void control_init(struct control *c, const struct control_cfg *cfg, const struct udp_io *io,
		const struct control_events *ev, uint32_t my_id, int64_t now_ms);
void control_shutdown(struct control *c, int64_t now_ms);

//
// Traffic and timers
//

void control_receive(struct control *c, const uint8_t *buf, size_t len, int64_t now_ms);
void control_tick(struct control *c, int64_t now_ms);
int64_t control_next_deadline(const struct control *c);
