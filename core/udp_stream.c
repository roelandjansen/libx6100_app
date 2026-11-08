// SPDX-License-Identifier: GPL-3.0-or-later
//
// Common logic of one RS-BA1 UDP stream (control, CI-V or audio).
//
// Port of icomUdpBase from wfview v3-initial src/radio/icomudpbase.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//
// Timer start rules per stream kind, as wfview:
//	control: are-you-there every 500 ms (first after 500 ms); ping + idle
//	         start on I-am-here
//	CI-V:    are-you-there sent at once, then every 500 ms; ping and idle
//	         run from the start
//	audio:   are-you-there sent at once, then every 500 ms; ping runs from
//	         the start; no idle
//	all:     retransmit scan every 100 ms from the start
//

#include "core/udp_stream.h"

#include <string.h>

//
// Endpoint ID from local IPv4 address (host order) and local port
//

uint32_t udp_stream_endpoint_id(uint32_t local_ip, uint16_t local_port)
{
	return ((local_ip >> 8) & 0xffu) << 24 | (local_ip & 0xffu) << 16 | (uint32_t)local_port;
}

//
// Raw datagram out
//

static int stream_write(struct udp_stream *s, const uint8_t *buf, size_t len)
{
	return s->io.send(s->io.ctx, buf, len);
}

//
// Fill a 0x10 control header
//

static void fill_control(const struct udp_stream *s, uint8_t *p, uint8_t type, uint16_t seq)
{
	memset(p, 0, PROTO_CONTROL_SIZE);
	proto_put_le32(p + PROTO_OFF_LEN, PROTO_CONTROL_SIZE);
	proto_put_le16(p + PROTO_OFF_TYPE, type);
	proto_put_le16(p + PROTO_OFF_SEQ, seq);
	proto_put_le32(p + PROTO_OFF_SENTID, s->my_id);
	proto_put_le32(p + PROTO_OFF_RCVDID, s->remote_id);
}

//
// TX buffer lookup by seq, NULL when not held
//

static struct udp_txbuf_entry *txbuf_find(struct udp_stream *s, uint16_t seq)
{
	struct udp_txbuf_entry *e;

	if (s->tx_count == 0)
		return NULL;
	if (seq < s->tx_first || (uint32_t)seq >= (uint32_t)s->tx_first + s->tx_count)
		return NULL;

	e = &s->tx[seq % UDP_STREAM_TXBUF];
	return e->seq == seq ? e : NULL;
}

//
// Missing list: find index of seq or insertion point
//

static size_t missing_pos(const struct udp_stream *s, uint16_t seq, bool *found)
{
	size_t i;

	for (i = 0; i < s->missing_n; i++) {
		if (s->missing[i].seq == seq) {
			*found = true;
			return i;
		}
		if (s->missing[i].seq > seq)
			break;
	}

	*found = false;
	return i;
}

static void missing_insert(struct udp_stream *s, uint16_t seq)
{
	bool found;
	size_t i = missing_pos(s, seq, &found);

	if (found)
		return;

	if (s->missing_n == UDP_STREAM_MISSING_CAP) {
		// More than wfview could ever want to keep; the next scan flushes anyway.
		s->missing_overflow = true;
		return;
	}

	memmove(&s->missing[i + 1], &s->missing[i], (s->missing_n - i) * sizeof(s->missing[0]));
	s->missing[i].seq = seq;
	s->missing[i].count = 0;
	s->missing_n++;
}

static void missing_remove_at(struct udp_stream *s, size_t i)
{
	memmove(&s->missing[i], &s->missing[i + 1], (s->missing_n - i - 1) * sizeof(s->missing[0]));
	s->missing_n--;
}

static void missing_clear(struct udp_stream *s)
{
	s->missing_n = 0;
	s->missing_overflow = false;
}

//
// RX tracking: append seq as new highest, trimming the oldest beyond BUFSIZE
//

static void rx_append(struct udp_stream *s, uint16_t seq)
{
	if ((uint32_t)s->rx_last - s->rx_first + 1 > PROTO_BUFSIZE)
		s->rx_first++;

	s->rx_last = seq;
}

static void rx_reset(struct udp_stream *s, uint16_t seq)
{
	s->rx_empty = false;
	s->rx_first = seq;
	s->rx_last = seq;
}

//
// Track a received data packet (type 0x00, seq != 0, not a ping)
//

static void rx_track(struct udp_stream *s, uint16_t seq)
{
	if (s->rx_empty) {
		rx_reset(s, seq);
		return;
	}

	if (seq < s->rx_first || (int)seq - (int)s->rx_last > PROTO_MAX_MISSING) {
		// Rollover or large gap: start again
		rx_reset(s, seq);
		missing_clear(s);
		return;
	}

	if (seq > s->rx_last) {
		if ((int)seq > (int)s->rx_last + 1) {
			uint16_t f;

			for (f = (uint16_t)(s->rx_last + 1); ; f++) {
				rx_append(s, f);
				if (f == seq)
					break;
				missing_insert(s, f);
			}
		} else {
			rx_append(s, seq);
		}
		return;
	}

	// Inside the tracked range: possibly a late missing packet
	{
		bool found;
		size_t i = missing_pos(s, seq, &found);

		if (found)
			missing_remove_at(s, i);
	}
}

//
// Retransmit requests from the peer
//

static void handle_retransmit_single(struct udp_stream *s, uint16_t seq)
{
	struct udp_txbuf_entry *e = txbuf_find(s, seq);

	s->stats.lost++;
	if (e != NULL) {
		stream_write(s, e->data, e->len);
		s->stats.retransmitted++;
	}
}

static void handle_retransmit_multi(struct udp_stream *s, const uint8_t *buf, size_t len, int64_t now_ms)
{
	size_t i;

	for (i = PROTO_CONTROL_SIZE; i + 1 < len; i += 2) {
		const uint16_t seq = proto_get_le16(buf + i);
		struct udp_txbuf_entry *e = txbuf_find(s, seq);

		if (e == NULL) {
			udp_stream_send_control(s, false, PROTO_TYPE_IDLE, seq, now_ms);
		} else {
			stream_write(s, e->data, e->len);
			s->stats.retransmitted++;
			s->stats.lost++;
		}
	}
}

//
// Ping request or reply
//

static uint32_t handle_ping(struct udp_stream *s, const uint8_t *buf, int64_t now_ms)
{
	const uint8_t reply = buf[PROTO_OFF_PING_REPLY];
	const uint16_t seq = proto_get_le16(buf + PROTO_OFF_SEQ);
	const uint32_t stamp = proto_get_le32(buf + PROTO_OFF_PING_TIME);

	if (reply == 0x00) {
		uint8_t p[PROTO_PING_SIZE];

		s->ping_lateness_ms = ping_delay_update(&s->ping_delay, now_ms, stamp);

		memset(p, 0, sizeof(p));
		proto_put_le32(p + PROTO_OFF_LEN, PROTO_PING_SIZE);
		proto_put_le16(p + PROTO_OFF_TYPE, PROTO_TYPE_PING);
		proto_put_le16(p + PROTO_OFF_SEQ, seq);
		proto_put_le32(p + PROTO_OFF_SENTID, s->my_id);
		proto_put_le32(p + PROTO_OFF_RCVDID, s->remote_id);
		p[PROTO_OFF_PING_REPLY] = 0x01;
		proto_put_le32(p + PROTO_OFF_PING_TIME, stamp);
		stream_write(s, p, sizeof(p));
		return UDP_EV_PING;
	}

	if (reply == 0x01 && seq == s->ping_seq)
		s->ping_seq++;

	return 0;
}

//
// Our own ping
//

static void send_ping(struct udp_stream *s)
{
	uint8_t p[PROTO_PING_SIZE];

	memset(p, 0, sizeof(p));
	proto_put_le32(p + PROTO_OFF_LEN, PROTO_PING_SIZE);
	proto_put_le16(p + PROTO_OFF_TYPE, PROTO_TYPE_PING);
	proto_put_le16(p + PROTO_OFF_SEQ, s->ping_seq);
	proto_put_le32(p + PROTO_OFF_SENTID, s->my_id);
	proto_put_le32(p + PROTO_OFF_RCVDID, s->remote_id);
	proto_put_le32(p + PROTO_OFF_PING_TIME, s->io.day_ms(s->io.ctx));
	stream_write(s, p, sizeof(p));
}

//
// Retransmit scan: request missing packets
//

static void send_retransmit_request(struct udp_stream *s)
{
	uint8_t p[PROTO_CONTROL_SIZE + UDP_STREAM_MISSING_CAP * 4];
	size_t n = 0;
	size_t i = 0;

	if (s->missing_n == 0 && !s->missing_overflow)
		return;

	if (s->missing_overflow || s->missing_n > PROTO_MAX_MISSING) {
		missing_clear(s);
		s->rx_empty = true;
		s->stats.flushes++;
		return;
	}

	while (i < s->missing_n) {
		struct udp_missing *m = &s->missing[i];

		if (m->seq == 0) {
			i++;
			continue;
		}

		if (m->count < PROTO_MAX_RETRANSMIT_REQ) {
			uint8_t *q = p + PROTO_CONTROL_SIZE + n * 4;

			proto_put_le16(q, m->seq);
			proto_put_le16(q + 2, m->seq);
			m->count++;
			n++;
			i++;
		} else {
			missing_remove_at(s, i);
			s->stats.missing_dropped++;
		}
	}

	if (n == 0)
		return;

	if (n == 1) {
		fill_control(s, p, PROTO_TYPE_RETRANSMIT, proto_get_le16(p + PROTO_CONTROL_SIZE));
		stream_write(s, p, PROTO_CONTROL_SIZE);
	} else {
		const size_t total = PROTO_CONTROL_SIZE + n * 4;

		fill_control(s, p, PROTO_TYPE_RETRANSMIT, 0);
		proto_put_le32(p + PROTO_OFF_LEN, (uint32_t)total);
		stream_write(s, p, total);
	}

	s->stats.requests_sent++;
}

//
// Start a stream
//

void udp_stream_init(struct udp_stream *s, enum udp_stream_kind kind, const struct udp_io *io,
		uint32_t my_id, int64_t now_ms)
{
	memset(s, 0, sizeof(*s));
	s->kind = kind;
	s->io = *io;
	s->my_id = my_id;
	s->rx_empty = true;
	ping_delay_reset(&s->ping_delay);

	s->t_retransmit = now_ms + PROTO_RETRANSMIT_PERIOD;
	s->t_ayt = now_ms + PROTO_AREYOUTHERE_PERIOD;

	switch (kind) {
	case UDP_STREAM_CONTROL:
		break;
	case UDP_STREAM_CIV:
		udp_stream_send_control(s, false, PROTO_TYPE_AREYOUTHERE, 0, now_ms);
		s->ayt_count++;
		s->t_ping = now_ms + PROTO_PING_PERIOD;
		s->t_idle = now_ms + PROTO_IDLE_PERIOD;
		break;
	case UDP_STREAM_AUDIO:
		udp_stream_send_control(s, false, PROTO_TYPE_AREYOUTHERE, 0, now_ms);
		s->ayt_count++;
		s->t_ping = now_ms + PROTO_PING_PERIOD;
		break;
	}
}

//
// Stop a stream: disconnect packet, timers off
//

void udp_stream_close(struct udp_stream *s)
{
	udp_stream_send_control(s, false, PROTO_TYPE_DISCONNECT, 0, 0);
	s->t_ayt = 0;
	s->t_ping = 0;
	s->t_idle = 0;
	s->t_retransmit = 0;
}

//
// Start ping and idle (control stream, on I-am-here)
//

void udp_stream_start_ping_idle(struct udp_stream *s, int64_t now_ms)
{
	s->t_ping = now_ms + PROTO_PING_PERIOD;
	s->t_idle = now_ms + PROTO_IDLE_PERIOD;
}

//
// Send a packet with the next sequence number, keep it for retransmission.
// buf must hold the full packet; its seq field is overwritten.
//

int udp_stream_send_tracked(struct udp_stream *s, uint8_t *buf, size_t len, int64_t now_ms)
{
	struct udp_txbuf_entry *e;
	int rc;

	if (len < PROTO_CONTROL_SIZE || len > PROTO_MAX_PACKET)
		return -1;

	proto_put_le16(buf + PROTO_OFF_SEQ, s->tx_seq);

	if (s->tx_seq == 0) {
		s->tx_count = 0;
		s->tx_first = 0;
	}
	if (s->tx_count > PROTO_BUFSIZE) {
		s->tx_first++;
		s->tx_count--;
	}
	if (s->tx_count == 0)
		s->tx_first = s->tx_seq;

	e = &s->tx[s->tx_seq % UDP_STREAM_TXBUF];
	e->seq = s->tx_seq;
	e->len = (uint16_t)len;
	memcpy(e->data, buf, len);
	s->tx_count++;

	s->tx_seq++;

	rc = stream_write(s, buf, len);

	if (s->t_idle != 0)
		s->t_idle = now_ms + PROTO_IDLE_PERIOD;

	s->stats.sent++;
	return rc;
}

//
// Idle and other control packets
//

int udp_stream_send_control(struct udp_stream *s, bool tracked, uint8_t type, uint16_t seq, int64_t now_ms)
{
	uint8_t p[PROTO_CONTROL_SIZE];

	fill_control(s, p, type, seq);

	if (tracked)
		return udp_stream_send_tracked(s, p, sizeof(p), now_ms);

	return stream_write(s, p, sizeof(p));
}

//
// Process one received datagram; returns UDP_EV_* flags.
// The caller handles stream-specific packets after this.
//

uint32_t udp_stream_receive(struct udp_stream *s, const uint8_t *buf, size_t len, int64_t now_ms)
{
	uint32_t ev = 0;
	uint16_t type;
	uint16_t seq;

	if (len < PROTO_CONTROL_SIZE)
		return 0;

	type = proto_get_le16(buf + PROTO_OFF_TYPE);
	seq = proto_get_le16(buf + PROTO_OFF_SEQ);

	if (len == PROTO_CONTROL_SIZE) {
		if (type == PROTO_TYPE_RETRANSMIT && proto_get_le32(buf + PROTO_OFF_LEN) == PROTO_CONTROL_SIZE)
			handle_retransmit_single(s, seq);

		if (type == PROTO_TYPE_IAMHERE) {
			s->ayt_count = 0;
			s->remote_id = proto_get_le32(buf + PROTO_OFF_SENTID);
			if (s->t_ayt != 0) {
				s->t_ayt = 0;
				if (s->kind == UDP_STREAM_CONTROL)
					udp_stream_start_ping_idle(s, now_ms);
			}
			udp_stream_send_control(s, false, PROTO_TYPE_READY, 1, now_ms);
			ev |= UDP_EV_IAMHERE;
		} else if (type == PROTO_TYPE_READY) {
			ev |= UDP_EV_READY;
		}
	} else if (len == PROTO_PING_SIZE && type == PROTO_TYPE_PING) {
		ev |= handle_ping(s, buf, now_ms);
	}

	if (type == PROTO_TYPE_RETRANSMIT && proto_get_le32(buf + PROTO_OFF_LEN) != PROTO_CONTROL_SIZE)
		handle_retransmit_multi(s, buf, len, now_ms);
	else if (proto_get_le32(buf + PROTO_OFF_LEN) != PROTO_PING_SIZE && type == PROTO_TYPE_IDLE && seq != 0)
		rx_track(s, seq);

	return ev;
}

//
// Fire due timers
//

void udp_stream_tick(struct udp_stream *s, int64_t now_ms)
{
	if (s->t_ayt != 0 && now_ms >= s->t_ayt) {
		udp_stream_send_control(s, false, PROTO_TYPE_AREYOUTHERE, 0, now_ms);
		s->ayt_count++;
		s->t_ayt = now_ms + PROTO_AREYOUTHERE_PERIOD;
	}

	if (s->t_ping != 0 && now_ms >= s->t_ping) {
		send_ping(s);
		s->t_ping = now_ms + PROTO_PING_PERIOD;
	}

	if (s->t_idle != 0 && now_ms >= s->t_idle) {
		// The tracked send itself moves t_idle forward
		udp_stream_send_control(s, true, PROTO_TYPE_IDLE, 0, now_ms);
	}

	if (s->t_retransmit != 0 && now_ms >= s->t_retransmit) {
		send_retransmit_request(s);
		s->t_retransmit = now_ms + PROTO_RETRANSMIT_PERIOD;
	}
}

//
// Earliest pending timer, 0 when none
//

int64_t udp_stream_next_deadline(const struct udp_stream *s)
{
	const int64_t t[4] = { s->t_ayt, s->t_ping, s->t_idle, s->t_retransmit };
	int64_t best = 0;
	size_t i;

	for (i = 0; i < 4; i++) {
		if (t[i] != 0 && (best == 0 || t[i] < best))
			best = t[i];
	}

	return best;
}
