// SPDX-License-Identifier: GPL-3.0-or-later
//
// Common logic of one RS-BA1 UDP stream (control, CI-V or audio):
// are-you-there, ready, ping, idle, tracked send, retransmit.
//
// Port of icomUdpBase from wfview v3-initial src/radio/icomudpbase.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//
// No sockets and no clock in here: the caller supplies a send function,
// a wall-clock source for ping stamps, and the monotonic time on every call.
// Single-threaded: all calls come from the session network thread.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/ping_delay.h"
#include "core/proto.h"

#define UDP_STREAM_TXBUF	(PROTO_BUFSIZE + 1)
#define UDP_STREAM_MISSING_CAP	512

enum udp_stream_kind {
	UDP_STREAM_CONTROL,
	UDP_STREAM_CIV,
	UDP_STREAM_AUDIO
};

//
// Events returned by udp_stream_receive() (bitmask)
//

#define UDP_EV_IAMHERE		0x01u
#define UDP_EV_READY		0x02u
#define UDP_EV_PING		0x04u

struct udp_io {

	//
	// Output: send one datagram to the peer, returns 0 on success
	//
	int (*send)(void *ctx, const uint8_t *buf, size_t len);

	//
	// Wall clock: ms since local midnight, used as ping stamp
	//
	uint32_t (*day_ms)(void *ctx);

	void *ctx;
};

struct udp_txbuf_entry {
	uint16_t seq;
	uint16_t len;
	uint8_t data[PROTO_MAX_PACKET];
};

struct udp_missing {
	uint16_t seq;
	uint8_t count;
};

struct udp_stream_stats {
	uint32_t sent;
	uint32_t lost;
	uint32_t retransmitted;
	uint32_t requests_sent;
	uint32_t missing_dropped;
	uint32_t flushes;
};

struct udp_stream {

	//
	// Identity
	//
	enum udp_stream_kind kind;
	struct udp_io io;
	uint32_t my_id;
	uint32_t remote_id;

	//
	// Sequence counters
	//
	uint16_t tx_seq;
	uint16_t ping_seq;
	uint32_t ayt_count;

	//
	// Timers (next fire time in ms, 0 = stopped)
	//
	int64_t t_ayt;
	int64_t t_ping;
	int64_t t_idle;
	int64_t t_retransmit;

	//
	// TX buffer: contiguous seq range tx_first .. tx_first + tx_count - 1
	//
	uint16_t tx_first;
	uint16_t tx_count;
	struct udp_txbuf_entry tx[UDP_STREAM_TXBUF];

	//
	// RX tracking: contiguous seq range rx_first .. rx_last
	//
	bool rx_empty;
	uint16_t rx_first;
	uint16_t rx_last;

	//
	// Missing packets, sorted by seq
	//
	size_t missing_n;
	bool missing_overflow;
	struct udp_missing missing[UDP_STREAM_MISSING_CAP];

	//
	// Delay estimate from peer pings
	//
	struct ping_delay ping_delay;
	int ping_lateness_ms;

	struct udp_stream_stats stats;
};

//
// Lifecycle
//

uint32_t udp_stream_endpoint_id(uint32_t local_ip, uint16_t local_port);
void udp_stream_init(struct udp_stream *s, enum udp_stream_kind kind, const struct udp_io *io,
		uint32_t my_id, int64_t now_ms);
void udp_stream_close(struct udp_stream *s);

//
// Traffic
//

uint32_t udp_stream_receive(struct udp_stream *s, const uint8_t *buf, size_t len, int64_t now_ms);
int udp_stream_send_tracked(struct udp_stream *s, uint8_t *buf, size_t len, int64_t now_ms);
int udp_stream_send_control(struct udp_stream *s, bool tracked, uint8_t type, uint16_t seq, int64_t now_ms);

//
// Timers
//

void udp_stream_tick(struct udp_stream *s, int64_t now_ms);
int64_t udp_stream_next_deadline(const struct udp_stream *s);
void udp_stream_start_ping_idle(struct udp_stream *s, int64_t now_ms);
